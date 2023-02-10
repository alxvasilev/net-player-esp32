#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
#define BQ_DEBUG
#include "equalizer.hpp"
#include "audioNode.hpp"
#include "volume.hpp"

struct IEqualizerCore
{
    typedef void(*ProcessFunc)(AudioNode::DataPullReq&, void* arg);
    virtual int bandCount() const = 0;
    virtual void init(int sampleRate, int8_t* gains) = 0;
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
    void* operator new(size_t size) { printf("eq core malloc %zu\n", size); return heap_caps_malloc(size, MALLOC_CAP_DMA); }
    void operator delete(void* ptr) noexcept { free(ptr); }
    MyEqualizerCore(const EqBandConfig* cfg);
    virtual int bandCount() const { return N; }
    virtual void init(int sampleRate, int8_t* gains) override {
        mEqualizerLeft.init(sampleRate, gains); mEqualizerRight.init(sampleRate, gains);
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
class NvsHandle;
class EqualizerNode: public AudioNode, public DefaultVolumeImpl
{
protected:
    enum { kMinBands = 4, kMaxBands = 10 };
    NvsHandle& mNvsHandle;
    StreamFormat mFormat;
    int mSampleRate = 0; // cached from mFormat, for performance
    std::unique_ptr<IEqualizerCore> mCore;
    IEqualizerCore::ProcessFunc mProcessFunc = nullptr;
    void* mProcFuncArg = nullptr;
    uint8_t mNumBands;
    uint8_t mChanCount = 0; // cached from mFormat, for performance
    bool mBypass = false;
    std::unique_ptr<int8_t[]> mGains;
    std::string eqName();
    void equalizerReinit(StreamFormat fmt);
    void updateBandGain(uint8_t band);
    void createCoreForNBands(int n, StreamFormat fmt);
public:
    Mutex mMutex;
    EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs);
    virtual Type type() const { return kTypeEqualizer; }
    virtual StreamError pullData(DataPullReq &dpr) override;
    virtual void confirmRead(int size) override { mPrev->confirmRead(size); }
    int numBands() const { return mNumBands; }
    bool setNumBands(uint8_t n);
    void disable(bool disabled) { mBypass = disabled; }
    void setBandGain(uint8_t band, int8_t dbGain);
    void setAllGains(const int8_t* gains, int len);
    const int8_t* gains() { return mGains.get(); }
    bool saveGains();
    const EqBandConfig bandCfg(uint8_t n) const { return mCore->bandConfig(n); }
    virtual IAudioVolume* volumeInterface() override { return this; }
};

#endif // EQUALIZERNODE_HPP
