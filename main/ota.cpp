#include <esp_log.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include "utils.hpp"

template <class T>
T min(T a, T b) { return (a < b) ? a : b; }

/* Receive .Bin file */
esp_err_t OTA_update_post_handler(httpd_req_t *req)
{
    ESP_LOGI("OTA", "OTA request received (%p)", currentTaskHandle());
    char otaBuf[1024];
    int contentLen = req->content_len;
    const auto update_partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        printf("Error %s with OTA Begin, Cancelling OTA\r\n", esp_err_to_name(err));
        return ESP_FAIL;
    }
    else {
        printf("Writing to partition '%s' subtype %d at offset 0x%x\r\n",
            update_partition->label, update_partition->subtype, update_partition->address);
    }

    uint64_t tsStart = esp_timer_get_time();
    int displayCtr = 0;
    for (int remain = contentLen; remain > 0; )
    {
        /* Read the data for the request */
        int recvLen;
        for (int numWaits = 0; numWaits < 4; numWaits++) {
            recvLen = httpd_req_recv(req, otaBuf, ::min((size_t)remain, sizeof(otaBuf)));
            if (recvLen != HTTPD_SOCK_ERR_TIMEOUT) {
                break;
            }
        }
        if (recvLen < 0)
        {
            ESP_LOGI("OTA", "OTA recv error %d", recvLen);
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
