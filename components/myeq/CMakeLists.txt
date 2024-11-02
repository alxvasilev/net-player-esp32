idf_component_register(SRC_DIRS . INCLUDE_DIRS .)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -std=gnu++17)
