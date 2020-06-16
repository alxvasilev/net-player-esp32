#ifndef OTA_HPP_INCLUDED
#define OTA_HPP_INCLUDED

bool rollbackIsPendingVerify();
void rollbackConfirmAppIsWorking();
const char* otaPartitionStateToStr(esp_ota_img_states_t state);
bool setOtherPartitionBootableAndRestart();
// Web server request handler to register with the applcation's server
extern const httpd_uri_t otaUrlHandler;

// This callback is called just before OTA starts. The application can
// use it to stop any ongoing process
typedef void(*OtaNotifyCallback)();
extern OtaNotifyCallback otaNotifyCallback;

#endif
