#ifndef NETPLAYER_UTILS_HPP
#define NETPLAYER_UTILS_HPP

#include <esp_http_server.h>
#include <vector>
#include <stdarg.h>
#include <esp_log.h>
#include <freertos/semphr.h>
#include <memory>
#include <string>
#include "buffer.hpp"
#include "mutex.hpp"
#include "timer.hpp"
#include "utils-parse.hpp"

#define myassert(cond) if (!(cond)) { \
    ESP_LOGE("ASSERT", "Assertion failed: %s at %s:%d", #cond, __FILE__, __LINE__); \
    *((int*)nullptr) = 0; }

#define TRACE ESP_LOGI("TRC", "%s:%d", __FILE__, __LINE__);

#define MY_STRINGIFY_HELPER(x) #x
#define MY_STRINGIFY(x) MY_STRINGIFY_HELPER(x)

struct utils {
protected:
    static bool sHaveSpiRam;
public:
    static bool haveSpiRam() { return sHaveSpiRam; }
    static bool detectSpiRam();
    static void* mallocTrySpiram(size_t internalSize, size_t spiramSize)
    {
        return sHaveSpiRam
            ? heap_caps_malloc(spiramSize, MALLOC_CAP_SPIRAM) : malloc(internalSize);
    }
    static void* mallocTrySpiram(size_t size)
    {
        return sHaveSpiRam
            ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : malloc(size);
    }
    constexpr uint32_t static log2(uint32_t n) noexcept
    {
        return (n > 1) ? 1 + log2(n >> 1) : 0;
    }
    static int16_t currentCpuFreq();
};

class UrlParams: public KeyValParser
{
public:
    UrlParams(httpd_req_t* req);
};

class FileHandle: public std::unique_ptr<FILE, void(*)(FILE*)>
{
protected:
    typedef std::unique_ptr<FILE, void(*)(FILE*)> Base;
public:
    FileHandle(FILE* f): Base(f, [](FILE* fp) { if (fp) fclose(fp); }) {}
};

static inline TaskHandle_t currentTaskHandle()
{
    extern volatile void * volatile pxCurrentTCB;
    return (TaskHandle_t)pxCurrentTCB;
}

static inline void usDelay(uint32_t us)
{
    auto end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end);
}

static inline void msDelay(uint32_t ms)
{
    usDelay(ms * 1000);
}

struct AsyncMsgBase
{
    esp_timer_handle_t mTimer = nullptr;
    void post()
    {
        ESP_ERROR_CHECK(esp_timer_start_once(mTimer, 0));
    }
    ~AsyncMsgBase() {
        if (mTimer) {
            esp_timer_stop(mTimer);
            esp_timer_delete(mTimer);
        }
    }
};

template <class Cb>
static void asyncCall(Cb&& func)
{
    struct AsyncMsg: public AsyncMsgBase {
        Cb mCallback;
        AsyncMsg(Cb&& cb): mCallback(std::forward<Cb>(cb))
        {
            esp_timer_create_args_t args = {};
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "asyncCall";
            args.arg = this;
            args.callback = &onTimer;
            ESP_ERROR_CHECK(esp_timer_create(&args, &mTimer));
        }
        static void onTimer(void* ctx) {
            auto self = static_cast<AsyncMsg*>(ctx);
            try {
                self->mCallback();
            } catch(std::exception& e) {
            }
            delete self;
        }
    };
    auto msg = new AsyncMsg(std::forward<Cb>(func));
    msg->post();
}

class ElapsedTimer
{
protected:
    int64_t mTsStart;
public:
    ElapsedTimer(): mTsStart(esp_timer_get_time()) {}
    int msStartTime() const { return (mTsStart + 500 ) / 1000; }
    int64_t usStartTime() const { return mTsStart; }
    int64_t usElapsed() const { return esp_timer_get_time() - mTsStart; }
    int msElapsed() const { return (usElapsed() + 500) / 1000; }
    void reset() { mTsStart = esp_timer_get_time(); }
};

namespace std {
template<>
    struct default_delete<FILE>
    {
        void operator()(FILE* file) const { fclose(file); }
    };
}

#endif
