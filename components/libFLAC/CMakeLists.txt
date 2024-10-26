idf_component_register(SRC_DIRS . INCLUDE_DIRS . include REQUIRES libogg)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-incompatible-pointer-types)
target_compile_definitions(${COMPONENT_LIB} PRIVATE -DHAVE_CONFIG_H=1)
