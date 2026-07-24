# CMakeLists.txt para tests/ - community-followup-service
# Corrige hallazgo R007: registra los smoke tests existentes en el sistema
# de construccion para que se compilen y ejecuten como parte del build.
#
# Cada test es un ejecutable independiente y liviano, vinculado unicamente
# al modulo fuente que ejercita (sin dependencias de httplib ni PostgRestClient).

enable_testing()

# --- followup-controller-tests ---
add_executable(test_followup_controller
    followup-controller-tests.c
    ../src/controllers/followup-controller.c
)
target_include_directories(test_followup_controller PRIVATE ../src)
add_test(NAME followup_controller_test COMMAND test_followup_controller)

# --- followup-repository-tests ---
add_executable(test_followup_repository
    followup-repository-tests.c
    ../src/repositories/followup-repository.c
)
target_include_directories(test_followup_repository PRIVATE ../src)
add_test(NAME followup_repository_test COMMAND test_followup_repository)

# --- followup-service-tests ---
add_executable(test_followup_service
    followup-service-tests.c
    ../src/services/followup-service.c
    ../src/validators/followup-validator.c
)
target_include_directories(test_followup_service PRIVATE ../src ../src/validators)
add_test(NAME followup_service_test COMMAND test_followup_service)

# --- compliance-engine-tests ---
add_executable(test_compliance_engine
    compliance-engine-tests.cpp
    ../src/compliance-engine/compliance-evaluator.cpp
)
target_include_directories(test_compliance_engine PRIVATE ../src)
set_property(TARGET test_compliance_engine PROPERTY CXX_STANDARD 20)
add_test(NAME compliance_engine_test COMMAND test_compliance_engine)

# --- longitudinal-followup-tests ---
add_executable(test_longitudinal_followup
    longitudinal-followup-tests.cpp
    ../src/services/longitudinal-followup-service.cpp
)
target_include_directories(test_longitudinal_followup PRIVATE ../src)
set_property(TARGET test_longitudinal_followup PROPERTY CXX_STANDARD 20)
add_test(NAME longitudinal_followup_test COMMAND test_longitudinal_followup)

# --- workflow-engine-tests ---
add_executable(test_workflow_engine
    workflow-engine-tests.cpp
    ../src/workflow-engine/workflow-manager.cpp
)
target_include_directories(test_workflow_engine PRIVATE ../src)
set_property(TARGET test_workflow_engine PROPERTY CXX_STANDARD 20)
add_test(NAME workflow_engine_test COMMAND test_workflow_engine)

# NOTA: followup-integration-tests.cpp se excluye deliberadamente de este
# registro. Su contenido actual (int main() { return 0; }) es un placeholder
# sin logica de prueba real; compilarlo y "pasarlo" generaria una falsa
# sensacion de cobertura de pruebas de integracion. Se documenta como
# pendiente de implementacion real en software_risk_matrix.csv (R007).
