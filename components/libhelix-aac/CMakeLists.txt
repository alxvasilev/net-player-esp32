idf_component_register(SRC_DIRS . INCLUDE_DIRS .)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-error=unused-label -Wno-error=return-type -Wno-error=missing-braces -Wno-error=pointer-sign -Wno-error=parentheses -Wno-implicit-fallthrough -Wno-stringop-overflow)
target_compile_definitions(${COMPONENT_LIB} PUBLIC -DHELIX_FEATURE_AUDIO_CODEC_AAC_SBR=1)
