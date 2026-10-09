add_library(libasar STATIC
    src/asar.cpp
    src/json.cpp
    src/sha256.cpp
)
target_include_directories(libasar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# asar.dll - C API for external consumers (link asar.lib / LoadLibrary).
add_library(asarlib SHARED
    src/asar.cpp
    src/json.cpp
    src/sha256.cpp
    src/capi.cpp
)
set_target_properties(asarlib PROPERTIES OUTPUT_NAME "asar")
target_compile_definitions(asarlib PRIVATE ASAR_BUILDING_DLL)
target_include_directories(asarlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
