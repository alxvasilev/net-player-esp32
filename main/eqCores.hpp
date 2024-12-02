#ifndef EQ_CORES_HPP
#define EQ_CORES_HPP
#include "equalizer.hpp"
#include "streamDefs.hpp"

class DataPacket;
struct IEqualizerCore
{
    enum Type { kTypeUnknown = 0, kTypeEsp = 1, kTypeCustom = 2 };
    typedef void(*ProcessFunc)(DataPacket& pkt, void* arg);
    virtual Type type() const = 0;
    virtual uint8_t numBands() const = 0;
    virtual int8_t* gains() = 0;
    virtual EqBandConfig* bandConfigs() = 0;
    virtual ProcessFunc getProcessFunc() const = 0;
    virtual void setBandGain(uint8_t band, int8_t dbGain) = 0;
    virtual void updateFilter(uint8_t band, bool resetState) = 0;
    virtual void updateAllFilters() = 0;
    virtual EqBandConfig bandConfig(uint8_t n) const = 0;
    virtual ~IEqualizerCore() {}
};

template<bool IsStereo>
class MyEqualizerCore: public IEqualizerCore
{
protected:
    Equalizer<IsStereo>& mEqualizer;
    static void processFloat(DataPacket& pkt, void* arg);
    MyEqualizerCore(Equalizer<IsStereo>& eq): mEqualizer(eq) {}
public:
    static MyEqualizerCore<IsStereo>* create(uint8_t numBands, uint32_t sampleRate);
    void operator delete(void* ptr) noexcept { free(ptr); }
    Type type() const override { return kTypeCustom; }
    virtual uint8_t numBands() const override { return mEqualizer.numBands(); }
    virtual ProcessFunc getProcessFunc() const override { return processFloat; }
    virtual void setBandGain(uint8_t band, int8_t dbGain) override {
        mEqualizer.setBandGain(band, dbGain);
    }
    virtual int8_t* gains() override { return mEqualizer.gains(); }
    virtual EqBandConfig* bandConfigs() override { return mEqualizer.bandConfigs(); }
    virtual void updateFilter(uint8_t band, bool resetState) { mEqualizer.updateFilter(band, resetState); }
    virtual void updateAllFilters() override { mEqualizer.updateAllFilters(true); }
    virtual EqBandConfig bandConfig(uint8_t n) const override {
        return mEqualizer.bandConfig(n);
    }
};
class EspEqualizerCore: public IEqualizerCore
{
protected:
    void* mEqualizer = nullptr;
    int mSampleRate = 0; // cache these because the esp eq wants them passed for each process() call
    int8_t mChanCount = 0;
    int8_t mGains[10];
    static void process16bitStereo(DataPacket&, void* arg);
public:
    Type type() const override { return kTypeEsp; }
    uint8_t numBands() const override { return 10; }
    virtual int8_t* gains() override { return mGains; }
    virtual EqBandConfig* bandConfigs() override { return nullptr; }
    EspEqualizerCore(StreamFormat fmt);
    void setBandGain(uint8_t band, int8_t dbGain) override;
    virtual void updateFilter(uint8_t band, bool resetState) { assert(false); }
    virtual void updateAllFilters() override;
    EqBandConfig bandConfig(uint8_t n) const override;
    ProcessFunc getProcessFunc() const override { return process16bitStereo; }
    virtual ~EspEqualizerCore();
};

#endif
