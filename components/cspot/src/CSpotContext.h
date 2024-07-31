#pragma once

#include <stdint.h>
#include <memory>

#include "Crypto.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "TimeProvider.h"
#include "protobuf/authentication.pb.h"  // for AuthenticationType_AUTHE...
#include "protobuf/metadata.pb.h"
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/detail/json_pointer.hpp"  // for json_pointer<>::string_t
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

namespace cspot {
struct Context {
  struct ConfigState {
    // Setup default bitrate to 160
    AudioFormat audioFormat = AudioFormat::AudioFormat_OGG_VORBIS_160;
    std::string deviceId;
    std::string deviceName;
    std::vector<uint8_t> authData;
    int volume;

    std::string username;
    std::string countryCode;
  };
  ConfigState mConfig;
  TimeProvider mTimeProvider;
  MercurySession mSession;

  std::string getCredentialsJson() {
#ifdef BELL_ONLY_CJSON
    cJSON* json_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(json_obj, "authData",
                            Crypto::base64Encode(mConfig.authData).c_str());
    cJSON_AddNumberToObject(
        json_obj, "authType",
        AuthenticationType_AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS);
    cJSON_AddStringToObject(json_obj, "username", mConfig.username.c_str());

    char* str = cJSON_PrintUnformatted(json_obj);
    cJSON_Delete(json_obj);
    std::string json_objStr(str);
    free(str);

    return json_objStr;
#else
    nlohmann::json obj;
    obj["authData"] = Crypto::base64Encode(config.authData);
    obj["authType"] =
        AuthenticationType_AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS;
    obj["username"] = config.username;

    return obj.dump();
#endif
  }

  Context(const LoginBlob& blob): mSession(blob, mTimeProvider)
  {
      mConfig.deviceId = blob.getDeviceId();
      mConfig.deviceName = blob.getDeviceName();
      mConfig.authData = blob.authData;
      mConfig.volume = 0;
      mConfig.username = blob.getUserName();
  }
};
}  // namespace cspot
