# community-followup-service — versión híbrida C/C++

Esta versión mejora el microservicio para alinearlo mejor con `CAPA1_OperacionAPS_ESPE.pdf`.

## Separación por responsabilidad

- `controllers/` en C: define rutas base del controlador de seguimiento.
- `services/` en C: reglas simples del servicio, como estado por defecto.
- `repositories/` en C: construcción de filtros para `followup_records`.
- `validators/` en C: validación de estado de cumplimiento, fechas y campos obligatorios.
- `database/` en C: definición de tabla y campos permitidos.
- `events/` en C + JSON: construcción de eventos APS `followup.created`, `followup.updated` y `followup.completed`.
- `workflow-engine/` en C++: flujo longitudinal y finalización del seguimiento.
- `compliance-engine/` en C++: evaluación de incumplimientos y alertas.

## Comandos

```bash
docker compose up --build
```

Pruebas rápidas:

```bash
curl http://localhost:8084/health
curl http://localhost/followups
curl http://localhost/compliance/alerts
```

## Evidencia de alineación con CAPA1

El servicio conserva la tabla principal `followup_records`, expone endpoints REST de seguimiento y cumplimiento, se ejecuta en Docker, usa Traefik como gateway y contiene carpetas/archivos reales para la arquitectura híbrida C/C++ propuesta.
