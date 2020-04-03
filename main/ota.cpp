#include <esp_log.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
void rebootTask(void*) {
    vTaskDelay(1000/portTICK_RATE_MS);
    esp_restart();
    for(;;);
}
/* Receive .Bin file */
esp_err_t OTA_update_post_handler(httpd_req_t *req)
{
    char otaBuf[1024];
    int contentLen = req->content_len;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        printf("Error %s with OTA Begin, Cancelling OTA\r\n", esp_err_to_name(err));
        return ESP_FAIL;
    }
    else {
        printf("Writing to partition subtype %d at offset 0x%x\r\n",
            update_partition->subtype, update_partition->address);
    }

    for (int remain = contentLen; remain > 0; )
    {
        /* Read the data for the request */
        int recvLen;
        for (int numWaits = 0; numWaits < 4; numWaits++) {
            recvLen = httpd_req_recv(req, otaBuf, MIN(remain, sizeof(otaBuf)));
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
        printf("OTA RX: %d of %d\r", contentLen - remain, contentLen);
        // Write OTA data
        esp_ota_write(ota_handle, otaBuf, recvLen);
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE("OTA", "esp_ota_end error: %d", err);
        return ESP_FAIL;
    }

    // Lets update the partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE("OTA", "esp_ota_set_boot_partition error %d", err);
        return ESP_FAIL;
    }

    const auto bootPartition = esp_ota_get_boot_partition();
    ESP_LOGI("OTA", "Next boot partition subtype %d at offset 0x%x",
        bootPartition->subtype, bootPartition->address);
    httpd_resp_sendstr(req, "OTA update successful\n");
    ESP_LOGI("OTA", "OTA update successful, restarting system...");
    xTaskCreate(&rebootTask, "rebootTask", 1024, NULL, 1, NULL);

    return ESP_OK;
}
