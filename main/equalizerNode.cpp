#include "equalizerNode.hpp"
#include "eqCores.hpp"
#include <nvsHandle.hpp>
#include <esp_equalizer.h>
#include <cmath>
#include "asyncCall.hpp"

//#define EQ_PERF 1
//#define CONVERT_PERF 1

static const char* TAG = "eq";

EqualizerNode::EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs)
: AudioNode(parent, "equalizer"), mNvsHandle(nvs)
{
    mDefaultNumBands = mNvsHandle.readDefault("eq.nbands", (uint8_t)kDefaultNumBands);
    if (mDefaultNumBands < kMyEqMinBands || mDefaultNumBands > kMyEqMaxBands) {
        mDefaultNumBands = kDefaultNumBands;
    }
    mUseEspEq = mNvsHandle.readDefault("eq.useEsp", (uint8_t)1);
    mOut24bit = mNvsHandle.readDefault("eq.out24bit", (uint8_t)1);
    size_t len = sizeof(mEqName);
    if (mNvsHandle.readString("eq.default", mEqName, len) == ESP_OK) {
        mEqName[len] = 0;
    }
    else {
        mEqName[0] = 0;
    }
    equalizerReinit(StreamFormat(44100, 16, 2));
}
std::string EqualizerNode::eqName() const
{
    if (mEqName[0]) {
        return mEqName;
    }
    else {
        std::string ret = "default";
        return appendAny(ret, mDefaultNumBands);
    }
}
uint8_t EqualizerNode::eqNumBands()
{
    if (!mEqName[0]) {
        return mDefaultNumBands;
    }
    std::string key = mEqName;
    key += ".cfg";
    int len = mNvsHandle.getBlobSize(key.c_str());
    uint8_t nBands;
    if ((len >= 0) && (len % sizeof(EqBandConfig) == 0) && (nBands = len / sizeof(EqBandConfig) >= kMyEqMinBands)) {
        return nBands;
    }
    key[key.size() - 4] = 0; // remove .cfg
    len = mNvsHandle.getBlobSize(key.c_str());
    return (len < kMyEqMinBands || len > kMyEqMaxBands) ? mDefaultNumBands : len;
}
void EqualizerNode::createCustomCore(StreamFormat fmt)
{
    auto nBands = eqNumBands();
    if (fmt.numChannels() >= 2) {
        mCore.reset(MyEqualizerCore<true>::create(nBands, fmt.sampleRate()));
    }
    else {
        mCore.reset(MyEqualizerCore<false>::create(nBands, fmt.sampleRate()));
    }
    size_t expectedLen = nBands * sizeof(EqBandConfig);
    auto len = expectedLen;
    if (mNvsHandle.readBlob(eqConfigKey().c_str(), mCore->bandConfigs(), len) != ESP_OK || len != expectedLen) {
        memcpy(mCore->bandConfigs(), EqBandConfig::defaultCfg(nBands), expectedLen);
    }
    else {
        ESP_LOGW(TAG, "Loaded custom config for %d-band equalizer '%s'", nBands, eqName().c_str());
    }
}
void EqualizerNode::equalizerReinit(StreamFormat fmt, bool forceLoadGains)
{
    ESP_LOGI(TAG, "Equalizer reinit");
    bool fmtChanged = fmt.asNumCode() && (fmt != mInFormat);
    if (fmtChanged) {
        mOutFormat = mInFormat = fmt;
    }
    bool justCreated = false;
    if (mUseEspEq && mInFormat.sampleRate() <= 48000) {
        mOutFormat.setBitsPerSample(16);
        if (fmtChanged || !mCore || mCore->type() != IEqualizerCore::kTypeEsp) {
            mCore.reset(new EspEqualizerCore(mOutFormat));
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
            justCreated = true;
        }
    }
    else { // need custom eq
        mOutFormat.setBitsPerSample(mOut24bit ? 24 : 16);
        if (fmtChanged || !mCore || mCore->type() != IEqualizerCore::kTypeCustom ||
                (eqIsDefault() && (mCore->numBands() != mDefaultNumBands))) {
            createCustomCore(mInFormat);
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
            justCreated = true;
        }
    }
    // load gains
    if (justCreated || forceLoadGains) {
        auto nBands = mCore->numBands();
        size_t len = nBands;
        auto key = eqGainsKey();
        if ((mNvsHandle.readBlob(key.c_str(), mCore->gains(), len) == ESP_OK) && (len == nBands)) {
            ESP_LOGI(TAG, "Loaded equalizer gains from NVS for '%s'", key.c_str());
        }
        else {
            ESP_LOGI(TAG, "Error loading or no gains for '%s', len=%d, nbands=%d", eqGainsKey().c_str(), len, nBands);
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
    MutexLocker locker(mMutex);
    mDefaultNumBands = n;
    equalizerReinit();
    mNvsHandle.write("eq.nbands", (uint8_t)n);
    return true;
}
bool EqualizerNode::setBandGain(uint8_t band, int8_t dbGain)
{
    MutexLocker locker(mMutex);
    myassert(mCore);
    if (band > mCore->numBands()) {
        return false;
    }
    mCore->setBandGain(band, dbGain);
    return true;
}

void EqualizerNode::zeroAllGains()
{
    MutexLocker locker(mMutex);
    auto nBands = mCore->numBands();
    memset(mCore->gains(), 0, nBands);
    mCore->updateAllFilters();
}
bool EqualizerNode::saveGains()
{
    MutexLocker locker(mMutex);
    return mNvsHandle.writeBlob(eqGainsKey().c_str(), mCore->gains(), mCore->numBands()) == ESP_OK;
}
bool EqualizerNode::reconfigEqBand(uint8_t band, uint16_t freq, uint16_t Q)
{
    MutexLocker locker(mMutex);
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
    MY_ESP_ERRCHECK(mNvsHandle.writeBlob(eqConfigKey().c_str(), allCfg, nBands * sizeof(EqBandConfig)),
        TAG, "writing band config", return false);
    return true;
}
bool EqualizerNode::setAllPeakingQ(int Q, bool clearState)
{
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
    auto len = strlen(name);
    if (len >= sizeof(mEqName)) {
        ESP_LOGW(TAG, "switchPreset: Name is too long");
        return false;
    }
    memcpy(mEqName, name, len + 1);
    equalizerReinit(0, true);
    return true;
}
void EqualizerNode::useEspEqualizer(bool use)
{
    MutexLocker locker(mMutex);
    if (mUseEspEq == use) {
        return;
    }
    mUseEspEq = use;
    mNvsHandle.write("eq.useEsp", (uint8_t)mUseEspEq);
    equalizerReinit(0, true);
    mCoreChanged = true;
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
        if (mCoreChanged) {
            mCoreChanged = false;
            // we have a race condition if we handle mCoreChanged before pullData, because we are unlocked
            // during pullData and someone may set mCoreChanged. In that case we can't return both the
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
