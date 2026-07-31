#include "followup_evaluation_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>

#include <omp.h>

#include "../parallel/parallel_config.h"

namespace cf::domain {

namespace {

struct RuleOutcome {
    bool triggered = false;
    int riskDelta = 0;
    int complianceDelta = 0;
    std::string alert;
    std::string recommendation;
};

struct RuleContext {
    std::string complianceStatus;
    std::string riskDescriptionLower;
    std::string noncomplianceCausesLower;
    std::string familyCommitmentLower;
    std::string healthTeamCommitmentLower;
    std::string scheduledActivitiesLower;
    std::string recordNumber;
    bool hasResponsibleStaff = false;
    bool hasRiskAssessment = false;
    bool hasAnalysisDate = false;
    int daysSinceEvaluation = -1;  // -1 = sin evaluation_date o formato inválido
};

// NOSONAR (cpp:S7034, "usar .contains() en vez de .find()"): .contains()
// es de C++23. No se aplica aquí porque no está confirmado que el proyecto
// compile con -std=c++23 (ver CMakeLists.txt) — .find() != npos es
// equivalente y compatible con cualquier estándar desde C++11.
bool contains(std::string_view haystackLower, const char* needle) {
    return haystackLower.find(needle) != std::string_view::npos;  // NOSONAR
}

std::string toLower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// -----------------------------------------------------------------------
// Tablas de reglas por categoría (datos, no código repetido 74 veces).
// -----------------------------------------------------------------------
constexpr std::array kFamilyWeak = {"no puede", "no dispone", "dificultad economica",
                                     "imposibilidad", "sin compromiso", "abandono del proceso"};
constexpr std::array kFamilyStrong = {"compromete", "acepta seguimiento", "dispuesto a colaborar"};
constexpr std::array kHealthTeamWeak = {"no disponible", "sin seguimiento", "pendiente asignacion",
                                         "sin contacto"};
constexpr std::array kCriticalNoncompliance = {
    "violencia", "negligencia", "abandono", "maltrato", "desnutricion",
    "consumo de sustancias", "hacinamiento", "trabajo infantil", "desercion escolar",
    "situacion de calle", "riesgo vital", "autolesion", "abuso", "explotacion", "vulnerabilidad extrema"};
constexpr std::array kCriticalRiskDesc = {
    "violencia", "negligencia", "abandono", "maltrato", "desnutricion",
    "consumo de sustancias", "hacinamiento", "trabajo infantil", "desercion escolar",
    "situacion de calle", "riesgo vital", "autolesion", "abuso", "explotacion", "enfermedad cronica"};
constexpr std::array kUrgency = {"urgente", "emergencia", "inmediato", "critico", "grave"};
constexpr std::array kActivityKeywords = {"visita", "taller", "seguimiento telefonico", "control"};
constexpr std::array kDateThresholds = {7, 15, 30, 60, 90};

constexpr int kFamilyWeakCount = 6;
constexpr int kFamilyStrongCount = 3;
constexpr int kFamilyStructuralCount = 2;
constexpr int kHealthTeamWeakCount = 4;
constexpr int kHealthTeamStructuralCount = 1;
constexpr int kNoncomplianceCount = 15;
constexpr int kRiskDescCount = 15;
constexpr int kUrgencyCount = 5;
constexpr int kDateDelayCount = 5;
constexpr int kResponsibleCount = 3;
constexpr int kActivitiesCount = 6;
constexpr int kRecordQualityCount = 6;
constexpr int kComplianceCount = 3;

constexpr int kFamilyCount = kFamilyWeakCount + kFamilyStrongCount + kFamilyStructuralCount;  // 11
constexpr int kHealthTeamCount = kHealthTeamWeakCount + kHealthTeamStructuralCount;            // 5

constexpr int kOffCompliance = 0;
constexpr int kOffFamily = kOffCompliance + kComplianceCount;                // 3
constexpr int kOffHealthTeam = kOffFamily + kFamilyCount;                    // 14
constexpr int kOffNoncompliance = kOffHealthTeam + kHealthTeamCount;         // 19
constexpr int kOffRiskDesc = kOffNoncompliance + kNoncomplianceCount;        // 34
constexpr int kOffUrgency = kOffRiskDesc + kRiskDescCount;                   // 49
constexpr int kOffDateDelay = kOffUrgency + kUrgencyCount;                   // 54
constexpr int kOffResponsible = kOffDateDelay + kDateDelayCount;             // 59
constexpr int kOffActivities = kOffResponsible + kResponsibleCount;         // 62
constexpr int kOffRecordQuality = kOffActivities + kActivitiesCount;        // 68
constexpr int kIndependentRuleCount = kOffRecordQuality + kRecordQualityCount;  // 74
constexpr int kCombinationRuleCount = 6;
constexpr int kTotalRuleCountValue = kIndependentRuleCount + kCombinationRuleCount;  // 80

// -----------------------------------------------------------------------
// Despacho de las 74 reglas independientes. Un único punto de entrada,
// indexado, sin std::function ni heap: cada llamada solo lee `ctx` (const)
// y devuelve un RuleOutcome por valor.
// -----------------------------------------------------------------------
namespace {

RuleOutcome evaluateComplianceRule(int local, const RuleContext& ctx) {
    if (local == 0 && ctx.complianceStatus == "SI_CUMPLE") return {true, -10, 15, "", ""};
    if (local == 1 && ctx.complianceStatus == "PARCIAL")
        return {true, 8, -5, "", "Reforzar seguimiento por cumplimiento parcial"};
    if (local == 2 && ctx.complianceStatus == "NO_CUMPLE")
        return {true, 25, -20, "Incumplimiento familiar detectado", "Programar visita domiciliaria prioritaria"};
    return {};
}

RuleOutcome evaluateFamilyRule(int local, const RuleContext& ctx) {
    if (local < kFamilyWeakCount) {
        if (contains(ctx.familyCommitmentLower, kFamilyWeak[local]))
            return {true, 6, -6, "", "Explorar apoyo adicional a la familia"};
        return {};
    }
    local -= kFamilyWeakCount;
    if (local < kFamilyStrongCount) {
        if (contains(ctx.familyCommitmentLower, kFamilyStrong[local])) return {true, -4, 6, "", ""};
        return {};
    }
    local -= kFamilyStrongCount;
    if (local == 0 && ctx.familyCommitmentLower.empty())
        return {true, 10, -10, "Compromiso familiar no registrado", "Registrar compromiso familiar explícito"};
    if (local == 1 && !ctx.familyCommitmentLower.empty() && ctx.familyCommitmentLower.size() < 15)
        return {true, 4, -4, "", "Ampliar el detalle del compromiso familiar registrado"};
    return {};
}

RuleOutcome evaluateHealthTeamRule(int local, const RuleContext& ctx) {
    if (local < kHealthTeamWeakCount) {
        if (contains(ctx.healthTeamCommitmentLower, kHealthTeamWeak[local]))
            return {true, 5, -5, "Compromiso del equipo de salud débil", ""};
        return {};
    }
    if (ctx.healthTeamCommitmentLower.empty())
        return {true, 8, -8, "Sin compromiso registrado del equipo de salud",
                "Asignar responsable de seguimiento en salud"};
    return {};
}

RuleOutcome evaluateNoncomplianceRule(int local, const RuleContext& ctx) {
    if (contains(ctx.noncomplianceCausesLower, kCriticalNoncompliance[local]))
        return {true, 12, -8, std::string("Causa crítica detectada: ") + kCriticalNoncompliance[local], ""};
    return {};
}

RuleOutcome evaluateRiskDescRule(int local, const RuleContext& ctx) {
    if (contains(ctx.riskDescriptionLower, kCriticalRiskDesc[local]))
        return {true, 10, 0, std::string("Término crítico en descripción: ") + kCriticalRiskDesc[local], ""};
    return {};
}

RuleOutcome evaluateUrgencyRule(int local, const RuleContext& ctx) {
    if (contains(ctx.riskDescriptionLower, kUrgency[local]) || contains(ctx.noncomplianceCausesLower, kUrgency[local]))
        return {true, 8, 0, "", "Priorizar atención por lenguaje de urgencia detectado"};
    return {};
}

RuleOutcome evaluateDateDelayRule(int local, const RuleContext& ctx) {
    if (ctx.daysSinceEvaluation >= kDateThresholds[local])
        return {true, 3 * (local + 1), -2 * (local + 1), "", ""};
    return {};
}

RuleOutcome evaluateResponsibleRule(int local, const RuleContext& ctx) {
    if (local == 0 && !ctx.hasResponsibleStaff)
        return {true, 6, 0, "Seguimiento sin responsable asignado", "Asignar responsable comunitario"};
    if (local == 1 && !ctx.hasRiskAssessment)
        return {true, 4, 0, "", "Vincular evaluación de riesgo previa"};
    if (local == 2 && !ctx.hasResponsibleStaff && !ctx.hasRiskAssessment)
        return {true, 10, 0, "Seguimiento sin responsable ni evaluación de riesgo vinculada", ""};
    return {};
}

RuleOutcome evaluateActivitiesRule(int local, const RuleContext& ctx) {
    if (local == 0 && ctx.scheduledActivitiesLower.empty())
        return {true, 5, -5, "", "Programar actividades de seguimiento"};
    if (local == 1 && !ctx.scheduledActivitiesLower.empty() && ctx.scheduledActivitiesLower.size() < 20)
        return {true, 2, -2, "", "Ampliar el detalle de las actividades programadas"};
    if (local >= 2 && local < 6) {
        const char* kw = kActivityKeywords[local - 2];
        if (!contains(ctx.scheduledActivitiesLower, kw))
            return {true, 0, 0, "", std::string("Considerar incluir actividad tipo: ") + kw};
    }
    return {};
}

RuleOutcome evaluateRecordQualityRule(int local, const RuleContext& ctx) {
    if (local == 0 && !ctx.hasAnalysisDate) return {true, 3, 0, "", "Registrar fecha de análisis"};
    if (local == 1 && ctx.riskDescriptionLower.empty())
        return {true, 5, -5, "Seguimiento sin descripción de riesgo", "Completar descripción de riesgo"};
    if (local == 2 && ctx.complianceStatus == "NO_CUMPLE" && ctx.noncomplianceCausesLower.empty())
        return {true, 8, 0, "Incumplimiento sin causas registradas", "Documentar causas del incumplimiento"};
    if (local == 3 && ctx.recordNumber.size() < 5) return {true, 0, 0, "", ""};
    if (local == 4 && !ctx.familyCommitmentLower.empty() && ctx.familyCommitmentLower == ctx.healthTeamCommitmentLower)
        return {true, 0, 0, "", "Revisar registro: compromiso familiar y del equipo de salud son idénticos"};
    if (local == 5 && ctx.scheduledActivitiesLower.empty() && ctx.complianceStatus == "NO_CUMPLE")
        return {true, 6, 0, "Incumplimiento sin actividades de seguimiento programadas",
                "Programar plan de actividades urgente"};
    return {};
}

}  // namespace

RuleOutcome evaluateIndependentRule(int index, const RuleContext& ctx) {
    if (index < kOffFamily)       return evaluateComplianceRule(index - kOffCompliance, ctx);
    if (index < kOffHealthTeam)   return evaluateFamilyRule(index - kOffFamily, ctx);
    if (index < kOffNoncompliance) return evaluateHealthTeamRule(index - kOffHealthTeam, ctx);
    if (index < kOffRiskDesc)     return evaluateNoncomplianceRule(index - kOffNoncompliance, ctx);
    if (index < kOffUrgency)      return evaluateRiskDescRule(index - kOffRiskDesc, ctx);
    if (index < kOffDateDelay)    return evaluateUrgencyRule(index - kOffUrgency, ctx);
    if (index < kOffResponsible)  return evaluateDateDelayRule(index - kOffDateDelay, ctx);
    if (index < kOffActivities)   return evaluateResponsibleRule(index - kOffResponsible, ctx);
    if (index < kOffRecordQuality) return evaluateActivitiesRule(index - kOffActivities, ctx);
    return evaluateRecordQualityRule(index - kOffRecordQuality, ctx);
}

std::string classifyRiskLevel(int score) {
    if (score >= 70) return "ALTO";
    if (score >= 40) return "MEDIO";
    return "BAJO";
}

std::string classifyPriority(std::string_view riskLevel) {
    if (riskLevel == "ALTO") return "URGENTE";
    if (riskLevel == "MEDIO") return "MODERADA";
    return "NORMAL";
}

// -----------------------------------------------------------------------
// 6 reglas de COMBINACIÓN — deliberadamente secuenciales. Dependen del
// resultado ya agregado de las 74 reglas independientes (riskScore,
// complianceScore, cantidad de alertas), así que no pueden evaluarse en
// paralelo junto con las anteriores sin introducir una dependencia de datos.
// -----------------------------------------------------------------------
int evaluateCombinationRules(EvaluationResult& result, const RuleContext& ctx) {
    int triggered = 0;

    if (result.riskScore >= 60 && ctx.complianceStatus == "NO_CUMPLE") {
        result.alerts.emplace_back("Combinación crítica: riesgo alto con incumplimiento");
        result.priority = "URGENTE";
        ++triggered;
    }
    if (static_cast<int>(result.alerts.size()) >= 5 && result.priority != "URGENTE") {
        result.priority = "ALTA";
        ++triggered;
    }
    if (result.complianceScore <= 20 && ctx.scheduledActivitiesLower.empty()) {
        result.recommendations.emplace_back("Intervención inmediata: bajo cumplimiento sin plan de actividades");
        ++triggered;
    }
    if (ctx.daysSinceEvaluation >= 30 && result.riskScore >= 40) {
        result.recommendations.emplace_back("Programar visita domiciliaria por retraso combinado con riesgo elevado");
        ++triggered;
    }
    if (!ctx.hasResponsibleStaff && result.riskScore >= 50) {
        result.alerts.emplace_back("Seguimiento de alto riesgo sin responsable asignado");
        ++triggered;
    }
    if (result.riskScore <= 15 && result.complianceScore >= 80) {
        result.recommendations.emplace_back("Evaluar cierre o espaciamiento del seguimiento por bajo riesgo sostenido");
        ++triggered;
    }

    return triggered;
}

int daysBetweenIsoDates(const std::string& from, const std::string& to) {
    int y1;
    int m1;
    int d1;
    int y2;
    int m2;
    int d2;
    if (std::sscanf(from.c_str(), "%d-%d-%d", &y1, &m1, &d1) != 3) return -1;
    if (std::sscanf(to.c_str(), "%d-%d-%d", &y2, &m2, &d2) != 3) return -1;

    // Fix S6229 x2 ("usar std::chrono en vez de mktime"): además de
    // resolver el hallazgo, esto corrige un bug sutil del código anterior
    // — mktime() interpreta el std::tm como HORA LOCAL, así que un cambio
    // de horario de verano (DST) entre las dos fechas podía desviar el
    // resultado en ±1 día. year_month_day/sys_days hacen aritmética de
    // calendario pura (sin zona horaria), así que ese caso queda resuelto
    // de raíz, no solo silenciado.
    const std::chrono::year_month_day ymdFrom{std::chrono::year{y1}, std::chrono::month{static_cast<unsigned>(m1)},
                                                std::chrono::day{static_cast<unsigned>(d1)}};
    const std::chrono::year_month_day ymdTo{std::chrono::year{y2}, std::chrono::month{static_cast<unsigned>(m2)},
                                              std::chrono::day{static_cast<unsigned>(d2)}};
    if (!ymdFrom.ok() || !ymdTo.ok()) return -1;

    const std::chrono::sys_days sysFrom = ymdFrom;
    const std::chrono::sys_days sysTo = ymdTo;
    return static_cast<int>((sysTo - sysFrom).count());
}

std::string todayIsoDate() {
    // Fix S6229 ("Replace this use of time with std::chrono"): esta
    // conversión es válida desde C++11, sin depender de ninguna feature
    // de C++20+, a diferencia de std::format más abajo.
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    // Fix S5945 ("usar std::string en vez de char[]"): buffer como
    // std::string en vez de char[32].
    //
    // NOSONAR (cpp:S6494, "usar std::format en vez de snprintf"):
    // std::format requiere GCC 13+; el Dockerfile usa ubuntu:22.04 con
    // GCC 11.4 por defecto — std::format no compilaría. snprintf sigue
    // siendo la opción segura y portable aquí.
    std::string buf(32, '\0');
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);  // NOSONAR
    // NOSONAR (cpp:S5813): "buf" parte en 32 bytes '\0' y se pasa junto
    // con buf.size(), así que snprintf nunca escribe más allá del buffer;
    // strlen() no puede leer fuera de sus límites.
    buf.resize(std::strlen(buf.c_str()));  // NOSONAR
    return buf;
}

RuleContext buildContext(const json& record) {
    RuleContext ctx;
    ctx.complianceStatus = record.value("compliance_status", "");
    ctx.riskDescriptionLower = toLower(record.value("risk_description", ""));
    ctx.noncomplianceCausesLower = toLower(record.value("noncompliance_causes", ""));
    ctx.familyCommitmentLower = toLower(record.value("family_commitment", ""));
    ctx.healthTeamCommitmentLower = toLower(record.value("health_team_commitment", ""));
    ctx.scheduledActivitiesLower = toLower(record.value("scheduled_activities", ""));
    ctx.recordNumber = record.value("record_number", "");
    ctx.hasResponsibleStaff = record.contains("responsible_staff_id") && !record["responsible_staff_id"].is_null();
    ctx.hasRiskAssessment = record.contains("risk_assessment_id") && !record["risk_assessment_id"].is_null();
    ctx.hasAnalysisDate = record.contains("analysis_date") && !record["analysis_date"].is_null();

    ctx.daysSinceEvaluation = -1;
    if (record.contains("evaluation_date") && record["evaluation_date"].is_string()) {
        const int days = daysBetweenIsoDates(record["evaluation_date"].get<std::string>(), todayIsoDate());
        if (days >= 0) ctx.daysSinceEvaluation = days;
    }
    return ctx;
}

}  // namespace

int totalRuleCount() { return kTotalRuleCountValue; }

EvaluationResult evaluateFollowup(const json& record) {
    const auto t0 = std::chrono::steady_clock::now();

    // Preparación secuencial y barata: normalizar texto a minúsculas y
    // extraer flags UNA sola vez, para que las 74 reglas no repitan ese
    // trabajo cada una por su cuenta dentro de la región paralela.
    const RuleContext ctx = buildContext(record);

    const auto& cfg = cf::parallel::config();
    const int threads = std::min(cfg.ompNumThreads, kIndependentRuleCount);

    std::array<RuleOutcome, kIndependentRuleCount> outcomes;

    // FASE PARALELA — 74 reglas independientes. Cada hilo escribe solo en
    // su propio índice de `outcomes`; no hay mutex ni push_back concurrente.
    #pragma omp parallel for num_threads(threads) schedule(dynamic, 8)
    for (int i = 0; i < kIndependentRuleCount; ++i) {
        outcomes[static_cast<std::size_t>(i)] = evaluateIndependentRule(i, ctx);
    }

    // Agregación SECUENCIAL sobre un arreglo ya calculado — no hay
    // condición de carrera posible aquí porque el parallel for ya terminó
    // (barrera implícita) antes de llegar a esta línea.
    EvaluationResult result;
    int risk = 40;         // línea base neutral, ajustable
    int compliance = 50;   // línea base neutral, ajustable
    int triggered = 0;
    for (const auto& o : outcomes) {
        if (!o.triggered) continue;
        ++triggered;
        risk += o.riskDelta;
        compliance += o.complianceDelta;
        if (!o.alert.empty()) result.alerts.push_back(o.alert);
        if (!o.recommendation.empty()) result.recommendations.push_back(o.recommendation);
    }

    result.riskScore = std::clamp(risk, 0, 100);
    result.complianceScore = std::clamp(compliance, 0, 100);
    result.riskLevel = classifyRiskLevel(result.riskScore);
    result.priority = classifyPriority(result.riskLevel);

    // FASE DE COMBINACIÓN — secuencial a propósito (ver comentario arriba).
    triggered += evaluateCombinationRules(result, ctx);

    result.rulesEvaluated = kTotalRuleCountValue;
    result.rulesTriggered = triggered;
    result.parallelEnabled = threads > 1;
    result.threadsUsed = threads;

    const auto t1 = std::chrono::steady_clock::now();
    result.processingTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return result;
}

}  // namespace cf::domain
