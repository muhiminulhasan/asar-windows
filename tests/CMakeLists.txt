add_executable(test_roundtrip test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE libasar)

add_executable(test_7zclient test_7zclient.cpp)
target_include_directories(test_7zclient PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/7zip
    ${CMAKE_SOURCE_DIR}/libasar/include
)
target_link_libraries(test_7zclient PRIVATE libasar oleaut32)

add_test(NAME roundtrip COMMAND test_roundtrip)
add_test(NAME 7zclient COMMAND test_7zclient $<TARGET_FILE:7z-asar>)

add_executable(test_asarfolder test_asarfolder.cpp)
target_include_directories(test_asarfolder PRIVATE
    ${CMAKE_SOURCE_DIR}/libasar/include
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(test_asarfolder PRIVATE libasar shell32 ole32 uuid)
# The test registers asarfolder.dll from its own directory (see
# RegisterExtensionForTest), so the DLL must sit next to the exe.
add_dependencies(test_asarfolder asarfolder)
add_custom_command(TARGET test_asarfolder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:asarfolder> $<TARGET_FILE_DIR:test_asarfolder>)

add_executable(test_capi test_capi.cpp)
target_include_directories(test_capi PRIVATE
    ${CMAKE_SOURCE_DIR}/libasar/include
)
target_compile_definitions(test_capi PRIVATE UNICODE _UNICODE)
target_link_libraries(test_capi PRIVATE asarlib)
add_custom_command(TARGET test_capi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:asarlib> $<TARGET_FILE_DIR:test_capi>)
add_test(NAME capi COMMAND test_capi)

# The namespace-extension test creates the COM class registered in HKCU, so
# it can only run when the built architecture matches the host (the class is
# loaded in-process). Cross-arch builds skip it instead of failing.
# When CMake itself runs under a 32-bit wrapper on a 64-bit host, Windows
# reports the true host architecture in PROCESSOR_ARCHITEW6432.
set(ASAR_HOST_ARCH $ENV{PROCESSOR_ARCHITEW6432})
if(NOT ASAR_HOST_ARCH)
    set(ASAR_HOST_ARCH $ENV{PROCESSOR_ARCHITECTURE})
endif()
message(STATUS "asarfolder test gate: platform=${CMAKE_VS_PLATFORM_NAME} host=${ASAR_HOST_ARCH}")
if((CMAKE_VS_PLATFORM_NAME STREQUAL "x64" AND ASAR_HOST_ARCH STREQUAL "AMD64")
   OR (CMAKE_VS_PLATFORM_NAME STREQUAL "Win32" AND ASAR_HOST_ARCH STREQUAL "x86")
   OR (CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64" AND ASAR_HOST_ARCH STREQUAL "ARM64"))
    add_test(NAME asarfolder COMMAND test_asarfolder)
endif()
