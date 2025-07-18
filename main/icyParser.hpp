#include <buffer.hpp>
#include <mutex.hpp>
class IcyInfo
{
protected:
    struct MallocFreeDeleter {
        void operator()(const void* ptr) const { ::free((void*)ptr); }
    };
    template <typename T>
    using unique_ptr_mfree = std::unique_ptr<T, MallocFreeDeleter>;

    unique_ptr_mfree<char> mTrackName;
    unique_ptr_mfree<char> mStaName;
    unique_ptr_mfree<char> mStaDesc;
    unique_ptr_mfree<char> mStaGenre;
    unique_ptr_mfree<char> mStaUrl;
    Mutex& mInfoMutex;
public:
    IcyInfo(Mutex& mutex): mInfoMutex(mutex) {}
    void clearIcyInfo();
    const char* trackName() const { return mTrackName.get(); }
    const char* staName() const { return mStaName.get(); }
    const char* staDesc() const { return mStaDesc.get(); }
    const char* staGenre() const { return mStaGenre.get(); }
    const char* staUrl() const { return mStaUrl.get(); }
};

class IcyParser: public IcyInfo
{
protected:
    // stream state
    int32_t mIcyCtr = 0;
    int32_t mIcyInterval = 0;
    int16_t mIcyRemaining = 0;
    DynBuffer mIcyMetaBuf;
    void parseIcyData(); // we extract only StreamTitle
public:
    IcyParser(Mutex& infoMutex): IcyInfo(infoMutex) {}
    int32_t icyInterval() const { return mIcyInterval; }
    int32_t bytesSinceLastMeta() const { return mIcyCtr; }
    void reset();
    bool parseHeader(const char* key, const char* value);
    bool processRecvData(char* buf, int& rlen);
};
