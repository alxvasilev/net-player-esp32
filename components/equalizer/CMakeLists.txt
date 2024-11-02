idf_component_register(INCLUDE_DIRS .)
add_prebuilt_library(esp_eq "libeq.a")
target_link_libraries(${COMPONENT_LIB} INTERFACE esp_eq)
