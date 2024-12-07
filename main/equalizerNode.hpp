#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
//#define BQ_DEBUG
#include "eqCores.hpp"
#include "audioNode.hpp"
#include "volume.hpp"

class NvsHandle;
class EqualizerNode: public AudioNode, public DefaultVolumeImpl
{
protected:
    enum { kMyEqMinBands = 3, kMyEqMaxBands = 20, kDefaultNumBands = 10 };
    typedef void(EqualizerNode::*PreConvertFunc)(PacketResult& pr);
    typedef void(EqualizerNode::*PostConvertFunc)(DataPacket& pkt);
    NvsHandle& mNvsHandle;
    StreamFormat mInFormat;
    StreamFormat mOutFormat;
    int mSampleRate = 0; // cached from mFormat, for performance
    std::unique_ptr<IEqualizerCore> mCore; // This is guaranteed to be non-NULL, except in equalizerReinit() which creates it
    bool mUseEspEq;
    bool mOut24bit;
    uint8_t mDefaultNumBands;
    bool mBypass = false;
    bool mCoreChanged = false;
    StreamId mStreamId = 0;
    uint8_t mSourceBps = 0;
    char mEqName[10] = {};
    float mFloatVolumeMul = 1.0;
    PreConvertFunc mPreConvertFunc = nullptr;
    PostConvertFunc mPostConvertFunc = nullptr;
    IEqualizerCore::ProcessFunc mProcessFunc = nullptr;
    uint8_t eqNumBands();
    bool eqIsDefault() const { return mEqName[0] == 0; }
    std::string eqName() const;
    std::string eqGainsKey() const { return eqName(); }
    std::string eqConfigKey() const { return eqName() + ".cfg"; }
    void loadEqConfig(uint8_t nBands);
    void equalizerReinit(StreamFormat fmt=0, bool forceLoadGains=false);
    void updateBandGain(uint8_t band);
    void createCustomCore(StreamFormat fmt);
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
    bool setDefaultNumBands(uint8_t n);
    IEqualizerCore::Type eqType() const { return mCore->type(); }
    const char* presetName() const { return mEqName[0] ? mEqName : "default"; }
    bool switchPreset(const char* name);
    void useEspEqualizer(bool use);
    void disable(bool disabled) { mBypass = disabled; }
    bool setBandGain(uint8_t band, int8_t dbGain);
    void zeroAllGains();
    const int8_t* gains() { return mCore->gains(); }
    bool saveGains();
    bool reconfigEqBand(uint8_t band, uint16_t freq, uint16_t Q);
    bool setAllPeakingQ(int Q, bool reset);
    const EqBandConfig bandCfg(uint8_t n) const { return mCore->bandConfig(n); }
    virtual IAudioVolume* volumeInterface() override { return this; }
    virtual void setVolume(uint8_t vol) override {
        printf("setVolume->%u\n", vol);
        DefaultVolumeImpl::setVolume(vol);
        mFloatVolumeMul = (float)vol / 100;
    }
};

#endif // EQUALIZERNODE_HPP
