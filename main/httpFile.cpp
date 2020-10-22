#include <esp_http_server.h>
#include "utils.hpp"
#include <dirent.h>
#include <sys/stat.h>

static constexpr int kFileIoBufSize = 1024;
static constexpr const char* TAG = "HTTPFS";

const char* urlGetPathAfterSlashCnt(const char* url, int slashCnt)
{
    for (; *url; url++) {
        if (*url == '/') {
            if (slashCnt-- <= 0) {
                return url;
            }
        }
    }
    return nullptr;
}
int pathDirLen(const char* path) {
    const char* lastSlash = nullptr;
    for (const char* p = path; *p; p++) {
        if (*p == '/') {
            lastSlash = p;
        }
    }
    return lastSlash ? (lastSlash - path) : 0;
}

static esp_err_t fsFilePostHandler(httpd_req_t* req)
{
    auto fn = urlGetPathAfterSlashCnt(req->uri, 1);
    if (!fn) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file name in URL");
        return ESP_FAIL;
    }
    std::string fname = fn;
    unescapeUrlParam(&fname[0], fname.size());

    std::unique_ptr<char[]> rxBuf(new char[kFileIoBufSize]);
    int contentLen = req->content_len;
    ESP_LOGI(TAG, "Receiving file '%s' of size %d...", fname.c_str(), contentLen);

    FileHandle file(fopen(fname.c_str(), "w"));
    if (!file) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file for writing");
        return ESP_FAIL;
    }

    int displayCtr = 0;
    for (int remain = contentLen; remain > 0;) {
        /* Read the data for the request */
        int recvLen;
        for (int numWaits = 0; numWaits < 4; numWaits++) {
            recvLen = httpd_req_recv(req, rxBuf.get(), std::min(remain, kFileIoBufSize));
            if (recvLen != HTTPD_SOCK_ERR_TIMEOUT) {
                break;
            }
        }

        if (recvLen < 0) {
            ESP_LOGI(TAG, "File recv error %d", recvLen);
            return ESP_FAIL;
        }
        remain -= recvLen;
        displayCtr += recvLen;
        if (displayCtr > 10240) {
            displayCtr = 0;
            printf("FS: Recv %d of %d bytes\r", contentLen - remain, contentLen);
        }
        if (fwrite(rxBuf.get(), 1, recvLen, file.get()) != recvLen) {
            ESP_LOGE(TAG, "Error writing to file");
            return ESP_FAIL;
        }
    }
    httpd_resp_send(req, "OK\r\n", 4);
    ESP_LOGI(TAG, "Success receiving file '%s'", fname.c_str());
    return ESP_OK;
}
bool respondWithDirContent(const std::string& fname, httpd_req_t* req)
{
    DIR* dir = opendir(fname.c_str());
    if (!dir) {
        std::string msg = "Error opening directory: ";
        msg.append(strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg.c_str());
        return false;
    }

    std::string buf;
    buf.reserve(300);
    int parentDirLen = pathDirLen(fname.c_str());
    buf = "<html><body><h1>";
    buf.append(fname).append("</h1><br/>");
    if (parentDirLen) {
        buf.append("<button onclick=\"window.location='/file")
           .append(fname, 0, parentDirLen).append("'\">..</button><br/>");
    }
    httpd_resp_send_chunk(req, buf.c_str(), buf.size());

    for(;;) {
        struct dirent* entry = readdir(dir);
        if (!entry) {
            break;
        }
        buf = "<a href='/file";
        buf.append(fname) += '/';
        buf.append(entry->d_name).append("'>")
           .append(entry->d_name).append("</a><br/>");
        httpd_resp_send_chunk(req, buf.c_str(), buf.size());
    }
    closedir(dir);
    buf = "</body></html>";
    httpd_resp_send_chunk(req, buf.c_str(), buf.size());
    httpd_resp_send_chunk(req, nullptr, 0);
    return true;
}
bool respondWithFileContent(const std::string& fname, httpd_req_t* req)
{
    std::unique_ptr<char[]> rxBuf(new char[kFileIoBufSize]);
    FileHandle file(fopen(fname.c_str(), "r"));
    if (!file) {
        std::string msg = "Error opening file: ";
        msg += strerror(errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Sending file '%s'...", fname.c_str());
    httpd_resp_set_type(req, "application/octet-stream");
    for (;;) {
        /* Read the data for the request */
        int readLen = fread(rxBuf.get(), 1, kFileIoBufSize, file.get());
        if (readLen > 0) {
            if (httpd_resp_send_chunk(req, rxBuf.get(), readLen) != ESP_OK) {
                ESP_LOGE(TAG, "Error sending file data, aborting");
                return false;
            }
            if (readLen != kFileIoBufSize) {
                break;
            }
        } else if (readLen == 0) {
            break;
        } else {
            ESP_LOGE(TAG, "Error reading file '%s': %s", fname.c_str(), strerror(errno));
            return false;
        }
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    ESP_LOGI(TAG, "File sent successfully");
    return true;
}

static esp_err_t fsFileGetHandler(httpd_req_t *req)
{
    auto fn = urlGetPathAfterSlashCnt(req->uri, 1);
    if (!fn) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file/dir path in URL");
        return ESP_FAIL;
    }
    std::string fname(fn);
    unescapeUrlParam(&fname[0], fname.size());
    ESP_LOGI(TAG, "Get file '%s'", fname.c_str());
    struct stat info;
    if (stat(fname.c_str(), &info) != 0) {
        std::string msg = "File/directory '";
        msg.append(fname).append("' not found");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, msg.c_str());
        return ESP_FAIL;
    }
    if (info.st_mode & S_IFDIR) { // path is a dir
        if (!respondWithDirContent(fname, req)) {
            return ESP_FAIL;
        }
    } else {
        if (!respondWithFileContent(fname, req)) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

extern const httpd_uri_t httpFsGet = {
    .uri       = "/file/*",
    .method    = HTTP_GET,
    .handler   = fsFileGetHandler,
    .user_ctx  = NULL
};
extern const httpd_uri_t httpFsPut = {
    .uri       = "/file/*",
    .method    = HTTP_POST,
    .handler   = fsFilePostHandler,
    .user_ctx  = NULL
};
