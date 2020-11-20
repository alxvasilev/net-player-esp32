#ifndef NETLOGGER_HPP
#define NETLOGGER_HPP

#include <esp_http_server.h>
#include <vector>
#include "utils.hpp"

class NetLogger
{
protected:
    enum: uint16_t { kInitialBufSize = 128 };
    typedef NetLogger Self;
    typedef void(*SinkFunc)(const char* data, int len, void* userp);

    Mutex mMutex;
    bool mDisableDefault;
    std::vector<char> mBuf;
    static NetLogger* gInstance;
    SinkFunc mSinkFunc = nullptr;
    void* mSinkFuncUserp = nullptr;
    // log server stuff
    struct LogConn
    {
        enum: uint8_t { kFlagHtml = 1 };
        int sockfd;
        // need this only to access self from session ctx free func, which takes
        // just a single pointer
        Self* self = nullptr;
        LogConn(int aSockFd, Self* aSelf): sockfd(aSockFd), self(aSelf) {}
    };

    std::vector<LogConn*> mConnections;
    Mutex mListMutex;
    httpd_handle_t mHttpServer = nullptr;
    static void connCloseFunc(void* ctx);
    // called by the http connection task
    static esp_err_t logRequestHandler(httpd_req_t* req);
    esp_err_t httpSendChunk(int fd, const char* data, uint16_t len);
    static void logSink(const char* data, int len, void* userp);
public:
    NetLogger(bool disableDefault);
    bool hasRemoteSink() const { return !mConnections.empty(); }
    void setSinkFunc(SinkFunc sinkFunc, void* userp);
    static int vprintf(const char * format, va_list args);
    static int printf(const char* fmt, ...);
    void registerWithHttpServer(httpd_handle_t server, const char* path);
    void unregisterWithHttpServer(const char* path);
    bool hasConnections() { return !mConnections.empty(); }
    bool waitForLogConnection(int sec=-1);
};

#endif
