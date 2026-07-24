#pragma once

#include <string>

namespace cf::parallel {

// Configuración de paralelismo, leída UNA sola vez al arrancar el proceso.
//
// Tres variables conceptualmente distintas — no deben confundirse:
//   - ompNumThreads: hilos que usa CADA región OpenMP dentro de UNA request
//     (paralelismo interno de CPU). Viene de OMP_NUM_THREADS.
//   - backendThreads: cuántas requests HTTP se atienden EN SIMULTÁNEO
//     (concurrencia del servidor, vía httplib::ThreadPool). Viene de
//     BACKEND_THREADS. Es independiente de OpenMP.
//   - parallelismLabel: etiqueta puramente informativa (p01, p02, p04...)
//     para trazabilidad en /health y en las métricas de los endpoints de
//     lote. Viene de CF_PARALLELISM_LEVEL si se define, o se deriva de
//     ompNumThreads. NO controla ningún hilo por sí misma.
//
// Importante: aquí NO se llama a omp_set_num_threads()/omp_set_dynamic().
// Esas funciones solo modifican el ICV (internal control variable) del hilo
// que las invoca, y no se propagan a los hilos que crea httplib::ThreadPool
// (son std::thread normales, no hilos OpenMP). En su lugar:
//   - ompNumThreads se pasa EXPLÍCITAMENTE con num_threads(...) en cada
//     pragma, así que no depende de qué hilo entra a la región paralela.
//   - OMP_DYNAMIC, OMP_MAX_ACTIVE_LEVELS, OMP_PROC_BIND y OMP_PLACES se
//     configuran como variables de entorno del proceso (ver
//     docker-compose.yml): el runtime de OpenMP las lee UNA vez al iniciar
//     y las aplica como configuración global por defecto para todos los
//     hilos, sin necesidad de ninguna llamada API adicional.
struct ParallelConfig {
    int ompNumThreads = 1;
    int backendThreads = 1;
    int minParallelBatchSize = 100;
    int persistChunkSize = 500;
    std::string parallelismLabel;

    // Solo informativos para /health (reflejan el valor de la variable de
    // entorno correspondiente; el runtime de OpenMP es quien realmente los
    // aplica, no este struct).
    bool ompDynamicDisabled = true;
    int ompMaxActiveLevels = 1;
    std::string ompProcBind;
    std::string ompPlaces;
};

const ParallelConfig& config();

}  // namespace cf::parallel
