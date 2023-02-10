#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
#define BQ_DEBUG
#include "equalizer.hpp"
#include "audioNode.hpp"
#include "volume.hpp"

struct IEqualizerCore
{
    enum Type { kTypeUnknown = 0, kTypeEsp = 1, kTypeCustom = 2 };
    typedef void(*ProcessFunc)(AudioNode::DataPullReq&, void* arg);
    virtual Type type() const = 0;
    virtual uint8_t numBands() const = 0;
    virtual void init(StreamFormat fmt, int8_t* gains) = 0;
    virtual ProcessFunc getProcessFunc(StreamFormat fmt, void*& arg) = 0;
    virtual void setBandGain(uint8_t band, int8_t dbGain) = 0;
    virtual void setAllGains(const int8_t* gains=nullptr) = 0;
    virtual EqBandConfig bandConfig(uint8_t n) const = 0;
    virtual ~IEqualizerCore() {}
};

struct EqBandConfig;
template<int N>
class MyEqualizerCore: public IEqualizerCore
{
protected:
    Equalizer<N, float> mEqualizerLeft;
    Equalizer<N, float> mEqualizerRight;
    static void process16bitStereo(AudioNode::DataPullReq&, void* arg);
    static void process32bitStereo(AudioNode::DataPullReq&, void* arg);
public:
    MyEqualizerCore();
    Type type() const override { return kTypeCustom; }
    void* operator new(size_t size) { printf("eq core malloc %zu\n", size); return heap_caps_malloc(size, MALLOC_CAP_DMA); }
    void operator delete(void* ptr) noexcept { free(ptr); }
    MyEqualizerCore(const EqBandConfig* cfg);
    virtual uint8_t numBands() const override { return N; }
    virtual void init(StreamFormat fmt, int8_t* gains) override {
        auto sr = fmt.sampleRate();
        mEqualizerLeft.init(sr, gains); mEqualizerRight.init(sr, gains);
    }
    virtual ProcessFunc getProcessFunc(StreamFormat fmt, void*& arg) override;
    virtual void setBandGain(uint8_t band, int8_t dbGain) override {
        mEqualizerLeft.setBandGain(band, dbGain); mEqualizerRight.setBandGain(band, dbGain);
    }
    virtual void setAllGains(const int8_t* gains) override {
        mEqualizerLeft.setAllGains(gains); mEqualizerRight.setAllGains(gains);
    }
    virtual EqBandConfig bandConfig(uint8_t n) const override {
        return mEqualizerLeft.bandConfigs()[n];
    }
};
class EspEqualizerCore: public IEqualizerCore
{
protected:
    void* mEqualizer = nullptr;
    int mSampleRate = 0; // cache these because the esp eq wants them passed for each process() call
    int8_t mChanCount = 0;
    static void process16bitStereo(AudioNode::DataPullReq&, void* arg);
public:
    Type type() const override { return kTypeEsp; }
    uint8_t numBands() const { return 10; }
    EspEqualizerCore();
    void init(StreamFormat fmt, int8_t* gains) override;
    void setBandGain(uint8_t band, int8_t dbGain) override;
    void setAllGains(const int8_t *gains) override;
    EqBandConfig bandConfig(uint8_t n) const override;
    ProcessFunc getProcessFunc(StreamFormat fmt, void*& arg) override {
        assert(fmt.bitsPerSample() == 16);
        arg = this;
        return process16bitStereo;
    }
    virtual ~EspEqualizerCore();
};

class NvsHandle;
class EqualizerNode: public AudioNode, public DefaultVolumeImpl
{
protected:
    enum { kMyEqMinBands = 4, kMyEqMaxBands = 10, kMyEqDefaultNumBands = 8 };
    NvsHandle& mNvsHandle;
    StreamFormat mFormat;
    int mSampleRate = 0; // cached from mFormat, for performance
    std::unique_ptr<IEqualizerCore> mCore;
    IEqualizerCore::ProcessFunc mProcessFunc = nullptr;
    void* mProcFuncArg = nullptr;
    bool mUseEspEq;
    uint8_t mMyEqDefaultNumBands;
    bool mBypass = false;
    std::unique_ptr<int8_t[]> mGains;
    char mEqName[10] = {};
    std::string eqNameKey() const;
    void equalizerReinit(StreamFormat fmt, bool forceLoadGains=false);
    void updateBandGain(uint8_t band);
    void createCustomCore(uint8_t nBands, StreamFormat fmt);
public:
    Mutex mMutex;
    EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs);
    virtual Type type() const { return kTypeEqualizer; }
    virtual StreamError pullData(DataPullReq &dpr) override;
    virtual void confirmRead(int size) override { mPrev->confirmRead(size); }
    int numBands() const { return mCore->numBands(); }
    bool setMyEqNumBands(uint8_t n);
    IEqualizerCore::Type eqType() const { return mCore->type(); }
    const char* presetName() const { return mEqName; }
    bool switchPreset(const char* name);
    void useEspEqualizer(bool use);
    void disable(bool disabled) { mBypass = disabled; }
    bool setBandGain(uint8_t band, int8_t dbGain);
    void setAllGains(const int8_t* gains, int len);
    const int8_t* gains() { return mGains.get(); }
    bool saveGains();
    const EqBandConfig bandCfg(uint8_t n) const { return mCore->bandConfig(n); }
    virtual IAudioVolume* volumeInterface() override { return this; }
};

#endif // EQUALIZERNODE_HPP
