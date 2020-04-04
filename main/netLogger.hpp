#ifndef NETLOGGER_HPP
#define NETLOGGER_HPP

#include <esp_http_server.h>
#include <vector>
#include "utils.hpp"

class NetLogger
{
protected:
    typedef NetLogger Self;
    typedef void(*SinkFunc)(const char* data, int len, void* userp);

    FILE* mOrigStdout = nullptr;
    FILE* mNewStdout = nullptr;
    bool mDisableDefault;
    static NetLogger* gInstance;
    SinkFunc mSinkFunc = nullptr;
    void* mSinkFuncUserp = nullptr;
    static int espVprintf(const char * format, va_list args);
    static int logFdWriteFunc(void* cookie, const char* data, int size);
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
    esp_err_t httpSendChunk(int fd, const char* data, size_t len);
    static void logSink(const char* data, int len, void* userp);
public:
    NetLogger(bool disableDefault);
    void setSinkFunc(SinkFunc sinkFunc, void* userp);
    int printfOri(const char* fmt, ...);
    void registerWithHttpServer(httpd_handle_t server, const char* path);
    void unregisterWithHttpServer(const char* path);
};

#endif
