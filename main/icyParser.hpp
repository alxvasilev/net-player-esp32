#include "buffer.hpp"
#include "mutex.hpp"
class IcyInfo
{
protected:
    BufPtr<char> mTrackName = nullptr;
    BufPtr<char> mStaName = nullptr;
    BufPtr<char> mStaDesc = nullptr;
    BufPtr<char> mStaGenre = nullptr;
    BufPtr<char> mStaUrl = nullptr;
    Mutex& mInfoMutex;
public:
    IcyInfo(Mutex& mutex): mInfoMutex(mutex) {}
    void clearIcyInfo();
    const char* trackName() const { return mTrackName.ptr(); }
    const char* staName() const { return mStaName.ptr(); }
    const char* staDesc() const { return mStaDesc.ptr(); }
    const char* staGenre() const { return mStaGenre.ptr(); }
    const char* staUrl() const { return mStaUrl.ptr(); }
};

class IcyParser: public IcyInfo
{
protected:
    // stream state
    int32_t mIcyCtr = 0;
    int32_t mIcyInterval = 0;
    int16_t mIcyRemaining = 0;
    int32_t mIcyEventAfterBytes = -1;
    DynBuffer mIcyMetaBuf;
public:
    IcyParser(Mutex& infoMutex): IcyInfo(infoMutex) {}
    int32_t icyInterval() const { return mIcyInterval; }
    void reset();
    bool parseHeader(const char* key, const char* value);
    bool processRecvData(char* buf, int& rlen);
    void parseIcyData(); // we extract only StreamTitle
};
