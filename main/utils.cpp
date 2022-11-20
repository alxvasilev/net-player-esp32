#include "utils.hpp"
#include <soc/rtc.h>
#include <esp_log.h>

bool utils::sHaveSpiRam = false;
bool utils::detectSpiRam()
{
    // Detect SPI RAM presence
    auto buf = heap_caps_malloc(4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf)
    {
        free(buf);
        sHaveSpiRam = true;
        ESP_LOGI("", "SPI RAM available");
    }
    else
    {
        sHaveSpiRam = false;
        ESP_LOGI("", "SPI RAM NOT available");
    }
    return sHaveSpiRam;
}

UrlParams::UrlParams(httpd_req_t* req)
{
    mSize = httpd_req_get_url_query_len(req) + 1;
    if (mSize <= 1) {
        mSize = 0;
        mBuf = nullptr;
        return;
    }
    mOwn = true;
    mBuf = (char*)malloc(mSize);
    if (httpd_req_get_url_query_str(req, mBuf, mSize) != ESP_OK) {
        free(mBuf);
        mBuf = nullptr;
        return;
    }
    parse('&', '=', kUrlUnescape);
}

int16_t utils::currentCpuFreq() {
    rtc_cpu_freq_config_t conf;
    rtc_clk_cpu_freq_get_config(&conf);
    return conf.freq_mhz;
}


