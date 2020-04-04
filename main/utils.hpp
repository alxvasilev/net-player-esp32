#ifndef NETPLAYER_UTILS_HPP
#define NETPLAYER_UTILS_HPP

#include <esp_http_server.h>
#include <vector>
#include <stdarg.h>
#include <audio_element.h>
#include <http_stream.h>
#include <i2s_stream.h>
#include <esp_log.h>
#include <sys/socket.h>

template<typename T>
struct BufPtr
{
protected:
    T* mPtr;
    BufPtr() {} // mPtr remains uninitialized, only for derived classes
public:
    T* ptr() { return mPtr; }
    BufPtr(T* ptr): mPtr(ptr){}
    BufPtr(BufPtr<T>&& other) {
        mPtr = other.mPtr;
        other.mPtr = nullptr;
    }
    ~BufPtr() {
        if (mPtr) {
            ::free(mPtr);
        }
    }
    void free() {
        if (mPtr) {
            ::free(mPtr);
        }
    }
    void freeAndReset(T* newPtr) {
        free();
        mPtr = newPtr;
    }
    void* release() {
        auto ret = mPtr;
        mPtr = nullptr;
        return ret;
    }
};
void binToHex(const uint8_t* data, size_t len, char* str);
bool unescapeUrlParam(char* str, size_t len);

uint8_t hexDigitVal(char digit);
class UrlParams: public BufPtr<char>
{
public:
    struct Substring
    {
        const char* str;
        size_t len;
        Substring(const char* aStr, size_t aLen): str(aStr), len(aLen) {}
        Substring() {}
        operator bool() const { return str != nullptr; }
    };
    struct KeyVal
    {
        Substring key;
        Substring val;
    };
protected:
    size_t mSize;
    std::vector<KeyVal> mKeyVals;
    bool parse()
    {
        auto end = mPtr + mSize - 1;
        char* pch = mPtr;
        for (;;) {
            auto start = pch;
            for (; (pch < end) && (*pch != '='); pch++);
            if (pch >= end) { // unexpected end
                return false;
            }
            *pch = 0; // null-terminate the key
            mKeyVals.emplace_back();
            KeyVal& keyval = mKeyVals.back();
            auto& key = keyval.key;
            key.str = start;
            key.len = pch - start;

            start = ++pch;
            for (; (pch < end) && (*pch != '&'); pch++);
            auto& val = keyval.val;
            auto len = pch - start;
            if (!unescapeUrlParam(start, len)) {
                return false;
            }
            val.str = start;
            val.len = len;
            if (pch >= end) {
                assert(pch == end);
                assert(*pch == 0);
                return true;
            }
            *(pch++) = 0; // null-terminate the value
        }
    }
public:
    const std::vector<KeyVal>& keyVals() const { return mKeyVals; }
    UrlParams(httpd_req_t* req)
    {
        mSize = httpd_req_get_url_query_len(req) + 1;
        if (mSize <= 1) {
            mSize = 0;
            mPtr = nullptr;
            return;
        }
        mPtr = (char*)malloc(mSize);
        if (httpd_req_get_url_query_str(req, mPtr, mSize) != ESP_OK) {
            free();
            return;
        }
        parse();
    }
    Substring strParam(const char* name)
    {
        for (auto& keyval: mKeyVals) {
            if (strcmp(name, keyval.key.str) == 0) {
                return keyval.val;
            }
        }
        return Substring(nullptr, 0);
    }
    long intParam(const char* name, long defVal)
    {
        auto strVal = strParam(name);
        auto str = strVal.str;
        if (!str) {
            return defVal;
        }
        char* endptr;
        auto val = strtol(str, &endptr, 10);
        return (endptr == str + strVal.len) ? val : defVal;
    }
};

static inline TaskHandle_t currentTaskHandle()
{
    extern volatile void * volatile pxCurrentTCB;
    return (TaskHandle_t)pxCurrentTCB;
}

class Mutex
{
    SemaphoreHandle_t mMutex;
    StaticSemaphore_t mMutexMem;
public:
    Mutex() {
        mMutex = xSemaphoreCreateMutexStatic(&mMutexMem);
    }
    void lock() { xSemaphoreTake(mMutex, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(mMutex); }
};

class MutexLocker
{
    Mutex& mMutex;
public:
    MutexLocker(Mutex& aMutex): mMutex(aMutex) { mMutex.lock(); }
    ~MutexLocker() { mMutex.unlock(); }
};

extern "C" int httpd_default_send(httpd_handle_t hd, int sockfd, const char *buf, size_t buf_len, int flags);

class StdoutRedirector
{
protected:
    typedef StdoutRedirector Self;
    typedef void(*SinkFunc)(const char* data, int len, void* userp);

    FILE* mOrigStdout = nullptr;
    FILE* mNewStdout = nullptr;
    bool mDisableDefault;
    static StdoutRedirector* gInstance;
    SinkFunc mSinkFunc = nullptr;
    void* mSinkFuncUserp = nullptr;
    static int espVprintf(const char * format, va_list args)
    {
        return vfprintf(gInstance->mNewStdout, format, args);
    }
    static int logFdWriteFunc(void* cookie, const char* data, int size)
    {
        if (!gInstance->mDisableDefault) {
            fwrite(data, 1, size, gInstance->mOrigStdout);
        }
        if (gInstance->mSinkFunc) {
            gInstance->mSinkFunc(data, size, gInstance->mSinkFuncUserp);
        }
        return size;
    }
    // log server stuff
    struct LogConn
    {
        enum: uint8_t { kFlagHtml = 1 };
        int sockfd;
        // need this only to access self from session ctx free func, which takes
        // just a single pointer
        Self* self = nullptr;
        uint8_t flags = 0;
        LogConn(int aSockFd, Self* aSelf): sockfd(aSockFd), self(aSelf) {}
    };

    std::vector<LogConn*> mConnections;
    Mutex mListMutex;
    httpd_handle_t mHttpServer = nullptr;
    static void connCloseFunc(void* ctx)
    {
        auto conn = static_cast<LogConn*>(ctx);
        assert(conn);
        auto& conns = conn->self->mConnections;
        for (auto it = conns.begin(); it != conns.end(); it++) {
            if (*it == conn) {
                conns.erase(it);
                httpd_sess_set_ctx(conn->self->mHttpServer, conn->sockfd, nullptr, nullptr);
                delete conn;
                return;
            }
        }
        assert(false);
    }
    // called by the http connection task
    static esp_err_t logRequestHandler(httpd_req_t* req)
    {
        if (req->sess_ctx) {
            return ESP_OK;
        }
        Self* self = static_cast<Self*>(req->user_ctx);
        bool isBrowser = false;
        char buf[64];
        auto err = httpd_req_get_hdr_value_str(req, "User-Agent", buf, sizeof(buf));
        if (err == ESP_OK || err == ESP_ERR_HTTPD_RESULT_TRUNC) {
            if (strstr(buf, "Mozilla")) {
                isBrowser = true;
            }
        }

        // force sending headers by sending a dummy chunk
        if (isBrowser) {
            err = httpd_resp_send_chunk(req, "<html><body><pre>", 17);
        } else {
            err = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        if (err != ESP_OK) {
            return ESP_FAIL;
        }
        int sockfd = httpd_req_to_sockfd(req);
        auto conn = new LogConn(sockfd, self);
        if (isBrowser) {
            conn->flags |= LogConn::kFlagHtml;
        }
        MutexLocker lock(self->mListMutex);
        // detect when socket is closed
        httpd_sess_set_ctx(req->handle, sockfd, conn, &Self::connCloseFunc);
        self->mConnections.push_back(conn);
        return ESP_OK;
    }
    esp_err_t sendChunkToFd(int fd, const char* data, size_t len)
    {
        char strBuf[10];
        auto numChars = snprintf(strBuf, sizeof(strBuf), "%x\r\n", len);
        return ((httpd_default_send(mHttpServer, fd, strBuf, numChars, 0) >= 0) &&
            (httpd_default_send(mHttpServer, fd, data, len, 0) >= 0) &&
            (httpd_default_send(mHttpServer, fd, "\r\n", 2, 0) >=0));
    }
    static void logSink(const char* data, int len, void* userp)
    {
        auto& self = *static_cast<Self*>(userp);
        MutexLocker lock(self.mListMutex);
        for (auto it = self.mConnections.begin(); it != self.mConnections.end();) {
            int sockfd = (*it)->sockfd;
            if (!self.sendChunkToFd(sockfd, data, len)) {
                it++;
                httpd_sess_trigger_close(self.mHttpServer, sockfd);
            } else {
                it++;
            }
        }
    }
public:
    StdoutRedirector(bool disableDefault)
    {
        if (gInstance) {
            abort();
        }
        gInstance = this;
        mDisableDefault = disableDefault;
        mOrigStdout = stdout;
        mNewStdout = stdout = funopen(nullptr, nullptr, &logFdWriteFunc, nullptr, nullptr);
        esp_log_set_vprintf(&espVprintf);
    }
    void setSinkFunc(SinkFunc sinkFunc, void* userp)
    {
        mSinkFunc = sinkFunc;
        mSinkFuncUserp = userp;
    }
    int printfOri(const char* fmt, ...)
    {
        va_list valist;
        va_start(valist, fmt);
        int n = vfprintf(mOrigStdout, fmt, valist);
        va_end(valist);
        return n;
    }
    void registerWithHttpServer(httpd_handle_t server, const char* path)
    {
        httpd_uri_t cfg = {
            .uri = path,
            .method = HTTP_GET,
            .handler = logRequestHandler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &cfg);
        setSinkFunc(logSink, this);
    }
    void unregisterWithHttpServer(const char* path)
    {
        setSinkFunc(nullptr, nullptr);
        httpd_unregister_uri(mHttpServer, path);
    }
};


extern "C" const i2s_stream_cfg_t myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT;
extern "C" const http_stream_cfg_t myHTTP_STREAM_CFG_DEFAULT;

#endif
