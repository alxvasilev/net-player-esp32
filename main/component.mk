#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)

#SRCDIRS := main
CXXFLAGS += -std=gnu++17
ifdef CONFIG_ESP32_SPIRAM_SUPPORT
    CPPFLAGS+=-DHELIX_FEATURE_AUDIO_CODEC_AAC_SBR=1
endif
