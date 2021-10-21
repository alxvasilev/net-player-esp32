#include "stationList.hpp"
#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include "utils.hpp"
#include "autoString.hpp"
#include <cJSON.h>

static const char* TAG = "STALIST";
using namespace std;

StationList::StationList(Mutex& aMutex, const char *nsName)
    :mNsName(strdup(nsName)), mutex(aMutex), currStation(*this)
{
    auto err = nvs_open_from_partition("nvs", mNsName, NVS_READWRITE, &mNvsHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        mNvsHandle = 0;
        return;
    }
    loadCurrent();
}
char* StationList::getString(const char* key)
{
    size_t len;
    if (nvs_get_str(mNvsHandle, key, nullptr, &len) != ESP_OK) {
        return nullptr;
    }
    char* result = (char*)malloc(len + 1);
    esp_err_t err;
    if ((err = nvs_get_str(mNvsHandle, key, result, &len)) != ESP_OK) {
        ESP_LOGE(TAG, "getString: Error reading string value: %s", esp_err_to_name(err));
        free(result);
        return nullptr;
    }
    result[len] = 0;
    return result;
}

bool StationList::loadCurrent()
{
    AutoCString curr = getString("_curr");
    if (curr && currStation.load(curr.c_str())) {
        return true;
    }
    ESP_LOGW(TAG, "Current station record not found or invalid, loading first station");
    return getNext(nullptr, currStation);
}

bool StationList::getNext(const char* after, Station& station)
{
    nvs_iterator_t it = nvs_entry_find("nvs", mNsName, NVS_TYPE_BLOB);
    if (after) {
        for(; it; it = nvs_entry_next(it)) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (strcmp(info.key, after) == 0) {
                break;
            }
        }
        if (!it) {
            return false;
        }
        it = nvs_entry_next(it);
    }
    // it may be null
    for(; it; it = nvs_entry_next(it)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (info.key[0] == '_') {
            continue;
        }
        const char* key = (info.key[0] == ':') ? info.key + 1 : info.key;
        if (!key) {
            continue;
        }
        if (station.load(key)) {
            nvs_release_iterator(it);
            return true;
        }
    }
    nvs_release_iterator(it); // should already be null
    return false;
}
bool StationList::setCurrent(const char* id)
{
    if (currStation.isValid() && strcmp(currStation.id(), id) == 0) {
        return true;
    }
    if (!currStation.load(id)) { // will not destroy current data if load fails
        return false;
    }
    return true;
}

bool StationList::bookmarkCurrent() {
    if (!currStation.isValid()) {
        return false;
    }
    AutoCString curr = getString("_curr");
    if (strcmp(curr.c_str(), currStation.id()) == 0) {
        return true;
    }
    bool ok = nvs_set_str(mNvsHandle, "_curr", currStation.id()) == ESP_OK;
    nvs_commit(mNvsHandle);
    return ok;
}

bool StationList::next()
{
    if (!currStation.isValid()) {
        return getNext(nullptr, currStation);
    } else {
        if (getNext(currStation.id(), currStation)) {
            return true;
        }
        return getNext(nullptr, currStation);
    }
}
bool StationList::stationExists(const char* id)
{
    size_t len;
    return nvs_get_blob(mNvsHandle, id, nullptr, &len) == ESP_OK;
}

bool Station::load(const char* id)
{
    size_t len;
    auto ret = nvs_get_blob(mParent.nvsHandle(), id, nullptr, &len);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Station::load: Error reading record for id '%s'", id);
        }
        return false;
    }
    char* data = (char*)malloc(len + 1);
    auto err = nvs_get_blob(mParent.nvsHandle(), id, data, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "loadStation: Error reading data for station '%s': %s", id, esp_err_to_name(err));
        return false;
    }
    data[len] = 0; // null-terminate last string for convenience
    if (!parse(id, data, len + 1)) {
        free(data);
        return false;
    }
    loadFlags();
    return true;
}
bool Station::parse(const char* aId, char* data, int dataLen)
{
    // urllen.2 url namelen.1 name notesLen.1 notes NULL
    if (dataLen < 10) {
        ESP_LOGW(TAG, "parse: Data record is too small (%d bytes)", dataLen);
        return false;
    }
    if (data[dataLen-1] != 0) {
        ESP_LOGE(TAG, "parse: Station record is not null-terminated");
        return false;
    }
    dataLen--;
    int remain = dataLen;
    int urlLen = *((uint16_t*)data);
    remain -= urlLen + 2;
    if (remain <= 0) {
        ESP_LOGW(TAG, "parse: URL length %d spans outside record data", urlLen);
        return false;
    }
    int nameLen = *((uint8_t*)(data + 2 + urlLen));
    remain -= nameLen + 1;
    if (remain <= 0) {
        ESP_LOGW(TAG, "parse: Name length %d spans outside record data", nameLen);
        return false;
    }
    int notesLen = *((uint8_t*)(data + 3 + urlLen + nameLen));
    remain -= notesLen + 1;
    if (remain < 0) {
        ESP_LOGW(TAG, "parse: Notes length %d spans outside record data", notesLen);
        return false;
    }
    //DynBuffer buf(data, dataLen);
    //printf("load: data='%s'\n", buf.toString());
    clear();
    mBuf = data;
    mId = strdup(aId);
    mUrl = data + 2;
    mName = mUrl + urlLen + 1;
    mNotes = mName + nameLen + 1;
    mUrl[urlLen] = 0; // use the nameLen byte as terminating null
    mName[nameLen] = 0; // use the notesLen byte as terminating null
    mNotes[notesLen] = 0;
    return true;
}
bool Station::loadFlags()
{
    std::string key = ":";
    key.append(mId);
    if (nvs_get_u16(mParent.nvsHandle(), key.c_str(), &mFlags) != ESP_OK) {
        ESP_LOGI(TAG, "loadFlags: No flags record for station %s", mId);
        mFlags = 0;
        return false;
    }
    return true;
}

void Station::clear()
{
    if (mBuf) {
        free(mBuf);
        mBuf = nullptr;
    } else {
        if (mUrl) {
            free(mUrl);
        }
        if (mName) {
            free(mName);
        }
        if (mNotes) {
            free(mNotes);
        }
    }
    mUrl = mName = mNotes = nullptr;
    mFlags = 0;
}

Station::Station(StationList& aParent, const char* id)
: Station(aParent)
{
    load(id);
}

void Station::startEditing(char* skip)
{
    mDirty = true;
    if (!mBuf) {
        return;
    }
    mUrl = (mUrl == skip) ? nullptr : strdup(mUrl);
    mName = (mName == skip) ? nullptr : strdup(mName);
    if (mNotes) {
        mNotes = (mNotes == skip) ? nullptr : strdup(mNotes);
    }
    free(mBuf);
    mBuf = nullptr;
}
Station& Station::setId(const char* id)
{
    if (mId) {
        free(mId);
    }
    mId = strdup(id);
    mDirty = true;
    return *this;
}
Station& Station::setUrl(const char* url)
{
    startEditing(mUrl);
    if (mUrl) {
        free(mUrl);
    }
    mUrl = strdup(url);
    return *this;
}
Station& Station::setName(const char* name)
{
    startEditing(mName);
    if (mName) {
        free(mName);
    }
    mName = strdup(name);
    return *this;
}
Station& Station::setNotes(const char* notes)
{
    startEditing(mNotes);
    if (mNotes) {
        free(mNotes);
    }
    mNotes = strdup(notes);
    return *this;
}

StationList::~StationList()
{
    if (mNvsHandle) {
        nvs_close(mNvsHandle);
        mNvsHandle = 0;
    }
}

bool Station::save()
{
    if (!isValid()) {
        return false;
    }
    if (!mDirty) {
        return true;
    }
    // urlLen.2 url nameLen.1 name notesLen.1 [notes]
    int urlLen = strlen(mUrl);
    if (urlLen > 1024) {
        ESP_LOGW(TAG, "URL is longer than 1024 bytes");
        return false;
    }
    int nameLen = strlen(mName);
    if (nameLen > 0xff) {
        nameLen = 0xff;
        mName[0xff] = 0;
    }
    DynBuffer data(urlLen + nameLen + 16);
    data.append<uint16_t>(urlLen);
    data.append(mUrl, urlLen);
    data.append<uint8_t>(nameLen);
    data.append(mName, nameLen);
    if (mNotes) {
        int notesLen = strlen(mNotes);
        if (notesLen > 0xff) {
            notesLen = 0xff;
            mNotes[0xff] = 0;
        }
        data.append<uint8_t>(notesLen);
        data.appendStr(mNotes);
    } else {
        data.append<uint8_t>(0);
    }
    auto err = nvs_set_blob(mParent.nvsHandle(), mId, data.buf(), data.dataSize());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: Error writing station data: %s", esp_err_to_name(err));
        return false;
    }
    data.freeBuf(); // we don't need this memory anymore
    saveFlags();
    nvs_commit(mParent.nvsHandle());
    mDirty = false;
    return true;
}
bool Station::saveFlags()
{
    auto idLen = strlen(mId);
    DynBuffer key(idLen + 2);
    key.appendChar(':').append(mId, idLen).nullTerminate();
    uint16_t flags;
    if (nvs_get_u16(mParent.nvsHandle(), key.buf(), &flags) == ESP_OK)
    {
        if (flags == mFlags) {
            return true;
        }
    } else if (mFlags == 0) {
        return true;
    }
    auto err = nvs_set_u16(mParent.nvsHandle(), key.buf(), mFlags);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: Error writing flags: %s", esp_err_to_name(err));
        return false;
    }
    nvs_commit(mParent.nvsHandle());
    return true;
}

bool Station::appendToJson(std::string& json)
{
    if (!isValid()) {
        ESP_LOGW(TAG, "appendToJson: Station is not valid");
        return false;
    }

    json.append("{\"id\":\"").append(mId)
        .append("\",\"n\":\"").append(jsonStringEscape(mName))
        .append("\",\"url\":\"").append(jsonStringEscape(mUrl));
    if (mNotes) {
        json.append("\",\"nts\":\"").append(jsonStringEscape(mNotes));
    }
    json.append("\",\"f\":").append(std::to_string(mFlags)) +='}';
    return true;
}
const char* Station::jsonStringProp(const cJSON* json, const char* name, bool mustExist)
{
    auto prop = cJSON_GetObjectItem(json, name);
    if (!prop) {
        if (!mustExist) {
            return nullptr;
        }
        throw runtime_error("Missing property '" + string(name) + "'");
    }
    if (prop->type != cJSON_String) {
        throw runtime_error("Property '" + string(name) + "' is not a string");
    }
    return prop->valuestring;
}

int Station::jsonIntProp(const cJSON* json, const char* name, bool mustExist, int defaultVal)
{
    auto prop = cJSON_GetObjectItem(json, name);
    if (!prop) {
        if (!mustExist) {
            return defaultVal;
        }
        throw runtime_error("Missing property '" + string(name) + "'");
    }
    if (prop->type != cJSON_Number) {
        throw runtime_error("Property '" + string(name) + "' is not a number");
    }
    return prop->valueint;
}

void Station::loadFromJson(const cJSON* json, const char* id)
{
    if (!id) {
        id = jsonStringProp(json, "id", true);
    }
    setId(id);
    setUrl(jsonStringProp(json, "url", true));
    setName(jsonStringProp(json, "n", true));
    auto notes = jsonStringProp(json, "nts", false);
    if (notes) {
        setNotes(notes);
    }
    auto flags = jsonIntProp(json, "f", false, -1);
    if (flags >= 0) {
        setFlags(flags);
    }
}

bool StationList::remove(const char* id)
{
    DynBuffer buf(16);
    buf.appendChar(':').appendStr(id);
    nvs_erase_key(mNvsHandle, buf.buf());
    return (nvs_erase_key(mNvsHandle, id) == ESP_OK);
}

template<class CB>
void StationList::enumerate(CB&& callback)
{
    nvs_iterator_t it = nvs_entry_find("nvs", mNsName, NVS_TYPE_BLOB);
    Station station(*this);
    for(; it; it = nvs_entry_next(it)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        char first = info.key[0];
        if (first == '_' || first == ':') {
            continue;
        }
        if (station.load(info.key)) {
            callback(station);
        }
    }
    nvs_release_iterator(it);
}
void StationList::registerHttpHandler(httpd_handle_t server)
{
/*    httpd_uri_t desc = {
        .uri       = "/slist",
        .method    = HTTP_GET,
        .handler   = httpHandler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server, &desc);*/
    httpd_uri_t desc2 = {
        .uri       = "/slist/import",
        .method    = HTTP_POST,
        .handler   = httpImportHandler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server, &desc2);
}

esp_err_t StationList::httpHandler(httpd_req_t* req)
{
    auto& self = *static_cast<StationList*>(req->user_ctx);
    MutexLocker locker(self.mutex);

    UrlParams params(req);
    auto action = params.strVal("a");
    if (!action) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No 'a' param specified");
        return ESP_FAIL;
    }
    if (strcmp(action.str, "dump") == 0) {
        return self.httpDumpAllStations(req, params);
    } else if (strcmp(action.str, "edit") == 0) {
        return self.httpEditStation(req, params, true);
    } else if (strcmp(action.str, "new") == 0) {
        return self.httpEditStation(req, params, false);
    } else if (strcmp(action.str, "setf") == 0) {
        return self.httpSetStationFlags(req, params);
    } else if (strcmp(action.str, "del") == 0) {
        return self.httpDelStation(req, params);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action");
        return ESP_FAIL;
    }
}
esp_err_t StationList::httpDumpAllStations(httpd_req_t* req, UrlParams& params)
{
    httpd_resp_sendstr_chunk(req, "{\"sl\":[");
    std::string str;
    bool first = true;
    enumerate([&](Station& station) {
        if (first) {
            first = false;
        } else {
            str = ",";
        }
        station.appendToJson(str);
        httpd_resp_send_chunk(req, str.c_str(), str.size());
        vTaskDelay(2);
    });
    if (currStation.isValid()) {
        str = "],\"curr\":\"";
        str.append(currStation.id()).append("\"}");
        httpd_resp_send_chunk(req, str.c_str(), str.size());
    } else {
        httpd_resp_sendstr_chunk(req, "]}");
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t StationList::httpEditStation(httpd_req_t* req, UrlParams& params, bool existing)
{
    auto id = params.strVal("id");
    if (!id) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No id specified");
        return ESP_FAIL;
    }
    Station station(*this);
    if (existing) {
        if (!station.load(id.str)) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Station not found");
            return ESP_FAIL;
        }
    } else {
        if (!params.intVal("ovr", 0) && stationExists(id.str)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Station with that id already exists");
            return ESP_FAIL;
        }
        station.setId(id.str);
    }
    auto p = params.strVal("n");
    if (p) {
        unescapeUrlParam(p.str, p.len);
        station.setName(p.str);
    }
    p = params.strVal("url");
    if (p) {
        unescapeUrlParam(p.str, p.len);
        station.setUrl(p.str);
    }
    p = params.strVal("nts");
    if (p) {
        unescapeUrlParam(p.str, p.len);
        station.setNotes(p.str);
    }
    auto flags = params.intVal("f", -1);
    if (flags > -1) {
        station.setFlags(flags);
    }
    if (!station.isValid()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"err\":\"Missing required param\"}");
        return ESP_FAIL;
    }
    if (!station.save()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"err\":\"Error saving station\"}");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}
esp_err_t StationList::httpSetStationFlags(httpd_req_t* req, UrlParams& params)
{
    auto id = params.strVal("id");
    if (!id) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No id specified");
        return ESP_FAIL;
    }
    int flags = params.intVal("f", -1);
    if (flags < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'f' parameter");
        return ESP_FAIL;
    }

    Station station(*this);
    if (!station.load(id.str)) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    if (!station.setFlags(flags)) {
        httpd_resp_send_500(req);
    }
    httpd_resp_sendstr(req, "{\"ok\"}");
    return ESP_OK;
}
esp_err_t StationList::httpDelStation(httpd_req_t* req, UrlParams& params)
{
    auto id = params.strVal("id");
    if (!id) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id param");
        return ESP_FAIL;
    }
    if (!remove(id.str)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t StationList::httpImportHandler(httpd_req_t* req)
{
  try {
    auto& self = *static_cast<StationList*>(req->user_ctx);
    MutexLocker locker(self.mutex);

    int contentLen = req->content_len;
    if (contentLen > SLIST_IMPORT_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Data too big (>" MY_STRINGIFY(SLIST_IMPORT_MAX_SIZE) ")\r\n");
        return ESP_FAIL;
    } else if (contentLen < 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Data too short\r\n");
        return ESP_FAIL;
    }
    std::unique_ptr<char[]> rxBuf(new char[contentLen + 1]);
    for (int received = 0; received < contentLen;) {
        /* Read the data for the request */
        int recvLen;
        for (int numWaits = 0; numWaits < 4; numWaits++) {
            recvLen = httpd_req_recv(req, rxBuf.get() + received, contentLen - received);
            if (recvLen != HTTPD_SOCK_ERR_TIMEOUT) {
                break;
            }
        }
        if (recvLen <= 0) {
            ESP_LOGI(TAG, "PostData recv error %d", recvLen);
            return ESP_FAIL;
        }
        received += recvLen;
    }
    rxBuf.get()[contentLen] = 0;

    UrlParams params(req);
    cJSON* json = cJSON_Parse(rxBuf.get());
    cJSON* list = cJSON_GetObjectItem(json ,"sl");
    if (!list) {
        throw runtime_error("Json has no 'sl' member");
    }
    ESP_LOGI(TAG, "Importing webradio stations...");
    int overwrite = params.intVal("ovr", 0);
    for (cJSON* item = list->child; item; item = item->next) {
        auto id = cJSON_GetObjectItem(item, "id");
        if (!id || id->type != cJSON_String) {
            throw runtime_error("Station item has no 'id' member");
        }
        if (!overwrite && self.stationExists(id->valuestring)) {
            ESP_LOGI(TAG, "Skip existing station %s", id->valuestring);
            continue;
        }
        Station station(self);
        station.loadFromJson(item, id->valuestring);
        station.save();
        ESP_LOGI(TAG, "Imported station %s", id->valuestring);
    }
  } catch(exception& e) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, e.what());
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "{\"ok\"}");
  return ESP_OK;
}
