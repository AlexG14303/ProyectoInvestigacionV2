# Optimización de la paralelización OpenMP — `community-followup-service`

> **Nota (lea esto primero):** este documento describe los endpoints de **lote** (`/followups/batch`, `/followups/batch/validate`, `/followups/analytics/summary`). Ya **no son la prueba principal** del laboratorio — el profesor exige una sola inserción por request, y además el uso de memoria de los endpoints de lote escala con el tamaño del lote × la concurrencia de JMeter, lo cual puede agotar la RAM disponible con la cantidad de hilos externos fija que pide la consigna. La prueba principal ahora es `POST /followups` con evaluación paralela interna e inserción única — ver **`README_OPENMP_SINGLE_INSERT.md`**. Este documento se conserva porque los endpoints de lote siguen existiendo en el código como herramientas secundarias/opcionales.

Este documento explica qué tenía de limitado la paralelización anterior, qué se cambió, dónde vive ahora OpenMP, cómo compilar y probar, y cómo interpretar los resultados de las corridas p01–p08.

---

## 1. Qué estaba mal o limitado en la paralelización anterior

La versión anterior aplicaba `#pragma omp parallel for` sobre las ~12 validaciones de campo de **un único** payload JSON, dentro de `POST /followups`. Eso tiene tres problemas de fondo, no de implementación:

1. **Grano demasiado fino.** Validar 12 campos (comparaciones de enteros, longitud de strings, un par de comparaciones de texto) cuesta del orden de 1–2 microsegundos en total. Crear y sincronizar un equipo de hilos OpenMP cuesta más que eso, incluso con el equipo de hilos ya "caliente". El resultado medido en pruebas previas: la versión paralela era **más lenta** que la secuencial en todos los niveles de hilos probados, y empeoraba progresivamente con más hilos.
2. **El endpoint individual no necesita paralelismo interno.** Una sola petición HTTP con un solo registro no tiene trabajo suficiente para dividir entre cores. El paralelismo, si va a existir, debe aplicarse donde hay **muchos** elementos independientes que procesar a la vez — no dentro del manejo de una petición.
3. **No había ningún endpoint que generara carga de CPU real y medible.** Todo el resto del proyecto (repositorios, servicios, workflow-engine, compliance-engine) delega a PostgREST/PostgreSQL: son operaciones de un solo registro, dominadas por red, no por cómputo.

## 2. Qué se cambió

| Antes | Ahora |
|---|---|
| `POST /followups` paralelizaba 12 validaciones de UN registro | `POST /followups` es simple y secuencial: valida → inserta → responde |
| No existía forma de medir CPU puro con OpenMP | `POST /followups/batch/validate` valida/normaliza/clasifica lotes completos, sin tocar la base de datos |
| No existía ingreso por lotes | `POST /followups/batch` valida en paralelo y persiste en bloques controlados |
| No existía análisis agregado | `GET /followups/analytics/summary` calcula estadísticas con `reduction` de OpenMP |
| `omp_set_num_threads()` en el hilo principal (no se propaga a los hilos que atienden requests) | `OMP_NUM_THREADS` se lee una sola vez de la variable de entorno y se pasa explícito con `num_threads(...)` en cada región paralela |
| Todo el código en `main.cpp` | Nueva estructura modular: `src/parallel/` y `src/domain/` |
| Sin umbral: OpenMP se usaba siempre, aunque el lote fuera pequeño | `MIN_PARALLEL_BATCH_SIZE` decide, vía cláusula `if()` de OpenMP, si conviene paralelizar |

## 3. Dónde se aplica OpenMP ahora

Todo el paralelismo real vive en **`src/parallel/followup_batch_processor.cpp`**, en dos funciones:

- **`processBatchCpuPhase`** — usada por `POST /followups/batch/validate` y `POST /followups/batch`. Reparte con `schedule(dynamic, 64)` la validación + normalización + clasificación de riesgo de cada registro del lote (el costo por registro no es uniforme: uno inválido corta temprano con una excepción, uno válido corre las 12 validaciones completas). Luego hace un segundo paso de conteo con `reduction(+:ok,bad,alto,medio,bajo)` y `schedule(static)`, porque ahí el costo por elemento sí es uniforme.
- **`computeAnalyticsSummary`** — usada por `GET /followups/analytics/summary`. Un solo `parallel for` con `reduction(+:...)` sobre siete acumuladores (total, cumplidos, no-cumplidos, parciales, riesgo alto/medio/bajo).

Ninguna de las dos hace llamadas HTTP ni de base de datos dentro de la región paralela. La persistencia (`POST /followups/batch`, fase 3) ocurre **después**, fuera del `parallel for`, en bloques secuenciales de `PERSIST_CHUNK_SIZE` registros usando `PostgRestClient::insertMany()` (inserción masiva vía PostgREST — un solo POST con un arreglo, en vez de N POST individuales).

El endpoint individual (`POST /followups`, `PATCH /followups/{id}`) llama a `cf::domain::cleanAndValidateFollowup()`, que es **puramente secuencial** — sin ningún `#pragma omp` involucrado.

### Protección contra condiciones de carrera

- **Nada de `push_back` concurrente.** `BatchProcessResult::items` se preasigna con `resize(n)` antes del `parallel for`; cada hilo escribe únicamente en `items[i]`, su propio índice.
- **Nada de mutex en la fase de conteo.** Se usa `reduction`, que le da a cada hilo su propia copia local de los acumuladores y las combina automáticamente al final — sin bloqueos manuales.
- **Nada de estado compartido mutable entre iteraciones.** Cada iteración del `parallel for` lee su propio elemento de entrada y escribe su propio elemento de salida; no hay dependencias entre iteraciones.

## 4. Por qué ahora sí es medible

Con `MIN_PARALLEL_BATCH_SIZE=100` (valor por defecto) y lotes de 1 000–1 000 000 de registros, cada hilo procesa cientos o miles de registros, no 3 campos. El overhead de crear el equipo de hilos (del orden de microsegundos) queda diluido entre un volumen de trabajo que sí escala con el tamaño del lote. Esto se verificó de forma empírica al desarrollar este proyecto: con un lote de 5 000 registros, la fase de validación paralela devuelve resultados **idénticos** en p01, p02 y p08 (mismo `records_valid`, mismo `risk_summary`), confirmando que la reducción y el reparto de trabajo son correctos independientemente del número de hilos.

## 5. Cómo compilar

```bash
docker compose build community-followup-service
```

o, para reconstruir todo el stack:

```bash
docker compose up --build -d
```

El `CMakeLists.txt` del servicio ahora incluye `find_package(OpenMP REQUIRED)` y enlaza `OpenMP::OpenMP_CXX`; el `Dockerfile` instala `libgomp1` en la imagen final de runtime (la librería compartida de OpenMP de GCC).

## 6. Cómo ejecutar y probar los endpoints

### 6.1. Endpoint individual (sin cambios de comportamiento)

```bash
curl -X POST http://localhost:8084/followups \
  -H "Content-Type: application/json" \
  -d '{"family_id": 1, "record_number": "SEG-001", "compliance_status": "PARCIAL"}'
```

### 6.2. Validación masiva (CPU puro, sin persistir)

```bash
pwsh scripts/test_batch_validate.ps1 -BatchSize 10000
```

o directamente:

```bash
curl -X POST http://localhost:8084/followups/batch/validate \
  -H "Content-Type: application/json" \
  --data @lote.json
```

donde `lote.json` tiene la forma `{"records": [ {...}, {...}, ... ]}`.

### 6.3. Ingreso por lotes (valida + inserta de verdad)

```bash
pwsh scripts/test_batch_insert.ps1 -BatchSize 10000
```

**Atención:** este script sí escribe filas nuevas en `followup_records`.

### 6.4. Resumen analítico

```bash
# Con datos reales de la base:
curl http://localhost:8084/followups/analytics/summary

# Con datos simulados en memoria (no toca la base), para probar volumen
# controlado aunque la tabla real tenga pocos registros:
curl "http://localhost:8084/followups/analytics/summary?simulate=500000"
```

### 6.5. Barrido completo p01 → p08

```bash
pwsh scripts/test_parallel_levels.ps1 -BatchSize 10000 -BackendThreads 1
```

Este script reinicia el contenedor con cada nivel de `OMP_NUM_THREADS`, confirma la configuración vía `/health`, corre la prueba y guarda `resultado_barrida_openmp.csv`.

> **Nota:** los tres scripts están en PowerShell porque así se pidieron. Se escribieron con cuidado pero no pudieron ejecutarse durante el desarrollo (el entorno de generación es Linux, sin PowerShell instalado) — a diferencia del código C++, que sí se compiló y se probó en vivo. Revíselos una vez antes de una corrida grande.

## 7. Cómo probar p01, p02, p04, p06, p08 manualmente

```bash
docker compose down

OMP_NUM_THREADS=1 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p01 docker compose up --build -d
OMP_NUM_THREADS=2 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p02 docker compose up --build -d
OMP_NUM_THREADS=4 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p04 docker compose up --build -d
OMP_NUM_THREADS=6 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p06 docker compose up --build -d
OMP_NUM_THREADS=8 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p08 docker compose up --build -d
```

Confirme cada corrida con:

```bash
curl http://localhost:8084/health
```

Verifique que `openmp.omp_num_threads_configured` coincide con el nivel que acaba de fijar antes de lanzar la prueba de carga.

## 8. `OMP_NUM_THREADS` vs. `BACKEND_THREADS` — no son lo mismo

| Variable | Qué controla | Afecta a |
|---|---|---|
| `OMP_NUM_THREADS` | Hilos que reparten el trabajo **dentro** de una región `#pragma omp parallel for` | `processBatchCpuPhase`, `computeAnalyticsSummary` |
| `BACKEND_THREADS` | Cuántas peticiones HTTP se atienden **en simultáneo** (tamaño del pool de hilos de httplib) | Todos los endpoints, incluido el individual |

Son ejes independientes. Puede tener `BACKEND_THREADS=8` y `OMP_NUM_THREADS=1` (mucha concurrencia externa, nada de paralelismo interno), o al revés.

- **Para medir OpenMP de forma limpia** (lo que este laboratorio necesita para comparar p01–p08): deje `BACKEND_THREADS=1` fijo y varíe solo `OMP_NUM_THREADS`. Así el tiempo que se mide en `/followups/batch/validate` refleja únicamente el paralelismo interno de la región OpenMP, sin que la concurrencia HTTP contamine la comparación.
- **Para medir una combinación realista** (concurrencia de usuarios + paralelismo interno), puede variar `BACKEND_THREADS` también — pero en ese caso, documente explícitamente ambos valores en cada corrida, porque el tiempo resultante ya no aísla el efecto de OpenMP por sí solo.

`CF_PARALLELISM_LEVEL` es una tercera variable, puramente informativa (aparece en `/health` y en las respuestas de los endpoints de lote como `parallelism_label`) — sirve para etiquetar la corrida (`p01`, `p04`, etc.) en sus reportes, pero no controla ningún hilo. La fuente de verdad del número de hilos es siempre `OMP_NUM_THREADS`.

## 9. Advertencias sobre sobrecarga (overhead)

- **`MIN_PARALLEL_BATCH_SIZE`** (por defecto 100): por debajo de este tamaño de lote, el código usa la cláusula `if()` de OpenMP para ejecutar secuencialmente, sin crear ningún equipo de hilos. No baje demasiado este umbral pensando que "más paralelo siempre es mejor" — con lotes de unas pocas decenas de registros, el overhead de sincronización puede seguir superando la ganancia.
- **`OMP_MAX_ACTIVE_LEVELS=1`**: evita paralelismo anidado. Si en algún momento se agrega una región `#pragma omp parallel` dentro de otra ya activa, esta variable evita que el número de hilos se multiplique sin control.
- **`OMP_PROC_BIND=TRUE` / `OMP_PLACES=cores`**: fijan cada hilo a un core físico, reduciendo la varianza entre corridas causada por el planificador del sistema operativo moviendo hilos entre cores. Recomendado para comparaciones p01–p08 reproducibles.
- **Persistencia en bloques (`PERSIST_CHUNK_SIZE`, por defecto 500)**: al insertar en PostgREST, un solo POST con 250 000–1 000 000 de registros en el body sería poco práctico (tamaño de payload, timeouts). Por eso `POST /followups/batch` divide la inserción en bloques de este tamaño, en un bucle **secuencial** fuera de la región OpenMP — la fase de persistencia es I/O-bound y no se beneficia de más hilos de CPU.

## 10. Cuándo p08 puede no ser mejor que p04 (o que p06)

Esto es esperable y no indica necesariamente un error de implementación:

- **Núcleos físicos disponibles.** Si el contenedor o la máquina tienen menos de 8 cores lógicos, p08 sobre-asigna hilos: varios hilos compiten por el mismo core, y el tiempo puede estabilizarse o empeorar frente a p06 o p04. Antes de interpretar p08 como "peor", confirme cuántos cores lógicos tiene disponibles el entorno de prueba.
- **Ley de Amdahl.** Aunque la fase de validación es mayormente paralelizable, hay una fracción secuencial inevitable (la fase de conteo final, la construcción de la respuesta JSON, y en `POST /followups/batch`, toda la fase de persistencia). Esa fracción secuencial pone un techo a la mejora posible sin importar cuántos hilos se agreguen.
- **`schedule(dynamic, 64)` tiene su propio costo.** Cada vez que un hilo pide un nuevo bloque de 64 elementos hay una pequeña sincronización. Con más hilos, hay más solicitudes de bloques compitiendo, lo que puede acercar el rendimiento entre p06 y p08 más de lo que uno esperaría ingenuamente.
- **Memoria y caché.** Con lotes de cientos de miles de registros, el cuello de botella puede dejar de ser "cuántos hilos calculan" y pasar a ser "cuánto tarda la memoria en alimentar a esos hilos" (ancho de banda de memoria compartido entre cores). Esto también aplana la curva en los niveles altos de paralelismo.

**Qué mirar para comprobar que la mejora es real y no un artefacto:** compare `processing_time_ms` y `records_per_second` entre niveles, pero también revise que `records_valid` y `risk_summary` sean **idénticos** en todos los niveles para el mismo lote de entrada (si no lo son, hay un problema de corrección, no de rendimiento — deténgase y revise antes de seguir midiendo velocidad). Si el tiempo dejó de bajar en un nivel intermedio, es señal de haber llegado al punto de saturación de esta configuración de hardware, no de que OpenMP "no funcione".

---

## Resumen de cambios (referencia rápida)

**Archivos nuevos:**
- `src/parallel/parallel_config.h` / `.cpp`
- `src/parallel/followup_batch_processor.h` / `.cpp`
- `src/domain/followup_validator.h` / `.cpp`
- `src/domain/followup_analyzer.h` / `.cpp`
- `scripts/test_batch_validate.ps1`
- `scripts/test_batch_insert.ps1`
- `scripts/test_parallel_levels.ps1`
- Este archivo, `README_OPENMP_OPTIMIZATION.md`

**Archivos modificados:**
- `src/main.cpp` — endpoint individual simplificado a secuencial puro; 3 endpoints nuevos; `/health` ampliado
- `shared/include/PostgRestClient.hpp` — se agregó `insertMany()` (inserción masiva por arreglo); `insert()` original queda intacto
- `community-followup-service/CMakeLists.txt` — nuevos módulos + `find_package(OpenMP REQUIRED)`
- `community-followup-service/Dockerfile` — `libgomp1` en la imagen final
- `docker-compose.yml` — variables `OMP_NUM_THREADS`, `BACKEND_THREADS`, `CF_PARALLELISM_LEVEL`, `OMP_DYNAMIC`, `OMP_MAX_ACTIVE_LEVELS`, `OMP_PROC_BIND`, `OMP_PLACES`, `MIN_PARALLEL_BATCH_SIZE`, `PERSIST_CHUNK_SIZE`

**Endpoints nuevos:**
- `POST /followups/batch/validate` — validación + normalización + clasificación de riesgo en paralelo, sin persistir
- `POST /followups/batch` — igual que el anterior, más inserción controlada por bloques
- `GET /followups/analytics/summary` — agregados con `reduction` OpenMP (parámetro opcional `?simulate=N`)

**Endpoint sin cambios de comportamiento (solo se movió el código a `src/domain/`):**
- `POST /followups`, `PATCH /followups/{id}`, `GET /followups`, `GET /followups/{id}`, `DELETE /followups/{id}`, `POST /followups/{id}/complete`
