#ifndef STATION_LIST_HPP
#define STATION_LIST_HPP

#include <nvs.h>
#include <esp_http_server.h>
#include <string>

class StationList;
class Station
{
protected:
    StationList& mParent;
    char* mBuf = nullptr;
    char* mId = nullptr;
    char* mName = nullptr;
    char* mUrl = nullptr;
    char* mNotes = nullptr;
    uint16_t mFlags = 0;
    bool mDirty = false;
    void clear();
    bool parse(const char* id, char* data, int dataLen);
    bool loadFlags();
    void startEditing(char* skip);
public:
    enum { kFlagFavorite = 1, kFlagRecord = 2 };
    const char* id() const { return mId; }
    const char* name() const { return mName; }
    const char* url() const { return mUrl; }
    const char* notes() const { return mNotes; }
    uint16_t flags() const { return mFlags; }
    Station(StationList& aParent): mParent(aParent) {}
    Station(StationList& aParent, const char* id); //attempts to load existing
    Station& setId(const char* id);
    Station& setUrl(const char* url);
    Station& setName(const char* name);
    Station& setNotes(const char* notes);
    bool setFlags(uint16_t flags) { mFlags = flags; return saveFlags(); }
    ~Station() { clear(); }
    bool isValid() const { return (mId && mUrl && mName); }
    bool load(const char* id);
    bool save();
    bool saveFlags();
    bool appendToJson(std::string& json);
    bool exists(const char* id) const;
    friend class StationList;
};

class UrlParams;

class StationList
{
    nvs_handle mNvsHandle;
    const char* mNsName;
    bool loadCurrent();
    bool getNext(const char* after, Station& station);
    char* getString(const char* key);
    static esp_err_t httpHandler(httpd_req_t* req);
    esp_err_t httpDumpAllStations(httpd_req_t* req, UrlParams& params);
    esp_err_t httpEditStation(httpd_req_t* req, UrlParams& params, bool exists);
    esp_err_t httpSetStationFlags(httpd_req_t* req, UrlParams& params);
    esp_err_t httpDelStation(httpd_req_t* req, UrlParams& params);
public:
    Station currStation;
    StationList(const char* nsName="stations");
    ~StationList();
    nvs_handle_t nvsHandle() const { return mNvsHandle; }
    bool setCurrent(const char* id);
    bool bookmarkCurrent();
    bool next();
    bool remove(const char* id);
    template<class CB>
    void enumerate(CB&& callback);
    void registerHttpHandler(httpd_handle_t server);
};
#endif
