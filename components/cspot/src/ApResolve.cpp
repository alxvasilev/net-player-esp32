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

  DynBuffer response = httpGet("https://apresolve.spotify.com/");
#ifdef BELL_ONLY_CJSON
  cJSON* json = cJSON_Parse(response.buf());
  const char* apstr = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "ap_list"), 0)->valuestring;
  std::string result(apstr ? apstr : "");
  cJSON_Delete(json);
  return result;
#else
  auto json = nlohmann::json::parse(responseStr);
  return json["ap_list"][0];
#endif
}
