COMPONENT_SRCDIRS := .
COMPONENT_ADD_INCLUDEDIRS += . ./include
CFLAGS += -O3 -DNDEBUG -Dmalloc=my_malloc -Drealloc=my_realloc
