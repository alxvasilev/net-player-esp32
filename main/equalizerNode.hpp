#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
#include "eqCores.hpp"
#include "audioNode.hpp"
#include "volume.hpp"

class NvsHandle;
class EqualizerNode: public AudioNode, public DefaultVolumeImpl
{
protected:
    enum { kMyEqMinBands = 3, kMyEqMaxBands = 20, kDefaultNumBands = 10 };
    typedef void(EqualizerNode::*PreConvertFunc)(DataPacket& pkt);
    typedef void(EqualizerNode::*PostConvertFunc)(PacketResult& pr);
    static constexpr const char kDefaultPresetPrefix[] = "deflt:";
    NvsHandle& mNvsHandle;
    StreamFormat mInFormat;
    StreamFormat mOutFormat;
    int mSampleRate = 0; // cached from mFormat, for performance
    std::unique_ptr<IEqualizerCore> mCore; // This is guaranteed to be non-NULL, except in equalizerReinit() which creates it
    uint16_t mDspBufSize = 0; // mp3 packets have 1152 samples, which is the maximum that should be allowed
    uint16_t mDspDataSize = 0;
    unique_ptr_mfree<uint8_t> mDspBuffer;
    bool mDspBufUseInternalRam;
    bool mUseEspEq;
    bool mOut24bit;
    bool mVolProbeBeforeDsp = false;
    uint8_t mDefaultNumBands;
    bool mBypass = false;
    bool mCoreTypeChanged = false;
    uint8_t mSourceBps = 0;
    StreamId mStreamId = 0;
    uint16_t mEqMaxFreqCappedTo = 0;
    std::string mEqId; // format is [e|f]:<name>[!xx] Prefix 'e' is for gains, 'f' is for config (frequnecies). !xx is for frequency-capped version
    float mFloatVolumeMul = 1.0;
    PreConvertFunc mPreConvertFunc = nullptr;
    PostConvertFunc mPostConvertFunc = nullptr;
    uint8_t eqNumBands();
    bool isDefaultPreset() const { return mEqId.size() < 2 ? false : strncmp(mEqId.c_str() + 2, kDefaultPresetPrefix, sizeof(kDefaultPresetPrefix)-1) == 0; }
    const char* eqGainsKey() { mEqId[0] = 'e'; return mEqId.c_str(); }
    const char* eqConfigKey() { mEqId[0] = 'f'; return mEqId.c_str(); }
    void eqLoadName();
    void loadEqConfig(uint8_t nBands);
    void deleteCore() { mCore.reset(); }
    void updateDefaultEqName(bool check=true);
    bool fitBandFreqsToSampleRate(EqBandConfig* config, int* nBands, int sampleRate);
    void equalizerReinit(StreamFormat fmt=0, bool forceLoadGains=false);
    void updateBandGain(uint8_t band);
    void createCustomCore(StreamFormat fmt);
    template <typename S, bool VolProbeEnabled>
    void preConvert16or8ToFloatAndApplyVolume(DataPacket& pr);
    template<int Bps, bool VolProbeEnabled>
    void preConvert24or32ToFloatAndApplyVolume(DataPacket& pr);
    template<bool VolProbeEnabled>
    void postConvertFloatTo24(PacketResult& pr);
    template<bool VolProbeEnabled>
    void postConvertFloatTo16(PacketResult& pr);
    template <typename T, bool VolProbeEnable>
    void preConvert8or16to16AndApplyVolume(DataPacket& pkt);
    template<int Bps, bool VolProbeEnabled>
    void preConvert24or32To16AndApplyVolume(DataPacket& pr);
    template <bool VolProbeEnabled>
    void postConvert16To24(PacketResult& pr);
    template <bool VolProbeEnabled>
    void postConvert16To16(PacketResult& pr);
    int preConvertFuncIndex() const;
    static const PreConvertFunc sPreConvertFuncsFloat[];
    static const PreConvertFunc sPreConvertFuncs16[];
    uint8_t* dspBufGetWritable(uint16_t writeSize) {
        if (mDspBufSize < writeSize) {
            mDspBuffer.reset((uint8_t*)heap_caps_realloc(mDspBuffer.release(), writeSize,
                mDspBufUseInternalRam ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM));
            mDspBufSize = writeSize;
        }
        mDspDataSize = writeSize;
        return mDspBuffer.get();
    }
    void dspBufRelease();
public:
    Mutex mMutex;
    EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs);
    virtual Type type() const { return kTypeEqualizer; }
    virtual StreamEvent pullData(PacketResult &dpr) override;
    int numBands() const { return mCore->numBands(); }
    bool setDefaultNumBands(uint8_t n);
    IEqualizerCore::Type eqType() const { return mCore->type(); }
    const char* presetName() const { return mEqId.size() > 2 ? mEqId.c_str() + 2 : nullptr; }
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
