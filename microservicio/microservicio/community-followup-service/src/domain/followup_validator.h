#pragma once

#include "json.hpp"

namespace cf::domain {

using nlohmann::json;

// Filtra los campos permitidos, aplica valores por defecto (compliance_status)
// y valida un ÚNICO registro de seguimiento. Lanza std::invalid_argument si
// el registro no es válido.
//
// Esta función es intencionalmente secuencial: no usa OpenMP en absoluto.
// El endpoint individual (POST /followups) la llama directamente, una vez
// por request. Los endpoints de lote (POST /followups/batch,
// POST /followups/batch/validate) también la llaman, pero desde DENTRO de
// una región paralela en cf::parallel::followup_batch_processor — el
// paralelismo real está en recorrer muchos registros a la vez, no en el
// costo de validar uno solo.
// Valida un único campo de fecha en formato YYYY-MM-DD (ausente = válido).
// Se expone aparte de cleanAndValidateFollowup porque POST /followups/{id}/complete
// arma un objeto de actualización parcial (solo compliance_status +
// evaluation_date + noncompliance_causes) y necesita validar la fecha de
// forma independiente, sin pasar por el resto de las reglas de un registro
// completo.
void validateIsoDateField(const json& data, const char* field);

json cleanAndValidateFollowup(const json& input, bool partialUpdate);

}  // namespace cf::domain
