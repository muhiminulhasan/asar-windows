add_executable(asar-cli
    main.cpp
)
set_target_properties(asar-cli PROPERTIES OUTPUT_NAME "asar")
target_link_libraries(asar-cli PRIVATE libasar)
