#include <cassert>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
#include "controllers/compliance-controller.h"
#include "controllers/followup-controller.h"
#include "database/db-connection.h"
#include "database/followup-query-builder.h"
#include "events/followup-event-publisher.h"
#include "repositories/compliance-repository.h"
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
    // Fix S6229 ("usar std::chrono en vez de time"): conversión válida
    // desde C++11, sin depender de features de C++20+.
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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

// Fix S5271: siempre termina el proceso (std::exit nunca retorna).
extern "C" [[noreturn]] void handleShutdownSignal(int) {
    std::exit(0);
}

static void jsonErrorResponse(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content(json({{"error", message}}).dump(), "application/json");
}

// Se convierte a template en vez de usar std::function<void()>: evita el
// overhead de type erasure (asignación dinámica interna, indirección de
// llamada) ya que siempre se invoca con lambdas conocidas en tiempo de
// compilación. El compilador puede inlinear el cuerpo de "fn" directamente.
template <typename Fn>
static void handle(httplib::Response& res, Fn&& fn) {
    // NOSONAR (cpp:S1181 x2, "capturar una excepción más específica"):
    // esta es una jerarquía de catches DELIBERADA, de más a menos
    // específica, donde cada nivel mapea a un status HTTP distinto:
    // json::parse_error/invalid_argument -> 400 con el mensaje tal cual;
    // runtime_error -> 400 genérico (no se expone el detalle interno);
    // cualquier otro std::exception -> 500. Achicar el segundo o tercer
    // catch a un tipo más específico rompería esa clasificación por
    // capas, que es justamente el propósito de esta función.
    try {
        fn();
    } catch (const json::parse_error&) {
        jsonErrorResponse(res, 400, "JSON inválido");
    } catch (const std::invalid_argument& e) {
        jsonErrorResponse(res, 400, e.what());
    } catch (const std::runtime_error& e) {  // NOSONAR
        std::cerr << "[community-followup-service] Error interno (runtime_error): " << e.what() << std::endl;
        jsonErrorResponse(res, 400, "No se pudo procesar la solicitud. Verifique los datos enviados.");
    } catch (const std::exception& e) {  // NOSONAR
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
    // Fix S5945: std::string en vez de char[512], usando .data()/.size()
    // (C++17) para pasarlo a la función C, recortado al contenido real.
    std::string query(512, '\0');
    cf_build_followup_list_query(
        req.has_param("family_id") ? req.get_param_value("family_id").c_str() : nullptr,
        req.has_param("risk_assessment_id") ? req.get_param_value("risk_assessment_id").c_str() : nullptr,
        req.has_param("compliance_status") ? req.get_param_value("compliance_status").c_str() : nullptr,
        query.data(),
        query.size());
    query.resize(std::strlen(query.c_str()));  // NOSONAR (S5813): buffer inicializado en ceros, tamaño real pasado explícitamente
    return query;
}

static std::string idFilter(int id) {
    std::string filter(96, '\0');
    cf_build_followup_id_filter(id, filter.data(), filter.size());
    filter.resize(std::strlen(filter.c_str()));  // NOSONAR (S5813): mismo motivo
    return filter;
}

static json eventFromCModule(const std::string& eventName, const json& item) {
    std::string buffer(1024, '\0');
    cf_build_followup_event_json(
        eventName.c_str(),
        item.value("followup_id", 0),
        item.value("family_id", 0),
        item.value("record_number", "").c_str(),
        item.value("compliance_status", "").c_str(),
        buffer.data(),
        buffer.size());
    buffer.resize(std::strlen(buffer.c_str()));  // NOSONAR (S5813): mismo motivo
    return json::parse(buffer);
}

// -----------------------------------------------------------------------------
// Handlers de registerRoutes(), extraídos a funciones nombradas.
// Esto reduce la complejidad cognitiva de registerRoutes() (antes 43, tope
// permitido 25) ya que registerRoutes() pasa a ser solo una lista de
// registros de rutas, sin lógica anidada. También permite declarar la
// captura explícita de cada lambda en vez de "[&]" implícito.
// -----------------------------------------------------------------------------
static void handleGetList(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                           const httplib::Request& req, httplib::Response& res) {
    handle(res, [&pgClient, &table, &req, &res]() {
        jsonOk(res, pgClient->getAll(table, buildListQuery(req)));
    });
}

static void handleGetOne(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                          const httplib::Request& req, httplib::Response& res) {
    handle(res, [&pgClient, &table, &req, &res]() {
        int id = std::stoi(req.matches[1]);
        auto item = pgClient->getOne(table, idFilter(id));
        if (item.is_null()) {
            jsonErrorResponse(res, 404, "Seguimiento no encontrado");
            return;
        }
        jsonOk(res, item);
    });
}

// Fix S1188 ("lambda con más de 20 líneas"): la lógica se extrae a una
// función nombrada (sin límite de longitud para Sonar) en vez de vivir
// dentro de la lambda; la lambda queda en una sola línea.
static json createFollowupWork(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                                const httplib::Request& req) {
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
    return created;
}

static void handleCreate(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                          const httplib::Request& req, httplib::Response& res) {
    handle(res, [&pgClient, &table, &req, &res]() {
        jsonCreated(res, createFollowupWork(pgClient, table, req));
    });
}

static void handlePatch(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                         const httplib::Request& req, httplib::Response& res) {
    handle(res, [&pgClient, &table, &req, &res]() {
        int id = std::stoi(req.matches[1]);
        // Fix R005 (S11/S14): PostgREST reporta éxito (200/204) en un
        // UPDATE aunque el WHERE no coincida con ninguna fila (es
        // comportamiento estándar de SQL/REST, no un error) — por eso
        // "ok" por sí solo no distingue "se actualizó" de "el id no
        // existía". Se verifica existencia explícitamente antes de
        // intentar la actualización, devolviendo 404 si no existe.
        auto existing = pgClient->getOne(table, idFilter(id));
        if (existing.is_null()) {
            jsonErrorResponse(res, 404, "Seguimiento no encontrado");
            return;
        }
        auto data = cleanFollowupPayload(parseBody(req), true);
        // Fix S6004+S112: "ok" en el init-statement del if, y excepción
        // dedicada (PostgRestError, ya definida en PostgRestClient.hpp)
        // en vez de std::runtime_error genérico.
        if (const bool ok = pgClient->update(table, idFilter(id), data); !ok) {
            throw PostgRestError("No se pudo actualizar el seguimiento");
        }
        auto item = pgClient->getOne(table, idFilter(id));
        if (!item.is_null()) {
            item["timeline_state"] = cf_timeline_state_label(item.value("compliance_status", ""));
            item["aps_event"] = eventFromCModule("followup.updated", item);
        }
        jsonOk(res, item.is_null() ? json({{"mensaje", "Seguimiento actualizado"}}) : item);
    });
}

// Fix S1188 ("lambda con más de 20 líneas"): misma técnica que en
// createFollowupWork — se extrae la lógica a una función nombrada.
static json completeFollowupWork(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                                  int id, const json& body) {
    const std::string requestedStatus = body.value("compliance_status", "");
    const std::string workflowStatus = cf_workflow_completion_status(requestedStatus);

    // Fix S5945 x2: char[] -> std::string.
    std::string preparedStatus(32, '\0');
    // Fix S6004: "error" declarado en el init-statement del if.
    if (std::string error(160, '\0');
        !cf_prepare_compliance_status(workflowStatus.c_str(), preparedStatus.data(), preparedStatus.size(),
                                       error.data(), error.size())) {
        error.resize(std::strlen(error.c_str()));  // NOSONAR (S5813): buffer en ceros, tamaño real pasado explícitamente
        throw std::invalid_argument(error);
    }
    preparedStatus.resize(std::strlen(preparedStatus.c_str()));  // NOSONAR (S5813): mismo motivo

    json data = json::object();
    data["compliance_status"] = preparedStatus;
    data["evaluation_date"] = body.value("evaluation_date", todayIsoDate());
    if (body.contains("noncompliance_causes")) {
        data["noncompliance_causes"] = body["noncompliance_causes"];
    } else if (cf_is_noncompliance_status(preparedStatus)) {
        data["noncompliance_causes"] = cf_noncompliance_default_cause("");
    }
    cf::domain::validateIsoDateField(data, "evaluation_date");

    // Fix S6004+S112: "ok" en el init-statement, excepción dedicada.
    if (const bool ok = pgClient->update(table, idFilter(id), data); !ok) {
        throw PostgRestError("No se pudo completar el seguimiento");
    }
    auto item = pgClient->getOne(table, idFilter(id));
    if (!item.is_null()) {
        item["timeline_state"] = cf_timeline_state_label(item.value("compliance_status", ""));
        item["aps_event"] = eventFromCModule(cf_workflow_completion_event_name(), item);
    }
    return item.is_null() ? json({{"mensaje", "Seguimiento completado"}}) : item;
}

static void handleComplete(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                            const httplib::Request& req, httplib::Response& res) {
    handle(res, [&pgClient, &table, &req, &res]() {
        int id = std::stoi(req.matches[1]);
        // Fix R005 (S11/S14): mismo motivo que en handlePatch — verificar
        // existencia antes de intentar completar el seguimiento.
        auto existing = pgClient->getOne(table, idFilter(id));
        if (existing.is_null()) {
            jsonErrorResponse(res, 404, "Seguimiento no encontrado");
            return;
        }
        json body = req.body.empty() ? json::object() : parseBody(req);
        jsonOk(res, completeFollowupWork(pgClient, table, id, body));
    });
}

static void handleClearAll(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                            httplib::Response& res) {
    handle(res, [&pgClient, &table, &res]() {
        // followup_id es la clave primaria y nunca es NULL,
        // por lo que este filtro selecciona todos los registros.
        //
        // Fix S6004+S112: "ok" en el init-statement, excepción dedicada.
        if (const bool ok = pgClient->remove(table, "followup_id=not.is.null"); !ok) {
            throw PostgRestError("No se pudo vaciar la tabla de seguimientos");
        }

        jsonOk(res, {
            {"mensaje", "Tabla de seguimientos vaciada correctamente"}
        });
    });
}

static void handleDeleteOne(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                             const httplib::Request& req, httplib::Response& res) {
    handle(res, [&pgClient, &table, &req, &res]() {
        int id = std::stoi(req.matches[1]);
        // Fix R005 (S11/S14): mismo motivo que en handlePatch — PostgREST
        // reporta éxito en un DELETE aunque el WHERE no matchee ninguna
        // fila, así que se verifica existencia antes de borrar.
        auto existing = pgClient->getOne(table, idFilter(id));
        if (existing.is_null()) {
            jsonErrorResponse(res, 404, "Seguimiento no encontrado");
            return;
        }
        if (const bool ok = pgClient->remove(table, idFilter(id)); !ok) {
            throw PostgRestError("No se pudo eliminar el seguimiento");
        }
        jsonOk(res, {{"mensaje", "Seguimiento eliminado"}, {"followup_id", id}});
    });
}

static void registerRoutes(httplib::Server& svr, const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table, const std::string& basePath) {
    // Fix S7121 x7: httplib::Server::Get/Post/Patch/Delete reciben
    // "const std::string&", así que llamar .c_str() aquí era redundante
    // (construye un const char*, que el parámetro vuelve a convertir a
    // std::string internamente). Se pasa el std::string directamente.
    svr.Get(basePath, [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handleGetList(pgClient, table, req, res);
    });

    svr.Get(basePath + R"(/(\d+))", [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handleGetOne(pgClient, table, req, res);
    });

    svr.Post(basePath, [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handleCreate(pgClient, table, req, res);
    });

    svr.Patch(basePath + R"(/(\d+))", [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handlePatch(pgClient, table, req, res);
    });

    svr.Post(basePath + R"(/(\d+)/complete)", [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handleComplete(pgClient, table, req, res);
    });

    // Vaciar completamente la tabla de seguimientos
    svr.Delete(basePath + "/clear",
        [pgClient, table](const httplib::Request&, httplib::Response& res) {
            handleClearAll(pgClient, table, res);
        });

    svr.Delete(basePath + R"(/(\d+))", [pgClient, table](const httplib::Request& req, httplib::Response& res) {
        handleDeleteOne(pgClient, table, req, res);
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

// -----------------------------------------------------------------------------
// Handlers de las rutas registradas directamente en main(), extraídos a
// funciones nombradas por la misma razón que en registerRoutes(): reduce la
// complejidad cognitiva de main() (antes 33, tope permitido 25).
// -----------------------------------------------------------------------------
static void handleHealth(const std::string& pgHost, int pgPort, const std::string& table,
                          const cf::parallel::ParallelConfig& parallelCfg, httplib::Response& res) {
    jsonOk(res, {
        {"status", "ok"},
        {"service", "community-followup-service"},
        {"architecture", "hibrida C/C++ con OpenMP — evaluacion paralela por seguimiento, insercion unica"},
        {"table", table},
        // NOSONAR (cpp:S6185, std::format): requiere GCC 13+, no disponible
        // en el toolchain (ubuntu:22.04 -> GCC 11.4).
        {"postgrest", pgHost + ":" + std::to_string(pgPort)},  // NOSONAR
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
}

static void handleComplianceAlerts(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                                    httplib::Response& res) {
    handle(res, [&pgClient, &table, &res]() {
        jsonOk(res, pgClient->getAll(table, cf_compliance_alert_filter()));
    });
}

// Fix R004 (S12/S13): cf_compliance_base_path() ("/compliance") estaba
// definida en compliance-controller.c pero nunca se registraba en
// registerRoutes()/main(), y las funciones de compliance-repository.c
// (cf_build_noncompliance_query, cf_build_pending_query) no se usaban en
// ningún lado — quedaban como código muerto. Este handler conecta ambas
// piezas ya existentes: un resumen de cumplimiento que combina los
// seguimientos en incumplimiento (NO_CUMPLE) y los pendientes (PARCIAL),
// reutilizando exactamente la lógica de query ya definida en el
// repositorio en vez de duplicarla o inventar un comportamiento nuevo.
static void handleComplianceOverview(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                                      httplib::Response& res) {
    handle(res, [&pgClient, &table, &res]() {
        std::string noncomplianceQuery(256, '\0');
        cf_build_noncompliance_query(noncomplianceQuery.data(), noncomplianceQuery.size());
        noncomplianceQuery.resize(std::strlen(noncomplianceQuery.c_str()));  // NOSONAR (S5813): buffer en ceros, tamaño real pasado explícitamente

        std::string pendingQuery(256, '\0');
        cf_build_pending_query(pendingQuery.data(), pendingQuery.size());
        pendingQuery.resize(std::strlen(pendingQuery.c_str()));  // NOSONAR (S5813): mismo motivo

        auto noncompliance = pgClient->getAll(table, noncomplianceQuery);
        auto pending = pgClient->getAll(table, pendingQuery);

        jsonOk(res, {
            {"noncompliance", noncompliance},
            {"pending", pending},
            {"noncompliance_count", noncompliance.is_array() ? static_cast<long>(noncompliance.size()) : 0},
            {"pending_count", pending.is_array() ? static_cast<long>(pending.size()) : 0}
        });
    });
}

// -------------------------------------------------------------------
// A. POST /followups/batch/validate — mide CPU puro con OpenMP.
// Valida + normaliza + clasifica un lote completo; NO inserta nada.
// -------------------------------------------------------------------
static void handleBatchValidate(const httplib::Request& req, httplib::Response& res) {
    handle(res, [&req, &res]() {
        json body = parseBody(req);
        auto raw = extractRecordsArray(body);
        auto result = cf::parallel::processBatchCpuPhase(raw, false);
        jsonOk(res, buildBatchValidateResponse(result));
    });
}

// -------------------------------------------------------------------
// B. POST /followups/batch — ingreso por lotes de verdad.
// Fase 2 (paralela, CPU): validar/normalizar/clasificar.
// Fase 3 (secuencial, controlada): insertar en PostgREST en bloques
// (PERSIST_CHUNK_SIZE), FUERA de la región OpenMP.
// -------------------------------------------------------------------
// Fix S1188 ("lambda con más de 20 líneas"): misma técnica de extracción.
static json batchInsertWork(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                             const httplib::Request& req) {
    json body = parseBody(req);
    auto raw = extractRecordsArray(body);

    auto result = cf::parallel::processBatchCpuPhase(raw, false);

    const auto& cfg = cf::parallel::config();
    const auto tPersistStart = std::chrono::steady_clock::now();

    json validRecords = json::array();
    // Fix S5350: "item" solo se lee (item.valid, item.normalized), nunca
    // se modifica -> referencia a const.
    for (const auto& item : result.items) {
        if (item.valid) validRecords.push_back(item.normalized);
    }

    long inserted = 0;
    json insertErrors = json::array();
    // Fix S5827: static_cast<std::size_t> ya deja el tipo explícito -> auto.
    const auto chunkSize = static_cast<std::size_t>(std::max(1, cfg.persistChunkSize));

    // Fix S886 ("las 3 expresiones del for deben tratar solo de control de
    // bucle"): se extrae el tamaño a una variable antes del bucle, así la
    // condición queda como una comparación simple entre dos variables de
    // control (start, total) en vez de una llamada a .size() en cada
    // vuelta. Además, chunkSize siempre es >= 1 (viene de std::max(1, ...)
    // arriba), así que "start" avanza en cada vuelta y el bucle no puede
    // volverse infinito.
    const std::size_t total = validRecords.size();
    assert(chunkSize > 0);
    for (std::size_t start = 0; start < total; start += chunkSize) {
        const std::size_t end = std::min(start + chunkSize, total);
        json chunk = json::array();
        for (std::size_t k = start; k < end; ++k) chunk.push_back(validRecords[k]);

        // NOSONAR (cpp:S1181, "capturar una excepción más específica"):
        // insertMany() puede lanzar distintos tipos de error de red/HTTP;
        // el manejo es idéntico en cada caso — registrar el bloque como
        // fallido y seguir con el siguiente bloque, sin abortar el lote
        // completo.
        try {
            auto insertedChunk = pgClient->insertMany(table, chunk);
            if (insertedChunk.is_array()) {
                inserted += static_cast<long>(insertedChunk.size());
            } else {
                // NOSONAR (cpp:S6185, std::format): requiere GCC 13+, no disponible en el toolchain (ubuntu:22.04 -> GCC 11.4).
                insertErrors.push_back("Respuesta inesperada insertando el bloque [" + std::to_string(start) + ".." + std::to_string(end) + ")");  // NOSONAR
            }
        } catch (const std::exception& e) {  // NOSONAR
            insertErrors.push_back("Bloque [" + std::to_string(start) + ".." + std::to_string(end) + "): " + e.what());  // NOSONAR
        }
    }

    const auto tPersistEnd = std::chrono::steady_clock::now();
    const double persistMs = std::chrono::duration<double, std::milli>(tPersistEnd - tPersistStart).count();
    const double totalMs = result.processingTimeMs + persistMs;

    return json{
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
    };
}

static void handleBatchInsert(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                               const httplib::Request& req, httplib::Response& res) {
    handle(res, [&pgClient, &table, &req, &res]() {
        jsonOk(res, batchInsertWork(pgClient, table, req));
    });
}

// -------------------------------------------------------------------
// C. GET /followups/analytics/summary — resumen con reduction OpenMP.
// ?simulate=N genera N registros SOLO en memoria (no toca la base de
// datos); sin ese parametro, trae todos los registros reales via
// PostgREST (una sola llamada de red) y calcula el resumen en memoria.
// -------------------------------------------------------------------
// Fix S1188 ("lambda con más de 20 líneas"): misma técnica de extracción.
static json analyticsSummaryWork(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                                  const httplib::Request& req) {
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

    return json{
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
    };
}

static void handleAnalyticsSummary(const std::shared_ptr<PostgRestClient>& pgClient, const std::string& table,
                                    const httplib::Request& req, httplib::Response& res) {
    handle(res, [&pgClient, &table, &req, &res]() {
        jsonOk(res, analyticsSummaryWork(pgClient, table, req));
    });
}

int main() {
    try {
        // Fix R008/R015 (S9/S12): cf_db_host_env()/cf_db_port_env()/
        // cf_db_name_env() (db-connection.c) definían los nombres de
        // estas variables de entorno pero nunca se usaban — main()
        // repetía los mismos literales directamente ("POSTGREST_HOST",
        // etc.), dejando db-connection.c como código muerto con 0% de
        // cobertura. Ahora es la única fuente de verdad para estos nombres.
        const std::string pgHost = env(cf_db_host_env(), "localhost");
        const int pgPort = std::atoi(env(cf_db_port_env(), "3000"));
        const int port = std::atoi(env("PORT", "8084"));
        const std::string table = env(cf_db_name_env(), cf_followup_table_name());
        const auto& parallelCfg = cf::parallel::config();

        auto pgClient = std::make_shared<PostgRestClient>(pgHost, pgPort);
        httplib::Server svr;

        // BACKEND_THREADS: concurrencia HTTP (independiente de OpenMP).
        // Se usa std::make_unique(...).release() en vez de "new" directo:
        // httplib::Server::new_task_queue requiere devolver un puntero crudo
        // (la librería toma posesión y lo libera internamente), pero
        // make_unique dentro de un contexto con excepciones asegura que, si
        // el constructor de ThreadPool lanzara, no queden fugas de memoria
        // antes del release().
        svr.new_task_queue = [threads = parallelCfg.backendThreads] {
            return std::make_unique<httplib::ThreadPool>(static_cast<size_t>(threads)).release();
        };

        svr.Get("/health", [pgHost, pgPort, table, &parallelCfg](const httplib::Request&, httplib::Response& res) {
            handleHealth(pgHost, pgPort, table, parallelCfg, res);
        });

        svr.Get(cf_compliance_alerts_path(), [pgClient, table](const httplib::Request&, httplib::Response& res) {
            handleComplianceAlerts(pgClient, table, res);
        });

        // Fix R004 (S12/S13): conectar cf_compliance_base_path() ("/compliance"),
        // definida en compliance-controller.c pero nunca registrada.
        svr.Get(cf_compliance_base_path(), [pgClient, table](const httplib::Request&, httplib::Response& res) {
            handleComplianceOverview(pgClient, table, res);
        });

        svr.Post("/followups/batch/validate", [](const httplib::Request& req, httplib::Response& res) {
            handleBatchValidate(req, res);
        });

        svr.Post("/followups/batch", [pgClient, table](const httplib::Request& req, httplib::Response& res) {
            handleBatchInsert(pgClient, table, req, res);
        });

        svr.Get("/followups/analytics/summary", [pgClient, table](const httplib::Request& req, httplib::Response& res) {
            handleAnalyticsSummary(pgClient, table, req, res);
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
