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
const char* fileGetExtension(const std::string& str) {
    for (int i = str.size()-1; i>=0; i--) {
        if (str[i] == '.') {
            return str.c_str() + i + 1;
        }
    }
    return nullptr;
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
bool respondWithDirContent(const std::string& dirname, httpd_req_t* req)
{
    DIR* dir = opendir(dirname.c_str());
    if (!dir) {
        std::string msg = "Error opening directory: ";
        msg.append(strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg.c_str());
        return false;
    }
    struct stat info;
    std::string buf;
    buf.reserve(300);
    buf = "{\"dir\":\"";
    buf.append(dirname).append("\",\"l\":[");
    httpd_resp_send_chunk(req, buf.c_str(), buf.size());
    int cnt = 0;
    std::string fname;
    std::string fullName;
    for(;;) {
        struct dirent* entry = readdir(dir);
        if (!entry) {
            break;
        }
        fname = entry->d_name;
        std::string buf;
        fname = jsonStringEscape(fname.c_str());
        buf = (cnt++) ? ",{\"n\":\"" : "{\"n\":\"";
        buf.append(fname).append("\",\"");
        fullName = dirname + '/' + fname;
        if (stat(fullName.c_str(), &info) != 0) {
            ESP_LOGE(TAG, "Can't stat '%s'", fullName.c_str());
            buf.append("e\":1}");
            httpd_resp_send_chunk(req, buf.c_str(), buf.size());
            continue;
        }
        if (info.st_mode & S_IFDIR) { // path is a dir
            buf.append("d\":1}");
        } else {
            buf.append("s\":").append(std::to_string(info.st_size)) += '}';
        }
        httpd_resp_send_chunk(req, buf.c_str(), buf.size());
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_send_chunk(req, nullptr, 0);
    return true;
}
static esp_err_t fsDirListHandler(httpd_req_t* req)
{
    auto fn = urlGetPathAfterSlashCnt(req->uri, 1);
    if (!fn) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file/dir path in URL");
        return ESP_FAIL;
    }
    std::string dirname(fn);
    unescapeUrlParam(&dirname[0], dirname.size());
    ESP_LOGI(TAG, "List dir '%s'", dirname.c_str());
    return respondWithDirContent(dirname, req) ? ESP_OK : ESP_FAIL;
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
    const char* ext = fileGetExtension(fname);
    const char* contentType = (ext && (strcasecmp(ext, "html") == 0))
        ? "text/html"
        : "application/octet-stream";
    httpd_resp_set_type(req, contentType);
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

static esp_err_t httpGetHandler(const char* urlPath, httpd_req_t* req)
{
    std::string fname(urlPath);
    auto pos = fname.find('?');
    if (pos != std::string::npos) {
        fname.resize(pos);
    }
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
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Is a diectory");
        return ESP_FAIL;
    }
    if (!respondWithFileContent(fname, req)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t fsWwwGetHandler(httpd_req_t* req) {
    if (strncmp(req->uri, "/www/", 5)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Url must start with /www/");
        return ESP_FAIL;
    }
    std::string fname = "/spiffs";
    fname.append(req->uri + 4);
    return httpGetHandler(fname.c_str(), req);
}

static esp_err_t fsFileGetHandler(httpd_req_t *req)
{
    auto fn = urlGetPathAfterSlashCnt(req->uri, 1);
    if (!fn) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file/dir path in URL");
        return ESP_FAIL;
    }
    return httpGetHandler(fn, req);
}

bool delDirectory(const char* dirname) {
    DIR* dir = opendir(dirname);
    if (!dir) {
        return false;
    }
    bool ok = true;
    ESP_LOGI(TAG, "Recursively deleting dir %s", dirname);
    for(;;) {
        struct dirent* entry = readdir(dir);
        if (!entry) {
            break;
        }
        std::string fname = dirname;
        fname += '/';
        fname.append(entry->d_name);
        struct stat info;
        if (stat(fname.c_str(), &info) != 0) {
            ESP_LOGW(TAG, "Cannot stat '%s' for deletion", fname.c_str());
            ok = false;
            continue;
        }
        if (info.st_mode & S_IFDIR) { // path is a dir
            ok &= delDirectory(fname.c_str());
        } else {
            ESP_LOGI(TAG, "Deleting file %s", fname.c_str());
            ok &= (remove(fname.c_str()) == 0);
        }
    }
    closedir(dir);
    if (ok) {
        ESP_LOGI(TAG, "Deleted emptied dir %s", dirname);
        ok &= (remove(dirname) == 0);
    }
    return ok;
}

static esp_err_t fsFileDelHandler(httpd_req_t *req)
{
    auto fn = urlGetPathAfterSlashCnt(req->uri, 1);
    if (!fn) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file/dir path in URL");
        return ESP_FAIL;
    }
    std::string fname(fn);
    unescapeUrlParam(&fname[0], fname.size());
    struct stat info;
    if (stat(fname.c_str(), &info) != 0) {
        std::string msg = "File/directory '";
        msg.append(fname).append("' not found");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, msg.c_str());
        return ESP_FAIL;
    }
    bool ok;
    if (info.st_mode & S_IFDIR) { // path is a dir
        ESP_LOGI(TAG, "Deleting directory '%s'", fname.c_str());
        ok = delDirectory(fname.c_str());
    } else {
        ok = remove(fname.c_str()) == 0;
    }
    if (ok) {
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error deleting file/dir");
        return ESP_FAIL;
    }
}

extern const httpd_uri_t httpFsPost = {
    .uri       = "/file/*",
    .method    = HTTP_POST,
    .handler   = fsFilePostHandler,
    .user_ctx  = NULL
};
extern const httpd_uri_t httpFsGet = {
    .uri       = "/file/*",
    .method    = HTTP_GET,
    .handler   = fsFileGetHandler,
    .user_ctx  = NULL
};
extern const httpd_uri_t httpWwwGet = {
    .uri       = "/www/*",
    .method    = HTTP_GET,
    .handler   = fsWwwGetHandler,
    .user_ctx  = NULL
};
extern const httpd_uri_t httpFsDirList = {
    .uri       = "/ls/*",
    .method    = HTTP_GET,
    .handler   = fsDirListHandler,
    .user_ctx  = NULL
};

extern const httpd_uri_t httpFsDel = {
    .uri       = "/delfile/*",
    .method    = HTTP_GET,
    .handler   = fsFileDelHandler,
    .user_ctx  = NULL
};

void httpFsRegisterHandlers(httpd_handle_t server)
{
    httpd_register_uri_handler(server, &httpFsPost);
    httpd_register_uri_handler(server, &httpFsGet);
    httpd_register_uri_handler(server, &httpFsDel);
    httpd_register_uri_handler(server, &httpFsDirList);
    httpd_register_uri_handler(server, &httpWwwGet);
}
