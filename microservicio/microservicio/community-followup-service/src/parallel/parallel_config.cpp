#include "parallel_config.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace cf::parallel {

namespace {

int positiveEnvInt(const char* key, int fallback) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return fallback;
    try {
        const int value = std::stoi(raw);
        return value > 0 ? value : fallback;
    } catch (...) {
        return fallback;
    }
}

std::string envString(const char* key, const std::string& fallback) {
    const char* raw = std::getenv(key);
    return (raw && *raw) ? std::string(raw) : fallback;
}

std::string defaultLabel(int threads) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "p%02d", threads);
    return std::string(buf);
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
