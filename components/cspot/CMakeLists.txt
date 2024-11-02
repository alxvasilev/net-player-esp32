message(STATUS srcdir=${CMAKE_CURRENT_SOURCE_DIR})
message(STATUS bindir=${CMAKE_CURRENT_BINARY_DIR})
idf_component_register(
    SRC_DIRS src bell-utils nanopb
    INCLUDE_DIRS src bell-utils/include nanopb ${CMAKE_CURRENT_BINARY_DIR}
#   ADDITIONAL_CLEAN_FILES ${CMAKE_CURRENT_BINARY_DIR}/protobuf/*.o
    REQUIRES mbedtls pthread json httpLib
)
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)
target_compile_definitions(${COMPONENT_LIB} PUBLIC -D BELL_ONLY_CJSON=1 -DPB_ENABLE_MALLOC=1)

file(GLOB PROTO_FILES protobuf/*.proto)
set(PB_SRCS)
set(PB_OBJS)
foreach(_path ${PROTO_FILES})
    set(_stem)
    get_filename_component(_stem "${_path}" NAME_WLE)
    list(APPEND PB_SRCS "${CMAKE_CURRENT_BINARY_DIR}/protobuf/${_stem}.pb.c")
    list(APPEND PB_OBJS "${CMAKE_CURRENT_BINARY_DIR}/protobuf/${_stem}.pb.c.o")
endforeach()

target_sources(${COMPONENT_LIB} PRIVATE ${PB_SRCS})

add_custom_command(
    COMMAND mkdir -p ${CMAKE_CURRENT_BINARY_DIR}/protobuf
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/protobuf
)
add_custom_command(
    COMMAND /usr/bin/python ${CMAKE_CURRENT_SOURCE_DIR}/nanopb/generator/nanopb_generator.py -I ${CMAKE_CURRENT_SOURCE_DIR}/protobuf -D ${CMAKE_CURRENT_BINARY_DIR}/protobuf ${PROTO_FILES}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/protobuf
    OUTPUT ${PB_SRCS}
)
