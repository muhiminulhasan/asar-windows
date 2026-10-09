# 7-Zip archive format plugin. Loaded from 7-Zip's Formats/ or Codecs/
# subfolder; must not link against anything beyond the CRT and oleaut32.
add_library(7z-asar MODULE
    src/plugin.cpp
    src/7z-asar.def
)
set_target_properties(7z-asar PROPERTIES PREFIX "" OUTPUT_NAME "7z-asar")
target_include_directories(7z-asar PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/7zip
    ${CMAKE_SOURCE_DIR}/libasar/include
)
target_link_libraries(7z-asar PRIVATE libasar oleaut32 ole32)
