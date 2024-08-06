#pragma once

#include <BellLogger.h>
#include <esp_log.h>

#define CSPOT_LOG(type, ...)                                                \
  do {                                                                      \
    bell::bellGlobalLogger->type(__FILE__, __LINE__, "cspot", __VA_ARGS__); \
  } while (0)

#define MERCURY_LOGI(fmt,...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define MERCURY_LOGD(fmt,...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
