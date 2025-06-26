idf_component_register(SRC_DIRS . INCLUDE_DIRS . REQUIRES libogg)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-incompatible-pointer-types -fno-strict-aliasing -Wcast-align -Wstrict-aliasing=2)
