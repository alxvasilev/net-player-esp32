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

int NetLogger::espVprintf(const char * format, va_list args)
{
    return vfprintf(gInstance->mNewStdout, format, args);
}
int NetLogger::logFdWriteFunc(void* cookie, const char* data, int size)
{
    if (!gInstance->mDisableDefault) {
        fwrite(data, 1, size, gInstance->mOrigStdout);
    }
    if (gInstance->mSinkFunc) {
        gInstance->mSinkFunc(data, size, gInstance->mSinkFuncUserp);
    }
    return size;
}

void NetLogger::connCloseFunc(void* ctx)
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
    MutexLocker lock(self->mListMutex);
    // detect when socket is closed
    httpd_sess_set_ctx(req->handle, sockfd, conn, &Self::connCloseFunc);
    self->mConnections.push_back(conn);
    return ESP_OK;
}
esp_err_t NetLogger::httpSendChunk(int fd, const char* data, size_t len)
{
    char strBuf[10];
    auto numChars = snprintf(strBuf, sizeof(strBuf), "%x\r\n", len);
    return ((httpd_default_send(mHttpServer, fd, strBuf, numChars, 0) >= 0) &&
            (httpd_default_send(mHttpServer, fd, data, len, 0) >= 0) &&
            (httpd_default_send(mHttpServer, fd, "\r\n", 2, 0) >=0));
}
void NetLogger::logSink(const char* data, int len, void* userp)
{
    auto& self = *static_cast<Self*>(userp);
    MutexLocker lock(self.mListMutex);
    for (auto it = self.mConnections.begin(); it != self.mConnections.end();) {
        int sockfd = (*it)->sockfd;
        if (!self.httpSendChunk(sockfd, data, len)) {
            it++;
            httpd_sess_trigger_close(self.mHttpServer, sockfd);
        } else {
            it++;
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
    mOrigStdout = stdout;
    mNewStdout = stdout = funopen(nullptr, nullptr, &logFdWriteFunc, nullptr, nullptr);
    esp_log_set_vprintf(&espVprintf);
}
void NetLogger::setSinkFunc(SinkFunc sinkFunc, void* userp)
{
    mSinkFunc = sinkFunc;
    mSinkFuncUserp = userp;
}
int NetLogger::printfOri(const char* fmt, ...)
{
    va_list valist;
    va_start(valist, fmt);
    int n = vfprintf(mOrigStdout, fmt, valist);
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
