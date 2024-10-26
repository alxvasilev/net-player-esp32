idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES st7735 mySystem httpLib libmad libFLAC libhelix-aac tremor
             tinyxml myeq equalizer spiffs cspot app_update
)

#COMPONENT_EXTRA_INCLUDES := $(BUILD_DIR_BASE)/cspot
