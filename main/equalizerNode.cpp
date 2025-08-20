#include "equalizerNode.hpp"
#include "eqCores.hpp"
#include <nvsHandle.hpp>
#include <esp_equalizer.h>
#include <cmath>
#include "asyncCall.hpp"

//#define EQ_PERF 1
//#define CONVERT_PERF 1

static const char* TAG = "eq";
#define LOCK_EQ() MutexLocker locker(mMutex)

EqualizerNode::EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs)
: AudioNode(parent, "equalizer"), mNvsHandle(nvs)
{
    mDefaultNumBands = mNvsHandle.readDefault("eq.nbands", (uint8_t)kDefaultNumBands);
    if (mDefaultNumBands < kMyEqMinBands || mDefaultNumBands > kMyEqMaxBands) {
        mDefaultNumBands = kDefaultNumBands;
    }
    mUseEspEq = mNvsHandle.readDefault("eq.useEsp", (uint8_t)1);
    mOut24bit = mNvsHandle.readDefault("eq.out24bit", (uint8_t)1);
    mDspBufUseInternalRam = mNvsHandle.readDefault("eq.useIntRam", (uint8_t)1);
    eqLoadName();
    equalizerReinit(StreamFormat(44100, 16, 2));
}
void EqualizerNode::eqLoadName()
{
    char name[16];
    int len = sizeof(name) - 1;
    if (mNvsHandle.readString("eq.default", name, len) == ESP_OK) {
        name[len] = 0;
        mEqId = " :";
        mEqId.append(name);
    }
    else {
        updateDefaultEqName(false);
    }
}
void EqualizerNode::updateDefaultEqName(bool check)
{
    if (check && !isDefaultPreset()) {
        return;
    }
    mEqId = " :";
    mEqId += kDefaultPresetPrefix;
    if (mUseEspEq) {
        mEqId += '0';
    }
    else {
        appendAny(mEqId, mDefaultNumBands);
        if (mEqMaxFreqCappedTo) {
            mEqId += '!';
            appendAny(mEqId, mEqMaxFreqCappedTo / 1000);
        }
    }
}
uint8_t EqualizerNode::eqNumBands()
{
    int len = mNvsHandle.getBlobSize(eqConfigKey());
    uint8_t nBands;
    if ((len > 0) && (len % sizeof(EqBandConfig) == 0)) {
        nBands = len / sizeof(EqBandConfig);
        if (nBands >= kMyEqMinBands && nBands <= kMyEqMaxBands) {
            return nBands;
        }
    }
    len = mNvsHandle.getBlobSize(eqGainsKey());
    if (len >= kMyEqMinBands && len <= kMyEqMaxBands) {
        return len;
    }
    return mUseEspEq ? 10 : mDefaultNumBands;
}
void EqualizerNode::createCustomCore(StreamFormat fmt)
{
    int sr = fmt.sampleRate();
    // If we have a frequency-capped preset, and have increased the samplerate, try the original preset
    if (mEqMaxFreqCappedTo && (sr >> 1) > mEqMaxFreqCappedTo) {
        ESP_LOGI(TAG, "We are using a freq-capped preset, samplerate increased, trying original");
        auto pos = mEqId.find_last_of('!');
        assert(pos != std::string::npos);
        mEqId = mEqId.substr(0, pos);
        mEqMaxFreqCappedTo = 0;
    }
    int nBands = eqNumBands();
    int expectedLen = nBands * sizeof(EqBandConfig);
    auto config = (EqBandConfig*)alloca(expectedLen);
    assert(config);
    auto len = expectedLen;
    auto ret = mNvsHandle.readBlob(eqConfigKey(), config, len);
    if (ret != ESP_OK || len != expectedLen) {
        auto defaultCfg = EqBandConfig::defaultCfg(nBands);
        assert(defaultCfg);
        memcpy(config, defaultCfg, expectedLen);
        ESP_LOGW(TAG, "Couldn't load config for %d-band '%s', using default", nBands, eqConfigKey());
    }
    fitBandFreqsToSampleRate(config, &nBands, sr);
    if (fmt.numChannels() >= 2) {
        mCore.reset(new MyEqualizerCore<true>(nBands, sr));
    }
    else {
        mCore.reset(new MyEqualizerCore<false>(nBands, sr));
    }
    memcpy(mCore->bandConfigs(), config, nBands * sizeof(EqBandConfig));
}
bool EqualizerNode::fitBandFreqsToSampleRate(EqBandConfig* config, int* nBands, int sampleRate)
{
    auto oriNbands = *nBands;
    auto oriEqId = mEqId;
    int nyquistFreq = sampleRate >> 1;
    EqBandConfig* topBand = config + *nBands - 1;
    if (topBand->freq < nyquistFreq) {
        return false;
    }
    ESP_LOGW(TAG, "Eq bands span outside the Nyquist frequency for samplerate %d, adjusting", sampleRate);
    mEqMaxFreqCappedTo = nyquistFreq;
    /*
       Default presets get renamed to: deflt:<nbands>!<maxBandFreq>
       (nbands is the original number, even if bands are removed)
       User presets get renamed to:      <presetName>!<maxBandFreq>
     */
    if (isDefaultPreset()) {
        updateDefaultEqName(false);
    }
    else { // append !xx suffix with the cap freq
        auto pos = mEqId.find_last_of('!');
        if (pos != std::string::npos) {
            mEqId.resize(pos + 1);
        }
        else {
            mEqId += '!';
        }
        appendAny(mEqId, mEqMaxFreqCappedTo / 1000);
    }
    // first, try to load a capped version from NVS
    int maxLen = *nBands * sizeof(EqBandConfig);
    int len = maxLen;
    bool have = (mNvsHandle.readBlob(eqConfigKey(), config, len) == ESP_OK) && (len <= maxLen) && (len % sizeof(EqBandConfig) == 0);
    if (have) { // use the saved config
        int nb = len / sizeof(EqBandConfig);
        if (nb >= kMyEqMinBands) {
            *nBands = nb;
            ESP_LOGI(TAG, "Loaded band config for Nyquist-capped eq '%s'", presetName());
        }
    }
    else { // adjust current config
        // delete peaking bands that are above Nyquist
        auto band = topBand;
        while(--band >= config) {
            if (band->freq < nyquistFreq) {
                break;
            }
            --(*nBands);
        }
        band++; // now points to new top (high-shelf) band
        band->freq = ((nyquistFreq * 90 / 50000)) * 500; // adjust high-shelf band to Nyquist freq, round at 500
        band->Q = 60; // 0.06: as the band's freq is now close to the Nyquist freq, need to widen it at the low end
        ESP_LOGI(TAG, "Modified config for '%s' to fit Nyquist limit", presetName());
    }
    if (*nBands == oriNbands) {
        return true;
    }
    // bands were removed, remove from gains as well, if we are going to use gains saved for original preset
    auto gains = (int8_t*)alloca(oriNbands);
    len = oriNbands;
    have = (mNvsHandle.readBlob(eqGainsKey(), gains, len) == ESP_OK) && (len == *nBands);
    if (have) {
        return true;
    }
    // we don't have gains for capped config, check for original preset, modify and save them
    oriEqId[0] = 'e';
    len = oriNbands;
    have = (mNvsHandle.readBlob(oriEqId.c_str(), gains, len) == ESP_OK) && (len == oriNbands);
    if (have) {
        gains[*nBands-1] = gains[oriNbands-1];
        mNvsHandle.writeBlob(eqGainsKey(), gains, *nBands);
    }
    return true;
}
const EqualizerNode::PreConvertFunc EqualizerNode::sPreConvertFuncsFloat[] = {
    &EqualizerNode::preConvert16or8ToFloatAndApplyVolume<int8_t, false>,
    &EqualizerNode::preConvert16or8ToFloatAndApplyVolume<int16_t, false>,
    &EqualizerNode::preConvert24or32ToFloatAndApplyVolume<24, false>,
    &EqualizerNode::preConvert24or32ToFloatAndApplyVolume<32, false>,
    &EqualizerNode::preConvert16or8ToFloatAndApplyVolume<int8_t, true>,
    &EqualizerNode::preConvert16or8ToFloatAndApplyVolume<int16_t, true>,
    &EqualizerNode::preConvert24or32ToFloatAndApplyVolume<24, true>,
    &EqualizerNode::preConvert24or32ToFloatAndApplyVolume<32, true>
};
const EqualizerNode::PreConvertFunc EqualizerNode::sPreConvertFuncs16[] = {
    &EqualizerNode::preConvert8or16to16AndApplyVolume<int8_t, false>,
    &EqualizerNode::preConvert8or16to16AndApplyVolume<int16_t, false>,
    &EqualizerNode::preConvert24or32To16AndApplyVolume<24, false>,
    &EqualizerNode::preConvert24or32To16AndApplyVolume<32, false>,
    &EqualizerNode::preConvert8or16to16AndApplyVolume<int8_t, true>,
    &EqualizerNode::preConvert8or16to16AndApplyVolume<int16_t, true>,
    &EqualizerNode::preConvert24or32To16AndApplyVolume<24, true>,
    &EqualizerNode::preConvert24or32To16AndApplyVolume<32, true>
};
int EqualizerNode::preConvertFuncIndex() const
{
    int idx = (mInFormat.bitsPerSample() >> 3) - 1;
    if (mVolProbeBeforeDsp) {
        idx |= 0x04;
    }
    if (idx > 7) {
        ESP_LOGE(TAG, "Unsupported bits per sample: %d", mInFormat.bitsPerSample());
        assert(false);
    }
    return idx;
}
void EqualizerNode::dspBufRelease()
{
    mDspBuffer.release();
    mDspBufSize = mDspDataSize = 0;
}
void EqualizerNode::equalizerReinit(StreamFormat fmt, bool forceLoadGains)
{
    ESP_LOGI(TAG, "Equalizer reinit");
    bool fmtChanged = fmt.asNumCode() && (fmt != mInFormat);
    if (fmtChanged) {
        mOutFormat = mInFormat = fmt;
        // The FLAC decoder, which has more DMA memory-demanding bitrates, produces smaller
        // packets (1024 samples) than the less demanding MP3 (max 1152 samples)
        // This gives us a chance to release some internal RAM when we need it most (for 96kHz i2s DMA)
        dspBufRelease();
    }
    if (mUseEspEq && mInFormat.sampleRate() <= 48000) {
        mOutFormat.setBitsPerSample(16);
        if (fmtChanged || !mCore || mCore->type() != IEqualizerCore::kTypeEsp) {
            mCore.reset(new EspEqualizerCore(mOutFormat));
            forceLoadGains = true;
            mPreConvertFunc = sPreConvertFuncs16[preConvertFuncIndex()];
            mPostConvertFunc = mOut24bit
                ? &EqualizerNode::postConvert16To24<true>
                : &EqualizerNode::postConvert16To16<true>;
        }
    }
    else { // need custom eq
        mOutFormat.setBitsPerSample(mOut24bit ? 24 : 16);
        if (fmtChanged || !mCore || mCore->type() != IEqualizerCore::kTypeCustom) {
            createCustomCore(mInFormat);
            forceLoadGains = true;
            mPreConvertFunc = sPreConvertFuncsFloat[preConvertFuncIndex()];
            mPostConvertFunc = mOut24bit
                ? &EqualizerNode::postConvertFloatTo24<true>
                : &EqualizerNode::postConvertFloatTo16<true>;
        }
    }
    // load gains
    if (forceLoadGains) {
        auto nBands = mCore->numBands();
        int len = nBands;
        if ((mNvsHandle.readBlob(eqGainsKey(), mCore->gains(), len) == ESP_OK) && (len == nBands)) {
            ESP_LOGI(TAG, "Loaded equalizer gains from NVS for '%s'", presetName());
        }
        else {
            ESP_LOGI(TAG, "Error loading or no gains for '%s', len=%d, expected=%d", presetName(), len, nBands);
            memset(mCore->gains(), 0, nBands);
        }
        mCore->updateAllFilters();
    }
}
bool EqualizerNode::setDefaultNumBands(uint8_t n)
{
    if (n < kMyEqMinBands || n > kMyEqMaxBands) {
        return false;
    }
    LOCK_EQ();
    mDefaultNumBands = n;
    if (isDefaultPreset()) {
        deleteCore();
        updateDefaultEqName(false);
        equalizerReinit();
    }
    mNvsHandle.write("eq.nbands", (uint8_t)n);
    return true;
}
bool EqualizerNode::setBandGain(uint8_t band, int8_t dbGain)
{
    LOCK_EQ();
    myassert(mCore);
    if (band > mCore->numBands()) {
        return false;
    }
    mCore->setBandGain(band, dbGain);
    return true;
}

void EqualizerNode::zeroAllGains()
{
    LOCK_EQ();
    auto nBands = mCore->numBands();
    memset(mCore->gains(), 0, nBands);
    mCore->updateAllFilters();
}
bool EqualizerNode::saveGains()
{
    int nbands;
    int8_t* gains;
    std::string key;
    {
        LOCK_EQ();
        nbands = mCore->numBands();
        gains = (int8_t*)alloca(nbands);
        memcpy(gains, mCore->gains(), nbands);
        key = eqGainsKey();
    }
    return mNvsHandle.writeBlob(key.c_str(), gains, nbands) == ESP_OK;
}
bool EqualizerNode::reconfigEqBand(uint8_t band, uint16_t freq, uint16_t Q)
{
    LOCK_EQ();
    assert(mCore);
    if (mCore->type() != IEqualizerCore::kTypeCustom) {
        ESP_LOGW(TAG, "ESP equalizer is not configurable");
        return false;
    }
    auto nBands = mCore->numBands();
    if (band > nBands) {
        ESP_LOGW(TAG, "Band %d not in range [0 - %d]", band, nBands);
        return false;
    }
    auto allCfg = mCore->bandConfigs();
    auto& cfg = allCfg[band];
    if (freq) {
        cfg.freq = freq;
    }
    if (Q) {
        cfg.Q = Q;
    }
    ESP_LOGI(TAG, "Reconfiguring band %d: freq=%d, Q=%f", band, cfg.freq, (float)cfg.Q / 1000);
    mCore->updateFilter(band, true);
    MY_ESP_ERRCHECK(mNvsHandle.writeBlob(eqConfigKey(), allCfg, nBands * sizeof(EqBandConfig)),
        TAG, "writing band config", return false);
    return true;
}
bool EqualizerNode::setAllPeakingQ(int Q, bool clearState)
{
    LOCK_EQ();
    if (mCore->type() != IEqualizerCore::kTypeCustom) {
        ESP_LOGW(TAG, "%s: Negative or zero Q value", __FUNCTION__);
        return false;
    }
    if (Q <= 0) {
        ESP_LOGW(TAG, "%s: Negative or zero Q value", __FUNCTION__);
        return false;
    }
    int last = mCore->numBands() - 1;
    auto cfgs = mCore->bandConfigs();
    for (int i = 1; i < last; i++) {
        cfgs[i].Q = Q;
        mCore->updateFilter(i, clearState);
    }
    return true;
}
bool EqualizerNode::switchPreset(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    int len = 0;
    for (const char* ch = name; *ch; ch++) {
        if (*ch == '!') {
            ESP_LOGW(TAG, "%s: Invalid char in preset name '%s'", __FUNCTION__, name);
            return false;
        }
        len++;
    }
    if (len > 10) {
        ESP_LOGW(TAG, "%s: Preset name is too long", __FUNCTION__);
        return false;
    }
    LOCK_EQ();
    mEqId = " :";
    mEqId += name;
    deleteCore();
    equalizerReinit(0, true);
    return true;
}
void EqualizerNode::useEspEqualizer(bool use)
{
    LOCK_EQ();
    if (mUseEspEq == use) {
        return;
    }
    mUseEspEq = use;
    mCoreTypeChanged = true;
    mNvsHandle.write("eq.useEsp", (uint8_t)mUseEspEq);
    updateDefaultEqName();
    equalizerReinit(0, true);
}

template<bool Enable, typename T, int Shift>
struct VolumeProbe;

template<typename T, int Shift>
struct VolumeProbe<false, T, Shift> {
    void leftSample(T val) {}
    void rightSample(T val) {}
    void getPeakLevels(IAudioVolume::StereoLevels& output) {}
};
template<typename T, int Shift>
struct VolumeProbe<true, T, Shift> {
    T leftPeak = 0;
    T rightPeak = 0;
    void leftSample(T val) {
        if (val > leftPeak) {
            leftPeak = val;
        }
    }
    void rightSample(T val) {
        if (val > rightPeak) {
            rightPeak = val;
        }
    }
    template<bool W=(sizeof(T) > 2)>
    std::enable_if_t<W, void> getPeakLevels(IAudioVolume::StereoLevels& output) {
        output.left = leftPeak >> Shift;
        output.right = rightPeak >> Shift;
    }
    template<bool W=(sizeof(T) <= 2), int Sh=Shift>
    std::enable_if_t<W && Sh == 0, void> getPeakLevels(IAudioVolume::StereoLevels& output) {
        output.left = leftPeak;
        output.right = rightPeak;
    }
    template<bool W=(sizeof(T) <= 2), int Sh=Shift>
    std::enable_if_t<W && Sh != 0, void> getPeakLevels(IAudioVolume::StereoLevels& output) {
        output.left = leftPeak << Shift;
        output.right = rightPeak << Shift;
    }
};

template <int Bps>
float toFloat24(int32_t in) { return in; }

template<>
float toFloat24<32>(int32_t in) { return (in >= 0 ? (in + 128) : (in - 128)) >> 8; }

template <int Bps, bool VolProbeEnable>
void EqualizerNode::preConvert24or32ToFloatAndApplyVolume(DataPacket& pkt)
{
    static_assert(Bps == 24 || Bps == 32, "Invalid bps parameter");
    myassert(mInFormat.bitsPerSample() >= 24);
    VolumeProbe<VolProbeEnable, int32_t, Bps - 16> volProbe;
    auto rptr = (int32_t*)pkt.data;
    auto rend = (int32_t*)(pkt.data + pkt.dataLen);
    auto wptr = (float*)dspBufGetWritable(pkt.dataLen);
    while(rptr < rend) {
        int32_t val = *rptr++;
        *(wptr++) = toFloat24<Bps>(val) * mFloatVolumeMul;
        volProbe.leftSample(val);
        val = *rptr++;
        *(wptr++) = toFloat24<Bps>(val) * mFloatVolumeMul;
        volProbe.rightSample(val);
    }
    volProbe.getPeakLevels(mAudioLevels);
}
template<typename S, bool VolProbeEnable>
void EqualizerNode::preConvert16or8ToFloatAndApplyVolume(DataPacket& pkt)
{
    enum { kSampleSizeMul = 4 / sizeof(S), kShift = 24 - sizeof(S) * 8 };
    myassert(mInFormat.bitsPerSample() == sizeof(S) * 8);
    auto wptr = (float*)dspBufGetWritable(pkt.dataLen * kSampleSizeMul);
    auto rptr = (const S*)pkt.data;
    auto rend = (const S*)(pkt.data + pkt.dataLen);
    VolumeProbe<VolProbeEnable, S, 2 - sizeof(S)> volProbe;
    while(rptr < rend) {
        S val = *(rptr++);
        *(wptr++) = ((float)(val << kShift)) * mFloatVolumeMul;
        volProbe.leftSample(val);
        val = *(rptr++);
        *(wptr++) = ((float)(val << kShift)) * mFloatVolumeMul;
        volProbe.rightSample(val);
    }
    volProbe.getPeakLevels(mAudioLevels);
}
template<int Bits>
static inline int32_t floatToInt(float f)
{
    // 24-bit signed integer limits
    constexpr int32_t kMin = -(1 << (Bits-1));
    constexpr int32_t kMax =  (1 << (Bits-1)) - 1;
    int32_t i = static_cast<int32_t>(lroundf(f));
    if (i > kMax) {
        return kMax;
    }
    else if (i < kMin) {
        return kMin;
    }
    else {
        return i;
    }
}
template<bool VolProbeEnabled>
void EqualizerNode::postConvertFloatTo24(PacketResult& pr)
{
    auto rptr = (const float*)mDspBuffer.get();
    auto rend = (const float*)(mDspBuffer.get() + mDspDataSize);
    if (!(pr.packet && (pr.packet->flags & StreamPacket::kFlagHasSpaceFor32Bit))) {
        pr.packet.reset(DataPacket::create(mDspDataSize));
        pr.packet->flags |= DataPacket::kFlagHasSpaceFor32Bit;
    }
    auto& pkt = pr.dataPacket();
    pkt.dataLen = mDspDataSize;
    auto wptr = (int32_t*)pkt.data;
    VolumeProbe<VolProbeEnabled, int32_t, 16> volProbe;
    while (rptr < rend) {
        int32_t ival = *(wptr++) = floatToInt<24>(*(rptr++)) << 8; // I2S requires samples to be left-aligned
        volProbe.leftSample(ival);
        ival = *(wptr++) = floatToInt<24>(*(rptr++)) << 8;
        volProbe.rightSample(ival);
    }
    volProbe.getPeakLevels(mAudioLevels);
}
template<bool VolProbeEnabled>
void EqualizerNode::postConvertFloatTo16(PacketResult& pr)
{
    auto rptr = (float*)mDspBuffer.get();
    auto rend = (float*)(mDspBuffer.get() + mDspDataSize);
    int outSize = mDspDataSize >> 1;
    auto pkt = (DataPacket*)pr.packet.get();
    if (!(pkt && pkt->bufSize >= outSize)) {
        pkt = DataPacket::create(outSize);
        pr.packet.reset(pkt);
    }
    else {
        pkt->dataLen = outSize;
    }
    auto wptr = (int16_t*)pkt->data;
    VolumeProbe<VolProbeEnabled, int16_t, 0> volProbe;
    while(rptr < rend) {
        int16_t ival = *wptr++ = floatToInt<24>(*(rptr++) + 128.5555f) >> 8;
        volProbe.leftSample(ival);
        ival = *wptr++ = floatToInt<24>(*(rptr++) + 128.5555f) >> 8;
        volProbe.rightSample(ival);
    }
    volProbe.getPeakLevels(mAudioLevels);
}
template<int Bps, bool VolProbeEnabled>
void EqualizerNode::preConvert24or32To16AndApplyVolume(DataPacket& pkt)
{
    // samples are in 32bit words
    static_assert(Bps > 16, "");
    // shift right to both decrease bps and volume-divide
    enum { kShift = Bps - 16 + kVolumeDivShift, kHalfDiv = 1 << (kShift-1) };
    int32_t* rptr = (int32_t*)pkt.data;
    int32_t* rend = (int32_t*)(pkt.data + pkt.dataLen);
    auto wptr = (int16_t*)dspBufGetWritable(pkt.dataLen >> 1);
    VolumeProbe<VolProbeEnabled, int32_t, 32-Bps> volProbe;
    while(rptr < rend) {
        auto val = *(rptr++);
        volProbe.leftSample(val);
        *wptr++ = (val * mVolume + ((val >= 0) ? kHalfDiv : -kHalfDiv)) >> kShift;
        val = *(rptr++);
        volProbe.rightSample(val);
        *wptr++ = (val * mVolume + ((val >= 0) ? kHalfDiv : -kHalfDiv)) >> kShift;
    }
    volProbe.getPeakLevels(mAudioLevels);
}
template <typename T, bool VolProbeEnable>
void EqualizerNode::preConvert8or16to16AndApplyVolume(DataPacket& pkt)
{
    // we do two shifts at once - one is for the volume multiply/divide, and the other one
    // is to left-align the sample, as is required by i2s
    static_assert(sizeof(T) <= 2);
    enum { kShift = (sizeof(T) == 1) ? 0 : 8 };
    enum { kSizeMult = 2 / sizeof(T) };
    auto rptr = (T*)pkt.data;
    auto rend = (T*)(pkt.data + pkt.dataLen);
    auto wptr = (int16_t*)dspBufGetWritable(pkt.dataLen * kSizeMult);
    VolumeProbe<VolProbeEnable, T, 16 - sizeof(T) * 8> volProbe;
    while(rptr < rend) {
        T val = *rptr++;
        int32_t unaligned = (static_cast<int32_t>(val) * mVolume + kVolumeDiv / 2);
        *wptr++ = (kShift != 0) ? (unaligned >> kShift) : unaligned;
        volProbe.leftSample(val);
        val = *rptr++;
        unaligned = (static_cast<int32_t>(val) * mVolume + kVolumeDiv / 2);
        *wptr++ = (kShift != 0) ? (unaligned >> kShift) : unaligned;
        volProbe.rightSample(val);
    }
    pkt.flags |= StreamPacket::kFlagLeftAlignedSamples;
    volProbe.getPeakLevels(mAudioLevels);
}
template <bool VolProbeEnabled>
void EqualizerNode::postConvert16To24(PacketResult& pr)
{
    DataPacket* pkt = (DataPacket*)pr.packet.get();
    if (!(pkt && (pkt->flags & StreamPacket::kFlagHasSpaceFor32Bit))) {
        pkt = DataPacket::create(mDspDataSize * 2);
        pr.packet.reset(pkt);
    }
    else {
        pkt->dataLen = mDspDataSize * 2;
    }
    auto rptr = (int16_t*)mDspBuffer.get();
    auto rend = (int16_t*)(mDspBuffer.get() + mDspDataSize);
    auto wptr = (int32_t*)pkt->data;
    VolumeProbe<VolProbeEnabled, int16_t, 0> volProbe;
    while(rptr < rend) {
        int16_t val = *rptr++;
        *wptr++ = val << 16; // i2s requires samples to be left-aligned
        volProbe.leftSample(val);
        val = *rptr++;
        *wptr++ = val << 16;
        volProbe.rightSample(val);
    }
    volProbe.getPeakLevels(mAudioLevels);
}
template <bool VolProbeEnabled>
void EqualizerNode::postConvert16To16(PacketResult& pr)
{
    auto pkt = (DataPacket*)pr.packet.get();
    if (!(pkt && pkt->bufSize >= mDspDataSize)) {
        pkt = DataPacket::create(mDspDataSize);
        pr.packet.reset(pkt);
    }
    else {
        pkt->dataLen = mDspDataSize;
    }
    if (VolProbeEnabled) {
        auto rptr = (int16_t*)mDspBuffer.get();
        auto rend = (int16_t*)(mDspBuffer.get() + mDspDataSize);
        auto wptr = (int16_t*)pkt->data;
        VolumeProbe<VolProbeEnabled, int16_t, 0> volProbe;
        while(rptr < rend) {
            int16_t val = *rptr++;
            *wptr++ = val;
            volProbe.leftSample(val);
            val = *rptr++;
            *wptr++ = val;
            volProbe.rightSample(val);
        }
        volProbe.getPeakLevels(mAudioLevels);
    }
    else {
        memcpy(pkt->data, mDspBuffer.get(), mDspDataSize);
    }
}
StreamEvent EqualizerNode::pullData(PacketResult& dpr)
{
    auto event = mPrev->pullData(dpr);
    if (!event) {
        MutexLocker locker(mMutex);
        if (mCoreTypeChanged) {
            mCoreTypeChanged = false;
            // we have a race condition if we handle mCoreTypeChanged before pullData, because we are unlocked
            // during pullData and someone may set mCoreTypeChanged. In that case we can't return both the
            // kEvtStreamChanged and the data packet
            return dpr.set(new NewStreamEvent(mStreamId, mOutFormat, mSourceBps));
        }
#ifdef CONVERT_PERF
        ElapsedTimer t;
#endif
        (this->*mPreConvertFunc)(dpr.dataPacket());
#ifdef CONVERT_PERF
        ESP_LOGI(TAG, "preconvert: %lld us", t.usElapsed());
#endif
        if (!mBypass) {
            mCore->process(mDspBuffer.get(), mDspDataSize);
        }
        (this->*mPostConvertFunc)(dpr);
        volumeNotifyLevelCallback();
        return kEvtData;
    }
    else if (event == kEvtStreamChanged) {
        auto& pkt = dpr.newStreamEvent();
        MutexLocker locker(mMutex);
        auto& fmt = pkt.fmt;
        mStreamId = pkt.streamId;
        mSourceBps = pkt.sourceBps;
        asyncCallWait([&]() { equalizerReinit(fmt); });
        fmt = mOutFormat;
        return event;
    }
    else {
        return event;
    }

    return kNoError;
}
