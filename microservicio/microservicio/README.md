# CAPA 1 APS — community-followup-service

Proyecto limpio para levantar únicamente el microservicio `community-followup-service`, alineado con `CAPA1_OperacionAPS_ESPE.pdf`.

## Arquitectura incluida

- `community-followup-service/`: microservicio de evolución y seguimiento APS.
- `shared-config/`: configuración transversal en YAML.
- `shared-events/`: contratos de eventos APS.
- `shared-security/`: políticas y auditoría.
- `shared-dto/followup/`: DTOs compatibles C/C++.
- `postgres`: base de datos con tabla `followup_records`.
- `postgrest`: API interna hacia PostgreSQL.
- `traefik`: gateway para enrutar `/followups` y `/compliance`.

## Lenguaje usado

- C: controladores básicos, servicios CRUD, repositorios, validadores, eventos y conexión/consulta de base de datos.
- C++: workflow longitudinal y motor de cumplimiento.

## Levantar el entorno

```bash
docker compose up --build
```

## Probar

```bash
curl http://localhost:8084/health
curl http://localhost/followups
curl http://localhost/compliance/alerts
```

## Endpoints principales

- `GET /health`
- `GET /followups`
- `GET /followups/{id}`
- `POST /followups`
- `PATCH /followups/{id}`
- `POST /followups/{id}/complete`
- `DELETE /followups/{id}`
- `GET /compliance/alerts`
