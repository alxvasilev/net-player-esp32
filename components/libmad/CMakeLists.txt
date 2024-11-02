idf_component_register(SRC_DIRS . INCLUDE_DIRS .)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3
    -Wno-error=unused-label -Wno-error=return-type -Wno-error=missing-braces -Wno-error=pointer-sign
    -Wno-error=parentheses -Wno-implicit-fallthrough -Wno-stringop-overflow -Wno-imcompatible-pointer-types
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE -D FPM_DEFAULT)
