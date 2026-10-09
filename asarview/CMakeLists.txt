add_executable(asarview WIN32
    src/asarview.cpp
    ${CMAKE_SOURCE_DIR}/resources/app.rc
)
target_include_directories(asarview PRIVATE ${CMAKE_SOURCE_DIR}/libasar/include)
target_link_libraries(asarview PRIVATE libasar comctl32 shell32 shlwapi ole32 uuid advapi32)

if(MSVC)
    # Opt into common controls v6 for a themed list view.
    target_link_options(asarview PRIVATE
        "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'")
endif()
