#ifndef OTA_HPP_INCLUDED
#define OTA_HPP_INCLUDED

bool rollbackIsPendingVerify();
void rollbackConfirmAppIsWorking();
const char* otaPartitionStateToStr(esp_ota_img_states_t state);
bool setOtherPartitionBootableAndRestart();

extern const httpd_uri_t otaUrlHandler;

#endif
