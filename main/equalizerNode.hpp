#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
//#define BQ_DEBUG
#include "equalizer.hpp"
#include "audioNode.hpp"
#include "volume.hpp"

struct IEqualizerCore
{
    enum Type { kTypeUnknown = 0, kTypeEsp = 1, kTypeCustom = 2 };
    typedef void(*ProcessFunc)(DataPacket& pkt, void* arg);
    virtual Type type() const = 0;
    virtual uint8_t numBands() const = 0;
    virtual void init(StreamFormat fmt, int8_t* gains) = 0;
    virtual ProcessFunc getProcessFunc(StreamFormat fmt) = 0;
    virtual void setBandGain(uint8_t band, int8_t dbGain) = 0;
    virtual void setAllGains(const int8_t* gains=nullptr) = 0;
    virtual EqBandConfig bandConfig(uint8_t n) const = 0;
    virtual ~IEqualizerCore() {}
};

struct EqBandConfig;
template<int N, bool IsStereo>
class MyEqualizerCore: public IEqualizerCore
{
protected:
    Equalizer<N, IsStereo> mEqualizer;
    static void processFloat(DataPacket& pkt, void* arg);
public:
    MyEqualizerCore();
    Type type() const override { return kTypeCustom; }
    void* operator new(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_DMA); }
    void operator delete(void* ptr) noexcept { free(ptr); }
    MyEqualizerCore(const EqBandConfig* cfg);
    virtual uint8_t numBands() const override { return N; }
    virtual void init(StreamFormat fmt, int8_t* gains) override {
        mEqualizer.init(fmt.sampleRate(), gains);
    }
    virtual ProcessFunc getProcessFunc(StreamFormat fmt) override;
    virtual void setBandGain(uint8_t band, int8_t dbGain) override {
        mEqualizer.setBandGain(band, dbGain);
    }
    virtual void setAllGains(const int8_t* gains) override {
        mEqualizer.setAllGains(gains);
    }
    virtual EqBandConfig bandConfig(uint8_t n) const override {
        return mEqualizer.bandConfigs()[n];
    }
};
class EspEqualizerCore: public IEqualizerCore
{
protected:
    void* mEqualizer = nullptr;
    int mSampleRate = 0; // cache these because the esp eq wants them passed for each process() call
    int8_t mChanCount = 0;
    static void process16bitStereo(DataPacket&, void* arg);
public:
    Type type() const override { return kTypeEsp; }
    uint8_t numBands() const { return 10; }
    EspEqualizerCore();
    void init(StreamFormat fmt, int8_t* gains) override;
    void setBandGain(uint8_t band, int8_t dbGain) override;
    void setAllGains(const int8_t *gains) override;
    EqBandConfig bandConfig(uint8_t n) const override;
    ProcessFunc getProcessFunc(StreamFormat fmt) override {
        return process16bitStereo;
    }
    virtual ~EspEqualizerCore();
};

class NvsHandle;
class EqualizerNode: public AudioNode, public DefaultVolumeImpl
{
protected:
    enum { kMyEqMinBands = 3, kMyEqMaxBands = 10, kMyEqDefaultNumBands = 8 };
    typedef void(EqualizerNode::*PreConvertFunc)(PacketResult& pr);
    typedef void(EqualizerNode::*PostConvertFunc)(DataPacket& pkt);
    NvsHandle& mNvsHandle;
    StreamFormat mInFormat;
    StreamFormat mOutFormat;
    int mSampleRate = 0; // cached from mFormat, for performance
    std::unique_ptr<IEqualizerCore> mCore;
    IEqualizerCore::ProcessFunc mProcessFunc = nullptr;
    bool mUseEspEq;
    bool mOut24bit;
    uint8_t mMyEqDefaultNumBands;
    bool mBypass = false;
    bool mCoreChanged = false;
    StreamId mStreamId = 0;
    uint8_t mSourceBps = 0;
    char mEqName[10] = {};
    std::unique_ptr<EqBandConfig[]> mBandConfigs;
    std::unique_ptr<int8_t[]> mGains;
    float mFloatVolumeMul = 1.0;
    PreConvertFunc mPreConvertFunc = nullptr;
    PostConvertFunc mPostConvertFunc = nullptr;
    std::string eqNameKey() const;
    std::string eqConfigKey(uint8_t nBands) const;
    void loadEqConfig(uint8_t nBands);
    void equalizerReinit(StreamFormat fmt=0, bool forceLoadGains=false);
    void updateBandGain(uint8_t band);
    void createCustomCore(uint8_t nBands, StreamFormat fmt);
    template<int Bps>
    void samples24or32ToFloatAndApplyVolume(PacketResult& pr);
    template <typename S>
    void samples16or8ToFloatAndApplyVolume(PacketResult& pr);
    void floatSamplesTo24bitAndGetLevelsStereo(DataPacket& pkt);
    void floatSamplesTo16bitAndGetLevelsStereo(DataPacket& pkt);
    template<int Bps>
    void samplesTo16bitAndApplyVolume(PacketResult& pr);
public:
    Mutex mMutex;
    EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs);
    virtual Type type() const { return kTypeEqualizer; }
    virtual StreamEvent pullData(PacketResult &dpr) override;
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
    bool reconfigEqBand(uint8_t band, uint16_t freq, int8_t bw);
    const EqBandConfig bandCfg(uint8_t n) const { return mCore->bandConfig(n); }
    virtual IAudioVolume* volumeInterface() override { return this; }
    virtual void setVolume(uint8_t vol) override {
        printf("setVolume->%u\n", vol);
        DefaultVolumeImpl::setVolume(vol);
        mFloatVolumeMul = (float)vol / 100;
    }
};

#endif // EQUALIZERNODE_HPP
