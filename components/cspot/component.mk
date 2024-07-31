PB_NAMES=$(basename $(notdir $(wildcard $(COMPONENT_PATH)/protobuf/*.proto)))
PB_SRCS=$(PB_NAMES:=.pb.c)

$(BUILD_DIR_BASE)/cspot/protobuf/%.pb.c: $(COMPONENT_PATH)/protobuf/%.proto | protobuf
	/usr/bin/python $(COMPONENT_PATH)/nanopb/generator/nanopb_generator.py -I $(COMPONENT_PATH)/protobuf -D $(BUILD_DIR_BASE)/cspot/protobuf $<

protobuf:
	mkdir -p $(BUILD_DIR_BASE)/cspot/protobuf

PB_SRCS_FULLP=$(addprefix $(BUILD_DIR_BASE)/cspot/protobuf/, $(PB_SRCS))
$(info "pb srcs: $(BUILD_DIR_BASE)")
src/AccessKeyFetcher.o: $(PB_SRCS_FULLP)

COMPONENT_SRCDIRS := ../../build/cspot/protobuf src bell-utils nanopb
COMPONENT_ADD_INCLUDEDIRS += src bell-utils/include nanopb
COMPONENT_EXTRA_INCLUDES := $(BUILD_DIR_BASE)/cspot
COMPONENT_EXTRA_CLEAN := protobuf/*.o
CXXFLAGS += -O3 -std=gnu++17
CFLAGS += -O3
CPPFLAGS=-DESP_PLATFORM=1 -D BELL_ONLY_CJSON=1 -DPB_ENABLE_MALLOC=1
