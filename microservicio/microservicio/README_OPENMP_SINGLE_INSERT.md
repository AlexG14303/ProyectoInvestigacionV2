# OpenMP con inserción única — `community-followup-service`

Este documento describe el enfoque **actual y principal** de paralelización: evaluación interna con OpenMP dentro de `POST /followups`, con una sola inserción por request. Reemplaza al enfoque de lotes como prueba principal.

> **La paralelización principal se aplica en la evaluación interna del seguimiento comunitario dentro del endpoint `POST /followups`. Cada request procesa un único seguimiento y realiza una única inserción en base de datos.**

## 1. Por qué no se usa batch como prueba principal

Dos razones, no una sola:

1. **La consigna del profesor** exige que la prueba principal sea 1 request → 1 seguimiento → 1 inserción. Los endpoints de lote (`/followups/batch`, `/followups/batch/validate`) procesan arreglos de registros por request, lo cual no cumple esa condición.
2. **Uso de memoria bajo carga real.** Con la cantidad fija de hilos externos de JMeter (200, según la consigna), cada request de lote mantiene en memoria un arreglo completo de objetos JSON mientras se procesa. Esa memoria escala con `tamaño del lote × peticiones concurrentes`: con 200 requests de lote concurrentes, el uso de RAM se dispara y el proceso puede colgarse. La inserción individual, en cambio, siempre mueve un solo registro por request sin importar cuántos hilos de JMeter estén disparando en paralelo — el uso de memoria se mantiene plano en vez de escalar con el volumen total de datos. Esto reproduce el comportamiento de memoria del microservicio original (sin lotes).

Los endpoints de lote **siguen existiendo** en el código como herramientas auxiliares (útiles para otros experimentos), pero `/health` los marca explícitamente como secundarios y ninguna documentación los presenta como la prueba principal.

## 2. Dónde se aplica OpenMP ahora

Todo el paralelismo vive en **`src/domain/followup_evaluation_engine.cpp`**, en la función `evaluateFollowup()`, llamada una vez por cada `POST /followups`, **antes** de la inserción.

El motor evalúa **80 reglas** del dominio de seguimiento comunitario:

| Categoría | Cantidad | Ejemplo |
|---|---|---|
| Estado de cumplimiento | 3 | `NO_CUMPLE` → sube riesgo, baja cumplimiento |
| Compromiso familiar (débil/fuerte/estructural) | 11 | `"no dispone"` en `family_commitment` → alerta |
| Compromiso del equipo de salud | 5 | `health_team_commitment` vacío → alerta operativa |
| Causas críticas de incumplimiento (por palabra clave) | 15 | `"violencia"`, `"negligencia"`, `"abandono"`, ... en `noncompliance_causes` |
| Términos críticos en descripción de riesgo | 15 | mismo tipo de término, buscado en `risk_description` |
| Lenguaje de urgencia | 5 | `"urgente"`, `"emergencia"`, `"critico"`, ... |
| Retraso por fecha (`evaluation_date`) | 5 | ≥7, ≥15, ≥30, ≥60, ≥90 días |
| Responsable / evaluación de riesgo vinculada | 3 | sin `responsible_staff_id` → alerta |
| Actividades programadas | 6 | vacías, muy cortas, o sin tipo de actividad esperado |
| Calidad del registro | 6 | incumplimiento sin causas documentadas, etc. |
| **Subtotal independiente (paralelo)** | **74** | |
| Combinación / escalamiento | 6 | riesgo alto + incumplimiento → prioridad URGENTE |
| **Total** | **80** | |

Las 74 reglas independientes se reparten con:

```cpp
#pragma omp parallel for num_threads(threads) schedule(dynamic, 8)
for (int i = 0; i < kIndependentRuleCount; ++i) {
    outcomes[i] = evaluateIndependentRule(i, ctx);
}
```

Cada hilo escribe únicamente en su propio índice de un `std::array` preasignado (`outcomes[i]`) — no hay `push_back` concurrente ni mutex. Cada regla solo **lee** un `RuleContext` construido una sola vez antes de la región paralela (texto ya normalizado a minúsculas, para no repetir ese trabajo 74 veces).

Las **6 reglas de combinación** (por ejemplo, "riesgo alto + incumplimiento → prioridad URGENTE") corren **después**, de forma **secuencial a propósito**: dependen del resultado ya agregado de las 74 anteriores (riesgo total, cantidad de alertas), así que no pueden evaluarse de forma independiente sin introducir una dependencia de datos entre iteraciones. Esto no es una limitación oculta — está documentado en el propio código y aquí.

## 3. Qué sigue siendo secuencial

- La validación básica del payload (`cf::domain::cleanAndValidateFollowup`) — sin cambios, sigue siendo simple y secuencial.
- La normalización de texto (mayúsculas→minúsculas) antes de las reglas — se hace una sola vez, no dentro del `parallel for`.
- Las 6 reglas de combinación, por la razón explicada arriba.
- La inserción en PostgREST — I/O de red, fuera de cualquier región OpenMP.

## 4. Cómo se garantiza una sola inserción por request

`POST /followups` llama a `pgClient->insert(table, data)` **una vez**, después de que termina toda la evaluación. No hay ninguna llamada a PostgREST dentro de `evaluateFollowup()` ni dentro de la región `#pragma omp parallel for`. El flujo real en `main.cpp`:

```text
cleanFollowupPayload(...)      -> validación secuencial
cf::domain::evaluateFollowup() -> evaluación paralela (80 reglas)
pgClient->insert(...)          -> UNA sola inserción
enriquecer respuesta            -> agregar riesgo/alertas/métricas al JSON de salida
```

## 5. Qué se guarda en la base de datos y qué solo se devuelve

La tabla `followup_records` **no se modificó** — no se agregaron columnas nuevas, para no arriesgar el esquema existente ni la compatibilidad con PostgREST. Por eso:

- **Se inserta**: exactamente los mismos campos que antes (`family_id`, `record_number`, `compliance_status`, etc.) — el registro insertado en base es idéntico en forma al de la versión anterior.
- **Solo se devuelve en la respuesta HTTP** (no se persiste): `risk_score`, `risk_level`, `compliance_score`, `priority`, `alerts`, `recommendations`, `parallel_metrics`. Si más adelante se necesita conservar estos valores, la opción más simple sería agregar una columna `JSONB` (por ejemplo `evaluation_metadata`) a la tabla — no se hizo aquí para no tocar el esquema sin que usted lo decida explícitamente.

Ejemplo de respuesta de `POST /followups`:

```json
{
  "followup_id": 123,
  "family_id": 10,
  "compliance_status": "NO_CUMPLE",
  "risk_score": 82,
  "risk_level": "ALTO",
  "compliance_score": 15,
  "priority": "URGENTE",
  "alerts": ["Incumplimiento familiar detectado", "..."],
  "recommendations": ["Programar visita domiciliaria prioritaria", "..."],
  "parallel_metrics": {
    "parallel_enabled": true,
    "threads_used": 4,
    "rules_evaluated": 80,
    "rules_triggered": 12,
    "processing_time_ms": 0.6
  },
  "aps_event": { "...": "..." }
}
```

## 6. Bug corregido: `aps_event` nunca se agregaba

El código anterior comparaba `created.is_array()`, pero `PostgRestClient::insert()` **siempre** devuelve un objeto (internamente hace `result[0]` si PostgREST respondió con un arreglo de un elemento). Esa condición nunca era verdadera, así que `aps_event` jamás se agregaba a la respuesta aunque el código pareciera hacerlo — no rompía nada visible, simplemente el campo faltaba en silencio. Se corrigió a `created.is_object()`.

## 7. Cómo ejecutar p01, p02, p04, p06, p08

```bash
docker compose down

OMP_NUM_THREADS=1 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p01 docker compose up --build -d
OMP_NUM_THREADS=2 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p02 docker compose up --build -d
OMP_NUM_THREADS=4 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p04 docker compose up --build -d
OMP_NUM_THREADS=6 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p06 docker compose up --build -d
OMP_NUM_THREADS=8 BACKEND_THREADS=1 CF_PARALLELISM_LEVEL=p08 docker compose up --build -d
```

Confirme cada corrida con `curl http://localhost:8084/health` antes de lanzar JMeter — revise que `openmp.omp_num_threads_configured` coincida con el nivel esperado.

Scripts de PowerShell (pruebas rápidas de sanidad, no reemplazan la corrida grande con JMeter):

```powershell
./scripts/test_single_insert_p01.ps1 -Requests 2000
./scripts/test_single_insert_p04.ps1 -Requests 2000
# ... o la barrida completa:
./scripts/test_parallel_levels_single_insert.ps1 -RequestsPerRun 2000 -Repeats 3
```

> Estos scripts se escribieron con cuidado pero no se pudieron ejecutar en el entorno donde se generó este proyecto (Linux, sin PowerShell instalado) — a diferencia del código C++, que sí se compiló y probó en vivo repetidas veces. Revíselos una vez antes de una corrida grande.

## 8. `OMP_NUM_THREADS` vs. `BACKEND_THREADS` vs. `CF_PARALLELISM_LEVEL`

| Variable | Qué controla | Ejemplo |
|---|---|---|
| `OMP_NUM_THREADS` | Hilos que reparten las 74 reglas **dentro de cada request** | `4` |
| `BACKEND_THREADS` | Cuántos requests HTTP se atienden **en simultáneo** (independiente de OpenMP) | `1` |
| `CF_PARALLELISM_LEVEL` | Etiqueta puramente informativa para trazabilidad (`/health`, logs) | `"p04"` |

**Para medir OpenMP de forma limpia** (recomendado para este experimento): fije `BACKEND_THREADS=1` y varíe solo `OMP_NUM_THREADS`. Si además varía `BACKEND_THREADS`, documente ambos valores explícitamente — el tiempo resultante ya mide concurrencia + paralelismo combinados, no solo OpenMP.

## 9. Cómo comprobar que OpenMP está activo en `/health`

```bash
curl http://localhost:8084/health
```

```json
{
  "openmp": {
    "enabled": true,
    "omp_num_threads_configured": 4,
    "omp_max_threads_runtime": 4,
    "dynamic": false,
    "max_active_levels": 1,
    "proc_bind": "TRUE",
    "places": "cores",
    "parallelism_label": "p04"
  },
  "backend_threads": 1,
  "evaluation_engine": {
    "enabled": true,
    "rules_configured": 80,
    "mode": "single_insert_parallel_evaluation"
  }
}
```

Si `omp_num_threads_configured` no coincide con lo que acaba de fijar al levantar Docker, el contenedor probablemente no se recreó (recuerde: la variable se lee una sola vez al iniciar el proceso).

## 10. Cómo interpretar los resultados — incluyendo una limitación real que debe reportar

Esto es importante y se lo decimos de frente: **es muy probable que no vea una mejora de tiempo total medible entre p01 y p08**, y eso no significa que la implementación esté mal hecha. Dos razones, medidas en este mismo proyecto antes de entregarlo:

1. **El bloque paralelizado es intrínsecamente pequeño.** 74 reglas por registro, corridas 500 000 veces en un microbenchmark aislado, mostraron que el tiempo secuencial de las 74 reglas (~2.9 microsegundos) es demasiado chico frente al costo de sincronizar un equipo de hilos OpenMP — el mismo patrón de overhead que ya se había medido con las 12 validaciones originales, aunque algo más diluido. Comparando contra un benchmark de agregación por reducción hecho en este mismo proyecto, el punto donde el overhead de OpenMP se vuelve insignificante (bajo el 3%) está alrededor de 10 000 elementos procesados en paralelo — 74 reglas está muy por debajo de ese umbral, incluso en hardware con núcleos de sobra.
2. **La inserción en PostgREST domina el tiempo total de cada request.** La evaluación completa (secuencial) cuesta microsegundos; una inserción por red hacia PostgREST/PostgreSQL típicamente cuesta milisegundos — 1000 a 3000 veces más. Aunque la fase de evaluación fuera instantánea, el tiempo total de `POST /followups` no se movería de forma perceptible.

**Qué SÍ es válido reportar:** que se implementó paralelismo real sobre trabajo genuino del dominio (no un bucle artificial), con una sola inserción por request tal como pidió el profesor, que el resultado es funcionalmente correcto y determinista (mismo `risk_score`, mismas alertas, sin importar el número de hilos — verificado en este proyecto antes de entregarlo), y que se determinó empíricamente que el tamaño natural de este bloque es insuficiente para mostrar mejora, con el cuello de botella real identificado en la inserción, no en el cómputo. Esa es una conclusión metodológicamente sólida, no un fracaso del experimento.

**Qué mirar en las corridas igualmente:**
- `parallel_metrics.processing_time_ms` en la respuesta de cada `POST /followups` — es la métrica más aislada del efecto de OpenMP en sí (aunque, por lo anterior, es esperable que no mejore con más hilos, o incluso empeore levemente).
- El tiempo total por request medido por JMeter/los scripts — refleja sobre todo `BACKEND_THREADS` (concurrencia HTTP) y la latencia de PostgREST, no el motor de reglas.
- **Antes que nada**, confirme que `risk_score`, `risk_level` y la cantidad de alertas son **idénticos** entre niveles para el mismo registro de entrada. Si no lo son, hay un problema de corrección que hay que resolver antes de hablar de rendimiento.

## 11. Por qué p08 puede no ser mejor que p06 o p04 (incluso sin la limitación de tamaño de bloque)

Aplican las mismas causas generales de cualquier experimento de paralelismo (ver también `README_OPENMP_OPTIMIZATION.md`, sección 10, escrita para los endpoints de lote): núcleos lógicos disponibles en el entorno de prueba, la fracción secuencial inevitable (reglas de combinación + construcción de la respuesta + la propia inserción), el costo de `schedule(dynamic, 8)` compitiendo por bloques de trabajo con más hilos activos, y en este caso específico, el hecho adicional de que 74 elementos es un lote demasiado chico para amortizar ese costo en cualquier escenario.

## 12. Limitar cuántos núcleos físicos/lógicos usa el contenedor (`CPU_SET` y `CPU_LIMIT`)

`OMP_NUM_THREADS` y `BACKEND_THREADS` son límites de **software**: le dicen al programa cuántos hilos crear. Ninguno de los dos controla en qué CPU física termina corriendo cada hilo — eso lo decide libremente el planificador del sistema operativo. Hay dos variables de **kernel** que sí controlan eso, y son complementarias, no redundantes:

```yaml
cpuset: ${CPU_SET:-0-11}
deploy:
  resources:
    limits:
      cpus: "${CPU_LIMIT:-12}"
```

- **`CPU_SET` (afinidad)** — a **cuáles** CPUs concretas tiene permitido correr el contenedor. Es lo que hace que `nproc` adentro del contenedor cambie.
- **`CPU_LIMIT` (cuota)** — **cuánto** tiempo de cómputo total puede consumir, como número fraccionario (equivalente a `--cpus` de `docker run`). Aunque el contenedor tenga acceso a 4 CPUs vía `CPU_SET`, esta cuota puede topar el uso real a, por ejemplo, 2.5 núcleos-tiempo dentro de esas 4 — útil si quiere simular un límite más fino que "núcleos enteros".

Por defecto (sin definir ninguna de las dos) usa las 12 CPUs lógicas de esta máquina sin cuota adicional — es decir, **sin restricción**, igual que antes de agregar estas líneas.

**Importante — esta máquina tiene 6 núcleos físicos, no 12.** `lscpu`/`/proc/cpuinfo` confirmaron que cada par consecutivo de CPUs lógicas comparte núcleo físico (hyperthreading 2 vías):

| Núcleo físico | CPUs lógicas |
|---|---|
| 0 | 0, 1 |
| 1 | 2, 3 |
| 2 | 4, 5 |
| 3 | 6, 7 |
| 4 | 8, 9 |
| 5 | 10, 11 |

Valores recomendados para un experimento de "cuántos núcleos físicos hacen falta" (**no** para la corrida grande de JMeter — ver advertencia abajo). `CPU_LIMIT` se fija igual a la cantidad de núcleos que aparecen en `CPU_SET`, para que ambos límites sean consistentes entre sí:

| Nivel | `CPU_SET` | `CPU_LIMIT` | Núcleos físicos reales |
|---|---|---|---|
| p01 | `0` | `1` | 1 |
| p02 | `0,2` | `2` | 2 |
| p04 | `0,2,4,6` | `4` | 4 |
| p06 | `0,2,4,6,8,10` | `6` | 6 (el máximo físico disponible) |
| p08 | `0,2,4,6,8,10,1,3` | `8` | 6 físicos + 2 hilos SMT repetidos — sobre-suscripción real, a propósito |

```powershell
$env:OMP_NUM_THREADS="4"; $env:CF_PARALLELISM_LEVEL="p04"; $env:CPU_SET="0,2,4,6"; $env:CPU_LIMIT="4"
docker compose up --build -d --force-recreate community-followup-service
docker compose exec community-followup-service nproc   # debe devolver 4
```

**Advertencia — no use `CPU_SET`/`CPU_LIMIT` en las corridas grandes de 250K-1M con `BACKEND_THREADS` alto.** Ambos restringen el contenedor completo, incluidos los hilos HTTP. Con `BACKEND_THREADS=65` y estos límites bajados a 1-2, tendría 65 hilos compitiendo por 1-2 núcleos físicos — reintroduciría el mismo problema de cola que se resolvió en la sesión de depuración, mezclando el efecto de núcleos físicos con el de sobre-concurrencia HTTP en la misma medición. Úselos solo en una prueba aparte y chica (2000-5000 peticiones), con `BACKEND_THREADS` bajado para que coincida con cada nivel.

**Nota sobre `deploy.resources.limits`:** esta sintaxis históricamente era exclusiva de Docker Swarm, pero el CLI moderno `docker compose` (v2, el que invoca con espacio, no el viejo `docker-compose` con guion) sí la aplica en `docker compose up` normal, sin necesidad de swarm. Antes de una corrida grande, valide igual con `docker compose config` para confirmar que su versión de Docker Desktop lo interpreta como se espera.

---

## Resumen de cambios respecto a la versión de lotes

**Archivos nuevos:**
- `src/domain/followup_evaluation_engine.h` / `.cpp`
- `scripts/_single_insert_runner.ps1` (motor común reutilizado por los 5 scripts de nivel)
- `scripts/test_single_insert_p01.ps1` ... `p08.ps1`
- `scripts/test_parallel_levels_single_insert.ps1`
- Este archivo

**Archivos modificados:**
- `src/main.cpp` — `POST /followups` ahora evalúa con OpenMP antes de insertar, corrige el bug de `aps_event`, enriquece la respuesta; `/health` agrega el bloque `evaluation_engine` y marca los endpoints de lote como secundarios
- `community-followup-service/CMakeLists.txt` — agrega `followup_evaluation_engine.cpp`
- `docker-compose.yml` — referencia de documentación actualizada

**Sin cambios:**
- Los endpoints de lote (`/followups/batch`, `/followups/batch/validate`, `/followups/analytics/summary`) siguen existiendo, documentados en `README_OPENMP_OPTIMIZATION.md`, pero ya no son la prueba principal — ver nota agregada al inicio de ese archivo.
