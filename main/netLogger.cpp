#include <esp_http_server.h>
#include <stdarg.h>
#include <audio_element.h>
#include <http_stream.h>
#include <i2s_stream.h>
#include <esp_log.h>
#include <sys/socket.h>
#include "netLogger.hpp"
#include "utils.hpp"

extern "C" int httpd_default_send(httpd_handle_t hd, int sockfd, const char *buf, size_t buf_len, int flags);
NetLogger* NetLogger::gInstance = nullptr;

int NetLogger::vprintf(const char * format, va_list args)
{
    MutexLocker lock(gInstance->mMutex);
    auto& buf = gInstance->mBuf;
    int num;
    for (;;) {
        num = ::vsnprintf(buf.data(), buf.size(), format, args);
        if (num < 0) {
            return num;
        } else if (num < buf.size()) {
            break;
        } else {
            buf.resize(num + 32);
        }
    }
    if (!gInstance->mDisableDefault) {
        fwrite(buf.data(), 1, num, stdout);
        fflush(stdout);
    }
    if (gInstance->mSinkFunc) {
        gInstance->mSinkFunc(buf.data(), num, gInstance->mSinkFuncUserp);
    }
    return num;
}

void NetLogger::connCloseFunc(void* ctx)
{
    auto conn = static_cast<LogConn*>(ctx);
    assert(conn);
    auto self = conn->self;
    MutexLocker locker(self->mListMutex);
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
esp_err_t NetLogger::logRequestHandler(httpd_req_t* req)
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
    // detect when socket is closed
    httpd_sess_set_ctx(req->handle, sockfd, conn, &Self::connCloseFunc);

    MutexLocker lock(self->mListMutex);
    self->mConnections.push_back(conn);
    return ESP_OK;
}

esp_err_t NetLogger::httpSendChunk(int fd, const char* data, uint16_t len)
{
    char strBuf[10];
    auto end = numToHex(len, strBuf);
    *(end++) = '\r';
    *(end++) = '\n';
    return ((httpd_default_send(mHttpServer, fd, strBuf, end-strBuf, 0) >= 0) &&
            (httpd_default_send(mHttpServer, fd, data, len, 0) >= 0) &&
            (httpd_default_send(mHttpServer, fd, "\r\n", 2, 0) >=0));
}

void NetLogger::logSink(const char* data, int len, void* userp)
{
    auto& self = *static_cast<Self*>(userp);
    MutexLocker lock(self.mListMutex);
    for (auto it = self.mConnections.begin(); it != self.mConnections.end(); it++) {
        int sockfd = (*it)->sockfd;
        if (!self.httpSendChunk(sockfd, data, len)) {
            it--;
            httpd_sess_trigger_close(self.mHttpServer, sockfd);
        }
    }
}

NetLogger::NetLogger(bool disableDefault)
{
    if (gInstance) {
        abort();
    }
    gInstance = this;
    mDisableDefault = disableDefault;
    mBuf.resize(kInitialBufSize);
    esp_log_set_vprintf(&NetLogger::vprintf);
}

void NetLogger::setSinkFunc(SinkFunc sinkFunc, void* userp)
{
    MutexLocker lock(mMutex);
    mSinkFunc = sinkFunc;
    mSinkFuncUserp = userp;
}

int NetLogger::printf(const char* fmt, ...)
{
    va_list valist;
    va_start(valist, fmt);
    int n = NetLogger::vprintf(fmt, valist);
    va_end(valist);
    return n;
}

void NetLogger::registerWithHttpServer(httpd_handle_t server, const char* path)
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
void NetLogger::unregisterWithHttpServer(const char* path)
{
    setSinkFunc(nullptr, nullptr);
    httpd_unregister_uri(mHttpServer, path);
}
