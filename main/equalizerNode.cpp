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
void EqualizerNode::equalizerReinit(StreamFormat fmt, bool forceLoadGains)
{
    ESP_LOGI(TAG, "Equalizer reinit");
    bool fmtChanged = fmt.asNumCode() && (fmt != mInFormat);
    if (fmtChanged) {
        mOutFormat = mInFormat = fmt;
    }
    if (mUseEspEq && mInFormat.sampleRate() <= 48000) {
        mOutFormat.setBitsPerSample(16);
        if (fmtChanged || !mCore || mCore->type() != IEqualizerCore::kTypeEsp) {
            mCore.reset(new EspEqualizerCore(mOutFormat));
            forceLoadGains = true;
            auto bps = mInFormat.bitsPerSample();
            if (bps <= 16) {
                mPreConvertFunc = nullptr; // no conversion and use DefaultVolumeImpl for volume
                volumeUpdateFormat(mOutFormat);
            }
            else if (bps == 24) {
                mPreConvertFunc = &EqualizerNode::samplesTo16bitAndApplyVolume<24>;
            }
            else if (bps == 32) {
                mPreConvertFunc = &EqualizerNode::samplesTo16bitAndApplyVolume<32>;
            }
            else {
                myassert(false);
            }
        }
    }
    else { // need custom eq
        mOutFormat.setBitsPerSample(mOut24bit ? 24 : 16);
        if (fmtChanged || !mCore || mCore->type() != IEqualizerCore::kTypeCustom) {
            createCustomCore(mInFormat);
            forceLoadGains = true;
            auto bps = mInFormat.bitsPerSample();
            if (bps == 16) {
                mPreConvertFunc = &EqualizerNode::samples16or8ToFloatAndApplyVolume<int16_t>;
            }
            else if (bps == 24) {
                mPreConvertFunc = &EqualizerNode::samples24or32ToFloatAndApplyVolume<24>;
            }
            else if (bps == 8) {
                mPreConvertFunc = &EqualizerNode::samples16or8ToFloatAndApplyVolume<int8_t>;
            }
            else if (bps == 32) {
                mPreConvertFunc = &EqualizerNode::samples24or32ToFloatAndApplyVolume<32>;
            }
            else {
                ESP_LOGE(TAG, "Unsupported bits per sample: %d", bps);
                assert(false);
            }
            mPostConvertFunc = mOut24bit
                ? &EqualizerNode::floatSamplesTo24bitAndGetLevelsStereo
                : &EqualizerNode::floatSamplesTo16bitAndGetLevelsStereo;
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
    mProcessFunc = mCore->getProcessFunc();
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

template <int Bps>
float toFloat24(int32_t in) { return in; }

template<>
float toFloat24<32>(int32_t in) { return (in >= 0 ? (in + 128) : (in - 128)) >> 8; }

template <int Bps>
void EqualizerNode::samples24or32ToFloatAndApplyVolume(PacketResult& pr)
{
    static_assert(Bps == 24 || Bps == 32, "Invalid bps parameter");
    auto& pkt = pr.dataPacket();
    myassert(mInFormat.bitsPerSample() >= 24);
    int32_t* end = (int32_t*)(pkt.data + pkt.dataLen);
    for (int32_t* sptr = (int32_t*)pkt.data; sptr < end; sptr++) {
        *((float*)sptr) = toFloat24<Bps>(*sptr) * mFloatVolumeMul;
    }
}
template<typename S>
void EqualizerNode::samples16or8ToFloatAndApplyVolume(PacketResult& pr)
{
    enum { kSampleSizeMul = 4 / sizeof(S), kShift = 24 - sizeof(S) * 8 };
    myassert(mInFormat.bitsPerSample() == sizeof(S) * 8);

    if (!(pr.packet->flags & StreamPacket::kFlagHasSpaceFor32Bit)) {
        // no space in input packet, create new packet to fit the float samples
        DataPacket::unique_ptr inPkt((DataPacket*)pr.packet.release());
        pr.packet.reset(DataPacket::create(inPkt->dataLen * kSampleSizeMul));
        auto& pkt = pr.dataPacket();
        float* wptr = (float*)pkt.data;
        const S* rptr = (S*)inPkt->data;
        S* end = (S*)(inPkt->data + inPkt->dataLen);
        while(rptr < end) {
            *(wptr++) = ((float)(*(rptr++) << kShift)) * mFloatVolumeMul;
        }
    }
    else { // in-place conversion
        auto& pkt = pr.dataPacket();
        float* wptr = (float*)(pkt.data + pkt.dataLen * kSampleSizeMul);
        const S* rptr = (S*)(pkt.data + pkt.dataLen);
        S* start = (S*)pkt.data;
        while(rptr > start) {
            *(--wptr) = ((float)(*(--rptr) << kShift)) * mFloatVolumeMul;
        }
        myassert(rptr == start);
        myassert(wptr == (float*)start);
        pkt.dataLen *= kSampleSizeMul;
    }
}
void EqualizerNode::floatSamplesTo24bitAndGetLevelsStereo(DataPacket& pkt) {
    float* end = (float*)(pkt.data + pkt.dataLen);
    int32_t leftPeak = 0;
    int32_t rightPeak = 0;
    for (float* sptr = (float*)pkt.data; sptr < end;) {
        int32_t ival = *((int32_t*)sptr) = lroundf(*sptr) << 8; // I2S requires samples to be left-aligned
        if (ival > leftPeak) {
            leftPeak = ival;
        }
        sptr++;
        ival = *((int32_t*)sptr) = lroundf(*sptr) << 8;
        if (ival > rightPeak) {
            rightPeak = ival;
        }
        sptr++;
    }
    mAudioLevels.left = leftPeak >> 16;
    mAudioLevels.right = rightPeak >> 16;
}
void EqualizerNode::floatSamplesTo16bitAndGetLevelsStereo(DataPacket& pkt)
{
    float* rend = (float*)(pkt.data + pkt.dataLen);
    int16_t leftPeak = 0;
    int16_t rightPeak = 0;
    float* rptr = (float*)pkt.data;
    int16_t* wptr = (int16_t*)pkt.data;
    while(rptr < rend) {
        int16_t ival = *wptr++ = ((int32_t)(*rptr++ + 128.5555f)) >> 8;
        if (ival > leftPeak) {
            leftPeak = ival;
        }
        ival = *wptr++ = ((int32_t)(*rptr++ + 128.5555f)) >> 8;
        if (ival > rightPeak) {
            rightPeak = ival;
        }
    }
    pkt.dataLen >>= 1;
    mAudioLevels.left = leftPeak;
    mAudioLevels.right = rightPeak;
}

template<int Bps>
void EqualizerNode::samplesTo16bitAndApplyVolume(PacketResult& pr)
{
    auto& pkt = pr.dataPacket();
    // samples are in 32bit words
    static_assert(Bps > 16, "");
    // shift right to both decrease bps and volume-divide
    enum { kShift = Bps - 16 + kVolumeDivShift, kHalfDiv = 1 << (kShift-1) };
    int32_t* end = (int32_t*)(pkt.data + pkt.dataLen);
    int16_t* wptr = (int16_t*)pkt.data;
    for(int32_t* rptr = (int32_t*)pkt.data; rptr < end; rptr++) {
        auto val = *rptr;
        *wptr++ = (val * mVolume + ((val >= 0) ? kHalfDiv : -kHalfDiv)) >> kShift;
    }
    pkt.dataLen >>= 1;
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
        if (mCore->type() == IEqualizerCore::kTypeEsp) {
            if (mInFormat.bitsPerSample() > 16) {
#ifdef CONVERT_PERF
                ElapsedTimer t;
#endif
                (this->*mPreConvertFunc)(dpr);
#ifdef CONVERT_PERF
                ESP_LOGI(TAG, "preconvert: %lld us", t.usElapsed());
#endif
            }
            else {
                volumeProcess(dpr.dataPacket());
            }
            if (!mBypass) {
                mProcessFunc(dpr.dataPacket(), mCore.get());
            }
            if (mVolLevelMeasurePoint == 1) {
                volumeGetLevel(dpr.dataPacket());
            }
        }
        else {
#ifdef CONVERT_PERF
            ElapsedTimer t;
#endif
            (this->*mPreConvertFunc)(dpr);
#ifdef CONVERT_PERF
            ESP_LOGI(TAG, "preconvert: %lld us", t.usElapsed());
#endif
            if (!mBypass) {
                mProcessFunc(dpr.dataPacket(), mCore.get());
            }
            (this->*mPostConvertFunc)(dpr.dataPacket());
        }
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
