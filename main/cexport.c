#include "freertos/FreeRTOS.h"
#include <audio_element.h>
#include <i2s_stream.h>
#include <http_stream.h>

const i2s_stream_cfg_t myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT =
    I2S_STREAM_INTERNAL_DAC_CFG_DEFAULT();

const http_stream_cfg_t myHTTP_STREAM_CFG_DEFAULT = HTTP_STREAM_CFG_DEFAULT();
