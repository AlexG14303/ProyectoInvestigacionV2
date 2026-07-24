# shared/ — librería INTERFACE (solo headers, sin .cpp)
# Todos los servicios la linkean para reutilizar helpers comunes.

add_library(shared INTERFACE)

target_include_directories(shared INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
