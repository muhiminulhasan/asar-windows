# Explorer shell extension (context menu handler + per-user self
# registration).
add_library(asarshell SHARED
    src/dllmain.cpp
    src/contextmenu.cpp
    src/actions.cpp
    src/registry.cpp
    src/asarshell.def
)
set_target_properties(asarshell PROPERTIES PREFIX "")
target_include_directories(asarshell PRIVATE
    ${CMAKE_SOURCE_DIR}/libasar/include
)
target_link_libraries(asarshell PRIVATE libasar shell32 shlwapi ole32 advapi32 user32 gdi32)
