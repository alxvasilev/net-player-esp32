#include "ApResolve.h"

#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <memory>            // for allocator, unique_ptr
#include <string_view>       // for string_view
#include <vector>            // for vector

#include <httpClient.hpp>
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

ApResolve::ApResolve(std::string apOverride) {
  this->apOverride = apOverride;
}

std::string ApResolve::fetchFirstApAddress() {
  if (apOverride != "") {
    return apOverride;
  }

  HttpClient client;
  DynBuffer response = client.get("https://apresolve.spotify.com/");
  response.nullTerminate();
  // parse json with nlohmann
#ifdef BELL_ONLY_CJSON
  cJSON* json = cJSON_Parse(response.buf());
  auto ap_string = std::string(
      cJSON_GetArrayItem(cJSON_GetObjectItem(json, "ap_list"), 0)->valuestring);
  cJSON_Delete(json);
  return ap_string;
#else
  auto json = nlohmann::json::parse(responseStr);
  return json["ap_list"][0];
#endif
}
