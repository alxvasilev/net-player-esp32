idf_component_register(SRC_DIRS . INCLUDE_DIRS . REQUIRES libogg)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -std=gnu++17 -Wno-incompatible-pointer-types)
