idf_component_register(SRC_DIRS . INCLUDE_DIRS . include)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
target_compile_definitions(${COMPONENT_LIB} PUBLIC -DNDEBUG=1)




