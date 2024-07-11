COMPONENT_SRCDIRS := .
COMPONENT_ADD_INCLUDEDIRS += .
CXXFLAGS += -O3 -std=gnu++17
CFLAGS += -O3
CPPFLAGS=-Dmalloc=my_malloc -Drealloc=my_realloc
