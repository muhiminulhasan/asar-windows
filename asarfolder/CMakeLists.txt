# Explorer namespace extension: makes .asar files browsable inside Explorer,
# like .zip files. Registered per user under HKCU.
add_library(asarfolder SHARED
    src/folder.cpp
    src/ui.cpp
    src/reg.cpp
    src/dllmain.cpp
    src/asarfolder.def
)
set_target_properties(asarfolder PROPERTIES PREFIX "")
target_include_directories(asarfolder PRIVATE
    ${CMAKE_SOURCE_DIR}/libasar/include
)
target_link_libraries(asarfolder PRIVATE libasar shell32 shlwapi ole32 oleaut32 uuid advapi32 user32 gdi32)
