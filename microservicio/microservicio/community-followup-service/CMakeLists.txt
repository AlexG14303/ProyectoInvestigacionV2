cmake_minimum_required(VERSION 3.8)
project(community-followup-service C CXX)

find_package(OpenMP REQUIRED)

add_executable(community-followup-service
    src/main.cpp
    src/parallel/parallel_config.cpp
    src/parallel/followup_batch_processor.cpp
    src/domain/followup_validator.cpp
    src/domain/followup_analyzer.cpp
    src/domain/followup_evaluation_engine.cpp
    src/controllers/followup-controller.c
    src/controllers/compliance-controller.c
    src/services/followup-service.c
    src/services/community-workflow-service.cpp
    src/services/longitudinal-followup-service.cpp
    src/repositories/followup-repository.c
    src/repositories/compliance-repository.c
    src/validators/followup-validator.c
    src/validators/compliance-validator.c
    src/validators/activity-validator.c
    src/database/db-connection.c
    src/database/followup-query-builder.c
    src/events/followup-event-publisher.c
    src/workflow-engine/workflow-manager.cpp
    src/workflow-engine/activity-scheduler.cpp
    src/workflow-engine/followup-timeline.cpp
    src/compliance-engine/compliance-evaluator.cpp
    src/compliance-engine/commitment-tracker.cpp
    src/compliance-engine/noncompliance-analyzer.cpp
)

set_property(TARGET community-followup-service PROPERTY CXX_STANDARD 20)
set_property(TARGET community-followup-service PROPERTY C_STANDARD 11)

target_include_directories(community-followup-service PRIVATE
    src
    src/parallel
    src/domain
    src/controllers
    src/services
    src/repositories
    src/validators
    src/database
    src/events
    src/workflow-engine
    src/compliance-engine
    src/dto
    ${CMAKE_SOURCE_DIR}/vendor
)

target_link_libraries(community-followup-service PRIVATE shared OpenMP::OpenMP_CXX)

# Subcarpeta de pruebas (corrige hallazgo R007)
add_subdirectory(tests)
