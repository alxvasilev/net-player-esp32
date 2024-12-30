#pragma once

#include <BellLogger.h>
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <esp_log.h>

inline static const char* strOrNull(const char* str) {
    return str ? str : "(null)";
}
#define CSPOT_LOG(type, ...)                                                \
  do {                                                                      \
    bell::bellGlobalLogger->type(__FILE__, __LINE__, "cspot", __VA_ARGS__); \
  } while (0)
#define TRKLDR_LOGI(fmt,...) ESP_LOGI("cspot-load", fmt, ##__VA_ARGS__)
#define TRKLDR_LOGD(fmt,...) ESP_LOGD("cspot-load", fmt, ##__VA_ARGS__)
#define TRKLDR_LOGW(fmt,...) ESP_LOGW("cspot-load", fmt, ##__VA_ARGS__)
#define TRKLDR_LOGE(fmt,...) ESP_LOGE("cspot-load", fmt, ##__VA_ARGS__)

#define MERCURY_LOGI(fmt,...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define MERCURY_LOGE(fmt,...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define MERCURY_LOGD(fmt,...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#define MERCURY_LOGP(fmt,...) // protocol logging

#define SPIRC_LOGI(fmt,...) ESP_LOGI("spirc", fmt, ##__VA_ARGS__)
#define SPIRC_LOGP(fmt,...) ESP_LOGI("spirc", "\e[34m" fmt, ##__VA_ARGS__)
#define SPIRC_LOGE(fmt,...) ESP_LOGE("spirc", fmt, ##__VA_ARGS__)
#define SPIRC_LOGW(fmt,...) ESP_LOGW("spirc", fmt, ##__VA_ARGS__)
#define SPIRC_LOGD(fmt,...) ESP_LOGD("spirc", fmt, ##__VA_ARGS__)
