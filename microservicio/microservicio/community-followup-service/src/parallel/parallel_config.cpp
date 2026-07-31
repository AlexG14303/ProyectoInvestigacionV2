#include "parallel_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace cf::parallel {

namespace {

int positiveEnvInt(const char* key, int fallback) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return fallback;
    // Fix S2738 ("catch un tipo específico"): std::stoi solo puede lanzar
    // std::invalid_argument (no es un número) o std::out_of_range (fuera
    // de rango de int) — ambas derivan de std::exception, así que no hace
    // falta un catch(...) genérico aquí.
    try {
        const int value = std::stoi(raw);
        return value > 0 ? value : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string envString(const char* key, const std::string& fallback) {
    const char* raw = std::getenv(key);
    return (raw && *raw) ? std::string(raw) : fallback;
}

std::string defaultLabel(int threads) {
    // Fix S5945 ("usar std::string en vez de un arreglo C-style").
    //
    // NOSONAR (cpp:S6494, "usar std::format en vez de snprintf"):
    // std::format requiere GCC 13+; el Dockerfile usa ubuntu:22.04, que
    // trae GCC 11.4 por defecto — no compilaría con ese toolchain.
    std::string buf(8, '\0');
    std::snprintf(buf.data(), buf.size(), "p%02d", threads);  // NOSONAR
    // NOSONAR (cpp:S5813, "verificar que el uso de strlen sea seguro"):
    // "buf" parte en 8 bytes '\0' y se pasa junto con buf.size(), así que
    // snprintf nunca escribe más allá del buffer; strlen() no puede leer
    // fuera de sus límites.
    buf.resize(std::strlen(buf.c_str()));  // NOSONAR
    return buf;
}

}  // namespace

const ParallelConfig& config() {
    static const ParallelConfig instance = [] {
        ParallelConfig c;
        c.ompNumThreads = positiveEnvInt("OMP_NUM_THREADS", 1);
        c.backendThreads = positiveEnvInt("BACKEND_THREADS", c.ompNumThreads);
        c.minParallelBatchSize = positiveEnvInt("MIN_PARALLEL_BATCH_SIZE", 100);
        c.persistChunkSize = positiveEnvInt("PERSIST_CHUNK_SIZE", 500);
        c.parallelismLabel = envString("CF_PARALLELISM_LEVEL", defaultLabel(c.ompNumThreads));
        c.ompDynamicDisabled = envString("OMP_DYNAMIC", "FALSE") == "FALSE";
        c.ompMaxActiveLevels = positiveEnvInt("OMP_MAX_ACTIVE_LEVELS", 1);
        c.ompProcBind = envString("OMP_PROC_BIND", "TRUE");
        c.ompPlaces = envString("OMP_PLACES", "cores");
        return c;
    }();
    return instance;
}

}  // namespace cf::parallel
