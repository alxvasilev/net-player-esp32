COMPONENT_SRCDIRS := .
COMPONENT_ADD_INCLUDEDIRS += . ./include
CFLAGS += -O3 -DHAVE_CONFIG_H -Dmalloc=my_malloc -Drealloc=my_realloc
