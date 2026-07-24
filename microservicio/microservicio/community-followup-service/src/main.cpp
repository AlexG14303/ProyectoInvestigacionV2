#include <csignal>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <omp.h>

#include "httplib.h"
#include "json.hpp"
#include "HttpHelpers.hpp"
#include "PostgRestClient.hpp"

extern "C" {
#include "controllers/followup-controller.h"
#include "database/followup-query-builder.h"
#include "events/followup-event-publisher.h"
#include "repositories/followup-repository.h"
#include "services/followup-service.h"
#include "validators/followup-validator.h"
}

#include "workflow-engine/workflow-manager.h"
#include "workflow-engine/followup-timeline.h"
#include "compliance-engine/compliance-evaluator.h"
#include "compliance-engine/noncompliance-analyzer.h"

#include "domain/followup_analyzer.h"
#include "domain/followup_evaluation_engine.h"
#include "domain/followup_validator.h"
#include "parallel/followup_batch_processor.h"
#include "parallel/parallel_config.h"

using nlohmann::json;

static const char* env(const char* key, const char* def) {
    const char* v = std::getenv(key);
    return v ? v : def;
}

static std::string todayIsoDate() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d");
    return out.str();
}

extern "C" void handleShutdownSignal(int) {
    std::exit(0);
}

static void jsonErrorResponse(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content(json({{"error", message}}).dump(), "application/json");
}

static void handle(httplib::Response& res, const std::function<void()>& fn) {
    try {
        fn();
    } catch (const json::parse_error&) {
        jsonErrorResponse(res, 400, "JSON inválido");
    } catch (const std::invalid_argument& e) {
        jsonErrorResponse(res, 400, e.what());
    } catch (const std::runtime_error& e) {
        std::cerr << "[community-followup-service] Error interno (runtime_error): " << e.what() << std::endl;
        jsonErrorResponse(res, 400, "No se pudo procesar la solicitud. Verifique los datos enviados.");
    } catch (const std::exception& e) {
        std::cerr << "[community-followup-service] Error interno (exception): " << e.what() << std::endl;
        jsonErrorResponse(res, 500, "Error interno del servidor.");
    }
}

// -----------------------------------------------------------------------------
// Endpoint individual (POST/PATCH /followups...). Se mantiene deliberadamente
// simple y SECUENCIAL: valida -> inserta/actualiza -> responde. Una sola
// petición con ~12 campos no genera trabajo de CPU suficiente para justificar
// una región paralela (esto ya se midió empíricamente: el overhead de crear
// un equipo de hilos OpenMP supera el costo de validar un registro). El
// paralelismo real vive en los endpoints de lote, más abajo.
// -----------------------------------------------------------------------------
static json cleanFollowupPayload(const json& input, bool partialUpdate = false) {
    return cf::domain::cleanAndValidateFollowup(input, partialUpdate);
}

static std::string buildListQuery(const httplib::Request& req) {
    char query[512] = {0};
    cf_build_followup_list_query(
        req.has_param("family_id") ? req.get_param_value("family_id").c_str() : nullptr,
        req.has_param("risk_assessment_id") ? req.get_param_value("risk_assessment_id").c_str() : nullptr,
        req.has_param("compliance_status") ? req.get_param_value("compliance_status").c_str() : nullptr,
        query,
        sizeof(query));
    return std::string(query);
}

static std::string idFilter(int id) {
    char filter[96] = {0};
    cf_build_followup_id_filter(id, filter, sizeof(filter));
    return std::string(filter);
}

static json eventFromCModule(const std::string& eventName, const json& item) {
    char buffer[1024] = {0};
    cf_build_followup_event_json(
        eventName.c_str(),
        item.value("followup_id", 0),
        item.value("family_id", 0),
        item.value("record_number", "").c_str(),
        item.value("compliance_status", "").c_str(),
        buffer,
        sizeof(buffer));
    return json::parse(buffer);
}

static void registerRoutes(httplib::Server& svr, const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table, const std::string& basePath) {
    svr.Get(basePath.c_str(), [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            jsonOk(res, pgClient->getAll(table, buildListQuery(req)));
        });
    });

    svr.Get((basePath + R"(/(\d+))").c_str(), [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            int id = std::stoi(req.matches[1]);
            auto item = pgClient->getOne(table, idFilter(id));
            if (item.is_null()) {
                jsonErrorResponse(res, 404, "Seguimiento no encontrado");
                return;
            }
            jsonOk(res, item);
        });
    });

    svr.Post(basePath.c_str(), [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            // 1) Validación básica (secuencial, simple).
            auto data = cleanFollowupPayload(parseBody(req));

            // 2) Evaluación interna paralela con OpenMP — ANTES de insertar.
            //    Calcula riesgo, cumplimiento, prioridad, alertas y
            //    recomendaciones a partir de las 80 reglas del motor. No
            //    hace ninguna llamada de red ni de base de datos.
            auto evaluation = cf::domain::evaluateFollowup(data);

            // 3) UNA sola inserción en base de datos. El payload insertado
            //    conserva exactamente las mismas columnas que antes (no se
            //    persisten los campos calculados: la tabla actual no tiene
            //    columnas para ellos — ver README_OPENMP_SINGLE_INSERT.md,
            //    sección "qué se guarda y qué solo se retorna").
            auto created = pgClient->insert(table, data);

            // Fix: insert() SIEMPRE devuelve un objeto (nunca un arreglo:
            // ya sea que PostgREST responda con un arreglo de 1 elemento,
            // insert() lo desempaqueta con result[0]). El chequeo anterior
            // (`created.is_array()`) nunca era verdadero, así que aps_event
            // jamás se agregaba aunque el código pareciera hacerlo.
            if (created.is_object()) {
                created["aps_event"] = eventFromCModule("followup.created", created);
            }

            // 4) Respuesta: el registro creado + resultados de la
            //    evaluación + métricas de paralelismo.
            if (created.is_object()) {
                created["risk_score"] = evaluation.riskScore;
                created["risk_level"] = evaluation.riskLevel;
                created["compliance_score"] = evaluation.complianceScore;
                created["priority"] = evaluation.priority;
                created["alerts"] = evaluation.alerts;
                created["recommendations"] = evaluation.recommendations;
                created["parallel_metrics"] = {
                    {"parallel_enabled", evaluation.parallelEnabled},
                    {"threads_used", evaluation.threadsUsed},
                    {"rules_evaluated", evaluation.rulesEvaluated},
                    {"rules_triggered", evaluation.rulesTriggered},
                    {"processing_time_ms", evaluation.processingTimeMs}
                };
            }
            jsonCreated(res, created);
        });
    });

    svr.Patch((basePath + R"(/(\d+))").c_str(), [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            int id = std::stoi(req.matches[1]);
            auto data = cleanFollowupPayload(parseBody(req), true);
            bool ok = pgClient->update(table, idFilter(id), data);
            if (!ok) throw std::runtime_error("No se pudo actualizar el seguimiento");
            auto item = pgClient->getOne(table, idFilter(id));
            if (!item.is_null()) {
                item["timeline_state"] = cf_timeline_state_label(item.value("compliance_status", ""));
                item["aps_event"] = eventFromCModule("followup.updated", item);
            }
            jsonOk(res, item.is_null() ? json({{"mensaje", "Seguimiento actualizado"}}) : item);
        });
    });

    svr.Post((basePath + R"(/(\d+)/complete)").c_str(), [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            int id = std::stoi(req.matches[1]);
            json body = req.body.empty() ? json::object() : parseBody(req);

            const std::string requestedStatus = body.value("compliance_status", "");
            const std::string workflowStatus = cf_workflow_completion_status(requestedStatus);
            char preparedStatus[32] = {0};
            char error[160] = {0};
            if (!cf_prepare_compliance_status(workflowStatus.c_str(), preparedStatus, sizeof(preparedStatus), error, sizeof(error))) {
                throw std::invalid_argument(error);
            }

            json data = json::object();
            data["compliance_status"] = preparedStatus;
            data["evaluation_date"] = body.value("evaluation_date", todayIsoDate());
            if (body.contains("noncompliance_causes")) {
                data["noncompliance_causes"] = body["noncompliance_causes"];
            } else if (cf_is_noncompliance_status(preparedStatus)) {
                data["noncompliance_causes"] = cf_noncompliance_default_cause("");
            }
            cf::domain::validateIsoDateField(data, "evaluation_date");

            bool ok = pgClient->update(table, idFilter(id), data);
            if (!ok) throw std::runtime_error("No se pudo completar el seguimiento");
            auto item = pgClient->getOne(table, idFilter(id));
            if (!item.is_null()) {
                item["timeline_state"] = cf_timeline_state_label(item.value("compliance_status", ""));
                item["aps_event"] = eventFromCModule(cf_workflow_completion_event_name(), item);
            }
            jsonOk(res, item.is_null() ? json({{"mensaje", "Seguimiento completado"}}) : item);
        });
    });

// Vaciar completamente la tabla de seguimientos
svr.Delete((basePath + "/clear").c_str(),
    [pgClient, table](const httplib::Request&, httplib::Response& res) {
        handle(res, [&]() {

            // followup_id es la clave primaria y nunca es NULL,
            // por lo que este filtro selecciona todos los registros.
            bool ok = pgClient->remove(
                table,
                "followup_id=not.is.null"
            );

            if (!ok) {
                throw std::runtime_error(
                    "No se pudo vaciar la tabla de seguimientos"
                );
            }

            jsonOk(res, {
                {"mensaje", "Tabla de seguimientos vaciada correctamente"}
            });
        });
    });

    svr.Delete((basePath + R"(/(\d+))").c_str(), [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            int id = std::stoi(req.matches[1]);
            bool ok = pgClient->remove(table, idFilter(id));
            if (!ok) throw std::runtime_error("No se pudo eliminar el seguimiento");
            jsonOk(res, {{"mensaje", "Seguimiento eliminado"}, {"followup_id", id}});
        });
    });
}

// -----------------------------------------------------------------------------
// Helpers para dar forma a las respuestas de los endpoints de lote/analítica.
// -----------------------------------------------------------------------------
static json riskSummaryJson(long alto, long medio, long bajo) {
    return json{{"alto", alto}, {"medio", medio}, {"bajo", bajo}};
}

static double recordsPerSecond(long records, double ms) {
    if (ms <= 0.0) return 0.0;
    return static_cast<double>(records) / (ms / 1000.0);
}

static std::vector<json> extractRecordsArray(const json& body) {
    if (!body.contains("records") || !body["records"].is_array()) {
        throw std::invalid_argument("Se espera un campo 'records' con un arreglo de seguimientos");
    }
    const auto& arr = body["records"];
    return std::vector<json>(arr.begin(), arr.end());
}

static json buildBatchValidateResponse(const cf::parallel::BatchProcessResult& result) {
    json sampleErrors = json::array();
    int shown = 0;
    for (const auto& item : result.items) {
        if (!item.valid) {
            sampleErrors.push_back(item.error);
            if (++shown >= 10) break;  // no inflar la respuesta con miles de errores
        }
    }

    const auto& cfg = cf::parallel::config();
    return json{
        {"records_received", result.recordsReceived},
        {"records_valid", result.recordsValid},
        {"records_invalid", result.recordsInvalid},
        {"risk_summary", riskSummaryJson(result.riskAlto, result.riskMedio, result.riskBajo)},
        {"parallel_enabled", result.parallelEnabled},
        {"threads_used", result.threadsUsed},
        {"parallelism_label", cfg.parallelismLabel},
        {"processing_time_ms", result.processingTimeMs},
        {"records_per_second", recordsPerSecond(result.recordsReceived, result.processingTimeMs)},
        {"sample_errors", sampleErrors}};
}

int main() {
    try{
    const std::string pgHost = env("POSTGREST_HOST", "localhost");
    const int pgPort = std::atoi(env("POSTGREST_PORT", "3000"));
    const int port = std::atoi(env("PORT", "8084"));
    const std::string table = env("FOLLOWUP_TABLE", cf_followup_table_name());
    const auto& parallelCfg = cf::parallel::config();

    auto pgClient = std::make_shared<PostgRestClient>(pgHost, pgPort);
    httplib::Server svr;

    // BACKEND_THREADS: concurrencia HTTP (independiente de OpenMP).
    svr.new_task_queue = [threads = parallelCfg.backendThreads] {
        return new httplib::ThreadPool(static_cast<size_t>(threads));
    };

    svr.Get("/health", [pgHost, pgPort, table, &parallelCfg](const httplib::Request&, httplib::Response& res) {
        jsonOk(res, {
            {"status", "ok"},
            {"service", "community-followup-service"},
            {"architecture", "hibrida C/C++ con OpenMP — evaluacion paralela por seguimiento, insercion unica"},
            {"table", table},
            {"postgrest", pgHost + ":" + std::to_string(pgPort)},
            {"openmp", {
                {"enabled", true},
                {"version", _OPENMP},
                {"omp_num_threads_configured", parallelCfg.ompNumThreads},
                {"omp_max_threads_runtime", omp_get_max_threads()},
                {"dynamic", static_cast<bool>(omp_get_dynamic())},
                {"max_active_levels", parallelCfg.ompMaxActiveLevels},
                {"proc_bind", parallelCfg.ompProcBind},
                {"places", parallelCfg.ompPlaces},
                {"parallelism_label", parallelCfg.parallelismLabel}
            }},
            {"backend_threads", parallelCfg.backendThreads},
            {"evaluation_engine", {
                {"enabled", true},
                {"rules_configured", cf::domain::totalRuleCount()},
                {"mode", "single_insert_parallel_evaluation"}
            }},
            {"batch_endpoints", {
                {"note", "disponibles como herramientas secundarias; NO son la prueba principal"},
                {"parallel_threshold", parallelCfg.minParallelBatchSize},
                {"persist_chunk_size", parallelCfg.persistChunkSize}
            }}
        });
    });

    svr.Get(cf_compliance_alerts_path(), [pgClient, table](const httplib::Request&, httplib::Response& res) {
        handle(res, [&]() {
            jsonOk(res, pgClient->getAll(table, cf_compliance_alert_filter()));
        });
    });

    // -------------------------------------------------------------------
    // A. POST /followups/batch/validate — mide CPU puro con OpenMP.
    // Valida + normaliza + clasifica un lote completo; NO inserta nada.
    // -------------------------------------------------------------------
    svr.Post("/followups/batch/validate", [](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            json body = parseBody(req);
            auto raw = extractRecordsArray(body);
            auto result = cf::parallel::processBatchCpuPhase(raw, false);
            jsonOk(res, buildBatchValidateResponse(result));
        });
    });

    // -------------------------------------------------------------------
    // B. POST /followups/batch — ingreso por lotes de verdad.
    // Fase 2 (paralela, CPU): validar/normalizar/clasificar.
    // Fase 3 (secuencial, controlada): insertar en PostgREST en bloques
    // (PERSIST_CHUNK_SIZE), FUERA de la región OpenMP.
    // -------------------------------------------------------------------
    svr.Post("/followups/batch", [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            json body = parseBody(req);
            auto raw = extractRecordsArray(body);

            auto result = cf::parallel::processBatchCpuPhase(raw, false);

            const auto& cfg = cf::parallel::config();
            const auto tPersistStart = std::chrono::steady_clock::now();

            json validRecords = json::array();
            for (auto& item : result.items) {
                if (item.valid) validRecords.push_back(item.normalized);
            }

            long inserted = 0;
            json insertErrors = json::array();
            const std::size_t chunkSize = static_cast<std::size_t>(std::max(1, cfg.persistChunkSize));

            for (std::size_t start = 0; start < validRecords.size(); start += chunkSize) {
                const std::size_t end = std::min(start + chunkSize, validRecords.size());
                json chunk = json::array();
                for (std::size_t k = start; k < end; ++k) chunk.push_back(validRecords[k]);

                try {
                    auto insertedChunk = pgClient->insertMany(table, chunk);
                    if (insertedChunk.is_array()) {
                        inserted += static_cast<long>(insertedChunk.size());
                    } else {
                        insertErrors.push_back("Respuesta inesperada insertando el bloque [" +
                                                std::to_string(start) + ".." + std::to_string(end) + ")");
                    }
                } catch (const std::exception& e) {
                    insertErrors.push_back("Bloque [" + std::to_string(start) + ".." + std::to_string(end) +
                                            "): " + e.what());
                }
            }

            const auto tPersistEnd = std::chrono::steady_clock::now();
            const double persistMs = std::chrono::duration<double, std::milli>(tPersistEnd - tPersistStart).count();
            const double totalMs = result.processingTimeMs + persistMs;

            jsonOk(res, {
                {"records_received", result.recordsReceived},
                {"records_valid", result.recordsValid},
                {"records_invalid", result.recordsInvalid},
                {"records_inserted", inserted},
                {"risk_summary", riskSummaryJson(result.riskAlto, result.riskMedio, result.riskBajo)},
                {"parallel_enabled", result.parallelEnabled},
                {"threads_used", result.threadsUsed},
                {"parallelism_label", cfg.parallelismLabel},
                {"cpu_phase_time_ms", result.processingTimeMs},
                {"persist_phase_time_ms", persistMs},
                {"total_time_ms", totalMs},
                {"records_per_second", recordsPerSecond(result.recordsReceived, totalMs)},
                {"insert_errors", insertErrors}
            });
        });
    });

    // -------------------------------------------------------------------
    // C. GET /followups/analytics/summary — resumen con reduction OpenMP.
    // ?simulate=N genera N registros SOLO en memoria (no toca la base de
    // datos); sin ese parametro, trae todos los registros reales via
    // PostgREST (una sola llamada de red) y calcula el resumen en memoria.
    // -------------------------------------------------------------------
    svr.Get("/followups/analytics/summary", [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handle(res, [&]() {
            std::vector<json> records;
            bool simulated = false;

            if (req.has_param("simulate")) {
                const int n = std::max(0, std::atoi(req.get_param_value("simulate").c_str()));
                records = cf::parallel::generateSyntheticRecords(n);
                simulated = true;
            } else {
                auto all = pgClient->getAll(table, "");
                if (all.is_array()) records.assign(all.begin(), all.end());
            }

            auto summary = cf::parallel::computeAnalyticsSummary(records);
            const auto& cfg = cf::parallel::config();

            jsonOk(res, {
                {"total_followups", summary.total},
                {"completed_followups", summary.completed},
                {"noncompliance_alerts", summary.nonCompliant},
                {"partial_followups", summary.partial},
                {"compliance_percentage", summary.compliancePercentage},
                {"risk_summary", riskSummaryJson(summary.riskAlto, summary.riskMedio, summary.riskBajo)},
                {"parallel_enabled", summary.parallelEnabled},
                {"threads_used", summary.threadsUsed},
                {"parallelism_label", cfg.parallelismLabel},
                {"processing_time_ms", summary.processingTimeMs},
                {"records_per_second", recordsPerSecond(summary.total, summary.processingTimeMs)},
                {"simulated", simulated},
                {"source_record_count", static_cast<long>(records.size())}
            });
        });
    });

    registerRoutes(svr, pgClient, table, cf_followup_base_path());
    registerRoutes(svr, pgClient, table, cf_followup_legacy_path());

    std::cout << "[community-followup-service] Arquitectura hibrida C/C++ con OpenMP activa\n";
    std::cout << "[community-followup-service] Tabla PostgREST: " << table << "\n";
    std::cout << "[community-followup-service] OMP_NUM_THREADS=" << parallelCfg.ompNumThreads
              << "  BACKEND_THREADS=" << parallelCfg.backendThreads
              << "  MIN_PARALLEL_BATCH_SIZE=" << parallelCfg.minParallelBatchSize
              << "  label=" << parallelCfg.parallelismLabel << "\n";
    std::cout << "[community-followup-service] Escuchando en :" << port << "\n";
    std::signal(SIGTERM, handleShutdownSignal);
    std::signal(SIGINT, handleShutdownSignal);
    svr.listen("0.0.0.0", port);

    return 0;
    } catch (const std::exception& e) {
        std::cerr << "[community-followup-service] Error fatal no controlado: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "[community-followup-service] Error fatal desconocido no controlado" << std::endl;
        return 1;
    }
}
