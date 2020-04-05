#include <esp_http_server.h>
#include "utils.hpp"

static constexpr char fsPrefix[] = "/spiffs/";
static constexpr int kFileIoBufSize = 512;
static constexpr const char* TAG = "HTTPFS";

static esp_err_t fsFilePostHandler(httpd_req_t *req)
{
    auto fname = getUrlFile(req->uri);
    if (!fname) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file name in URL");
        return ESP_FAIL;
    }
    std::unique_ptr<char[]> rxBuf(new char[kFileIoBufSize]);
    strcpy(rxBuf.get(), fsPrefix);
    strcpy(rxBuf.get() + sizeof(fsPrefix) - 1, fname);
    int contentLen = req->content_len;
    ESP_LOGI(TAG, "Uploading file '%s' of size %d...", rxBuf.get(), contentLen);

    std::unique_ptr<FILE> file(fopen(rxBuf.get(), "w"));
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file for writing");
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

        if (recvLen < 0)
        {
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
    ESP_LOGI(TAG, "Success uploading file '%s'", fname);
    return ESP_OK;
}

static esp_err_t fsFileGetHandler(httpd_req_t *req)
{
    auto fname = getUrlFile(req->uri);
    if (!fname) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file name in URL");
        return ESP_FAIL;
    }

    std::unique_ptr<char[]> rxBuf(new char[kFileIoBufSize]);
    strcpy(rxBuf.get(), fsPrefix);
    strcpy(rxBuf.get() + sizeof(fsPrefix) - 1, fname);
    std::unique_ptr<FILE> file(fopen(rxBuf.get(), "r"));
    if (!file) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloading file '%s'...", rxBuf.get());

    for (;;) {
        /* Read the data for the request */
        int readLen = fread(rxBuf.get(), 1, kFileIoBufSize, file.get());
        if (readLen > 0) {
            httpd_resp_send_chunk(req, rxBuf.get(), readLen);
            if (readLen != kFileIoBufSize) {
                break;
            }
        } else if (readLen == 0) {
            break;
        } else {
            ESP_LOGE(TAG, "Error reading file '%s'", fname);
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    ESP_LOGI(TAG, "File download successful");
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
