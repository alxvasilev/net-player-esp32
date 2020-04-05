PROJECT_NAME := netplayer

include $(ADF_PATH)/project.mk

SPIFFS_IMAGE_FLASH_IN_PROJECT := 1
$(eval $(call spiffs_create_partition_image,storage,spiffs))
