cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)

project(asar-windows VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically link the CRT so the shipped DLL/EXE files have no runtime
# dependency beyond Windows itself.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/utf-8 /W3 /EHsc)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()

add_subdirectory(libasar)
add_subdirectory(tools/asar)
add_subdirectory(7z-asar)
add_subdirectory(asarshell)
add_subdirectory(asarfolder)
add_subdirectory(asarview)
add_subdirectory(tests)
