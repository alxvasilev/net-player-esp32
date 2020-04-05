#include <esp_log.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include "utils.hpp"

template <class T>
T min(T a, T b) { return (a < b) ? a : b; }
enum: int16_t { kOtaBufSize = 512 };
bool rollbackIsPendingVerify()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t otaState;
    if (esp_ota_get_state_partition(running, &otaState) != ESP_OK) {
        return false;
    }
    return (otaState == ESP_OTA_IMG_PENDING_VERIFY);
}

/* Receive .Bin file */
static esp_err_t OTA_update_post_handler(httpd_req_t *req)
{
    ESP_LOGI("OTA", "OTA request received (%p)", currentTaskHandle());
    char* otaBuf = new char[kOtaBufSize]; // no need to free it, we will reboot
    int contentLen = req->content_len;
    const auto update_partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        ESP_LOGW("OTA", "Invalid OTA state of running app, trying to set it");
        esp_ota_mark_app_valid_cancel_rollback();
        err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE("OTA", "Error %s after attempting to fix OTA state of running app, aborting OTA", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin error");
            return ESP_FAIL;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE("OTA", "esp_ota_begin returned error %s, aborting OTA", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin error");
        return ESP_FAIL;
    }

    ESP_LOGI("OTA", "Writing to partition '%s' subtype %d at offset 0x%x",
        update_partition->label, update_partition->subtype, update_partition->address);

    uint64_t tsStart = esp_timer_get_time();
    int displayCtr = 0;
    for (int remain = contentLen; remain > 0; )
    {
        /* Read the data for the request */
        int recvLen;
        for (int numWaits = 0; numWaits < 4; numWaits++) {
            recvLen = httpd_req_recv(req, otaBuf, ::min(remain, (int)kOtaBufSize));
            if (recvLen != HTTPD_SOCK_ERR_TIMEOUT) {
                break;
            }
        }
        if (recvLen < 0)
        {
            ESP_LOGE("OTA", "OTA recv error %d, aborting", recvLen);
            return ESP_FAIL;
        }
        remain -= recvLen;
        displayCtr += recvLen;
        if (displayCtr > 10240) {
            displayCtr = 0;
            printf("OTA: Recv %d of %d bytes\r", contentLen - remain, contentLen);
        }
        esp_ota_write(ota_handle, otaBuf, recvLen);
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE("OTA", "esp_ota_end error: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Lets update the partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE("OTA", "esp_ota_set_boot_partition error %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    const auto bootPartition = esp_ota_get_boot_partition();
    const char msg[] = "OTA update successful";
    httpd_resp_send(req, msg, sizeof(msg));
    ESP_LOGI("OTA", "OTA update successful (%.1f sec)",
        (((double)esp_timer_get_time() - tsStart) / 1000000));
    ESP_LOGI("OTA", "Will boot from partition '%s', subtype %d at offset 0x%x",
        bootPartition->label, bootPartition->subtype, bootPartition->address);

    // Reboot asynchronously, after we return the http response
    ESP_LOGI("OTA", " restarting system...");
    esp_timer_create_args_t args = {};
    args.dispatch_method = ESP_TIMER_TASK;
    args.callback = [](void*) {
        esp_restart();
    };
    args.name = "rebootTimer";
    esp_timer_handle_t oneshot_timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &oneshot_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(oneshot_timer, 1000000));
    return ESP_OK;
}

extern const httpd_uri_t otaUrlHandler = {
    .uri       = "/ota",
    .method    = HTTP_POST,
    .handler   = OTA_update_post_handler,
    .user_ctx  = NULL
};

static constexpr const char* RBK = "RBK";
void rollbackConfirmAppIsWorking()
{
    if (!rollbackIsPendingVerify()) {
        return;
    }
    ESP_LOGW(RBK, "App appears to be working properly, confirming boot partition...");
    esp_ota_mark_app_valid_cancel_rollback();
}

#define ENUM_NAME_CASE(name) case name: return #name

const char* otaPartitionStateToStr(esp_ota_img_states_t state)
{
    switch (state) {
        ENUM_NAME_CASE(ESP_OTA_IMG_NEW);
        ENUM_NAME_CASE(ESP_OTA_IMG_PENDING_VERIFY);
        ENUM_NAME_CASE(ESP_OTA_IMG_VALID);
        ENUM_NAME_CASE(ESP_OTA_IMG_INVALID);
        ENUM_NAME_CASE(ESP_OTA_IMG_ABORTED);
        ENUM_NAME_CASE(ESP_OTA_IMG_UNDEFINED);
        default: return "(UNKNOWN)";
    }
}

bool setOtherPartitionBootableAndRestart()
{
    if (rollbackIsPendingVerify()) {
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }

    ESP_LOGW(RBK, "Could not cancel current OTA, manually switching boot partition");
    auto otherPartition = esp_ota_get_next_update_partition(NULL);
    if (!otherPartition) {
        ESP_LOGE(RBK, "There is no second OTA partition");
        return false;
    }
    esp_ota_img_states_t state;
    auto err = esp_ota_get_state_partition(otherPartition, &state);
    if (err != ESP_OK) {
        ESP_LOGW(RBK, "Error getting state of other partition: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(RBK, "Other partition has state %s", otaPartitionStateToStr(state));
    }
    ESP_ERROR_CHECK(esp_ota_set_boot_partition(otherPartition));

    esp_restart();
    return true;
}

