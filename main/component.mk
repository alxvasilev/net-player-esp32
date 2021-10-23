#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)

CXXFLAGS += -std=c++14
CPPFLAGS+=-DHELIX_FEATURE_AUDIO_CODEC_AAC_SBR=1
