#include "equalizerNode.hpp"
#include <nvsHandle.hpp>
#include <esp_equalizer.h>
#include <cmath>
#include "asyncCall.hpp"

//#define EQ_PERF 1
//#define CONVERT_PERF 1

static const char* TAG = "eq";

template <int N, bool Stereo>
MyEqualizerCore<N, Stereo>::MyEqualizerCore(const EqBandConfig* cfg)
: mEqualizer(cfg)
{
    ESP_LOGI("eq", "Created %d-band custom %s equalizer", N, Stereo ? "stereo" : "mono");
}

EqualizerNode::EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs)
: AudioNode(parent, "equalizer"), mNvsHandle(nvs)
{
    mMyEqDefaultNumBands = mNvsHandle.readDefault("eq.nbands", (uint8_t)kMyEqDefaultNumBands);
    if (mMyEqDefaultNumBands < kMyEqMinBands || mMyEqDefaultNumBands > kMyEqMaxBands) {
        mMyEqDefaultNumBands = kMyEqDefaultNumBands;
    }
    mUseEspEq = mNvsHandle.readDefault("eq.useStock", (uint8_t)1);
    mOut24bit = mNvsHandle.readDefault("eq.out24bit", (uint8_t)0);
    size_t len = sizeof(mEqName);
    if (mNvsHandle.readString("eq.default", mEqName, len) == ESP_OK) {
        mEqName[len] = 0;
    } else {
        strcpy(mEqName, "default");
    }
    equalizerReinit(StreamFormat(44100, 16, 2));
}
std::string EqualizerNode::eqNameKey() const
{
    std::string result = "eq.";
    result += mEqName;
    if (mCore->type() == IEqualizerCore::kTypeEsp) {
        result += '0';
    } else {
        appendAny(result, mCore->numBands());
    }
    return result;
}
std::string EqualizerNode::eqConfigKey(uint8_t nBands) const {
    std::string result = "eq.";
    result += mEqName;
    appendAny(result, nBands);
    result += ".cfg";
    return result;
}
void EqualizerNode::loadEqConfig(uint8_t nBands)
{
    mBandConfigs.reset(new EqBandConfig[nBands]);
    size_t len = nBands * sizeof(EqBandConfig);
    if (mNvsHandle.readBlob(eqConfigKey(nBands).c_str(), mBandConfigs.get(), len) != ESP_OK || len != nBands * sizeof(EqBandConfig)) {
        mBandConfigs.reset();
    } else {
        ESP_LOGW(TAG, "Loaded custom band-config for %d-band equalizer '%s'", nBands, mEqName);
    }
}
#define CASE_N_BANDS(N, Stereo) \
    case N: mCore.reset(new MyEqualizerCore<N, Stereo>(mBandConfigs.get())); break

#define SWITCH_CASES(Stereo)      \
    switch (nBands) {             \
        CASE_N_BANDS(10, Stereo); \
        CASE_N_BANDS(9, Stereo);  \
        CASE_N_BANDS(8, Stereo);  \
        CASE_N_BANDS(7, Stereo);  \
        CASE_N_BANDS(6, Stereo);  \
        CASE_N_BANDS(5, Stereo);  \
        CASE_N_BANDS(4, Stereo);  \
        CASE_N_BANDS(3, Stereo);  \
        default:                  \
            ESP_LOGW(TAG, "createCustomCore: Unsupported number of bands: %d", nBands); \
            assert(false);                                                              \
    }

void EqualizerNode::createCustomCore(uint8_t nBands, StreamFormat fmt)
{
    loadEqConfig(nBands);
    if (fmt.numChannels() >= 2) {
        SWITCH_CASES(true);
    }
    else {
        SWITCH_CASES(false);
    }
    mGains.reset(new int8_t[nBands]);
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
            mCore.reset(new EspEqualizerCore);
            mGains.reset(new int8_t[10]);
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
        bool isDefault = (strcmp(mEqName, "default") == 0);
        if (fmtChanged || !mCore || mCore->type() != IEqualizerCore::kTypeCustom ||
          (isDefault && (mCore->numBands() != mMyEqDefaultNumBands))) {
            size_t len = 0;
            uint8_t nBands = (!isDefault &&
                (mNvsHandle.readBlob(eqNameKey().c_str(), nullptr, len) == ESP_OK) &&
                (len >= kMyEqMinBands && len <= kMyEqMaxBands))
            ? len : mMyEqDefaultNumBands;
            createCustomCore(nBands, mInFormat);
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
    if (justCreated || forceLoadGains) {
        auto nBands = mCore->numBands();
        size_t len = nBands;
        if ((mNvsHandle.readBlob(eqNameKey().c_str(), mGains.get(), len) == ESP_OK) && (len == nBands)) {
            ESP_LOGI(TAG, "Loaded equalizer gains from NVS for '%s'", mEqName);
        } else {
            ESP_LOGI(TAG, "Error loading gains for '%s', len=%d, nbands=%d", eqNameKey().c_str(), len, nBands);
            memset(mGains.get(), 0, nBands);
        }
    }
    mCore->init(mInFormat, mGains.get());
    mProcessFunc = mCore->getProcessFunc(mInFormat);
}
bool EqualizerNode::setMyEqNumBands(uint8_t n) {
    if (n < kMyEqMinBands || n > kMyEqMaxBands) {
        return false;
    }
    MutexLocker locker(mMutex);
    mMyEqDefaultNumBands = n;
    equalizerReinit();
    mNvsHandle.write("eq.nbands", (uint8_t)n);
    return true;
}
bool EqualizerNode::setBandGain(uint8_t band, int8_t dbGain)
{
    MutexLocker locker(mMutex);
    if (!mCore) {
        return false;
    }
    if (band > mCore->numBands()) {
        return false;
    }
    mGains[band] = dbGain;
    mCore->setBandGain(band, dbGain);
    return true;
}

void EqualizerNode::setAllGains(const int8_t* gains, int len)
{
    MutexLocker locker(mMutex);
    auto nBands = mCore->numBands();
    if (gains) {
        assert(len == nBands);
        memcpy(mGains.get(), gains, nBands);
    } else {
        memset(mGains.get(), 0, nBands);
    }
    mCore->setAllGains(mGains.get());
}
bool EqualizerNode::saveGains()
{
    MutexLocker locker(mMutex);
    return mNvsHandle.writeBlob(eqNameKey().c_str(), mGains.get(), mCore->numBands()) == ESP_OK;
}
bool EqualizerNode::reconfigEqBand(uint8_t band, uint16_t freq, int8_t bw)
{
    MutexLocker locker(mMutex);
    if (!mCore || mCore->type() != IEqualizerCore::kTypeCustom) {
        return false;
    }
    uint8_t nBands = mCore->numBands();
    auto len = nBands * sizeof(EqBandConfig);
    EqBandConfig* cfg;
    if (mBandConfigs) {
        cfg = mBandConfigs.get();
    } else {
        cfg = (EqBandConfig*)alloca(len);
        memcpy(cfg, EqBandConfig::defaultForNBands(nBands), len);
    }
    auto bandCfg = cfg + band;
    if (freq) {
        bandCfg->freq = freq;
    }
    if (bw) {
        bandCfg->width = bw;
    }
    ESP_LOGI(TAG, "Reconfiguring band %d: freq=%d, Q=%f", band, bandCfg->freq, (float)bandCfg->width / 10);
    if (mNvsHandle.writeBlob(eqConfigKey(nBands).c_str(), cfg, len) == ESP_OK) {
        mCore.reset();
        equalizerReinit();
        return true;
    }
    return false;
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
    mNvsHandle.write("eq.useStock", (uint8_t)mUseEspEq);
    equalizerReinit(0, true);
    mCoreChanged = true;
}

template<int N, bool Stereo>
void MyEqualizerCore<N, Stereo>::processFloat(DataPacket& pkt, void* arg) {
    auto& self = *static_cast<MyEqualizerCore<N, Stereo>*>(arg);
#ifdef EQ_PERF
    static float msAvg = 0;
    ElapsedTimer timer;
#endif
    self.mEqualizer.process((float*)pkt.data, pkt.dataLen / ((Stereo ? 2 : 1) * sizeof(float)));
#ifdef EQ_PERF
    auto ms = timer.msElapsed();
    msAvg = (msAvg * 99 + ms) / 100;
    ESP_LOGI(TAG, "eq process(my) %d: %d (%.2f) ms", pkt.dataLen / 8, ms, msAvg);
#endif
}
template<int N, bool Stereo>
IEqualizerCore::ProcessFunc MyEqualizerCore<N, Stereo>::getProcessFunc(StreamFormat fmt)
{
    return processFloat;
}

EspEqualizerCore::EspEqualizerCore()
{
    ESP_LOGI("eq", "Created ESP equalizer core");
}
void EspEqualizerCore::init(StreamFormat fmt, int8_t* gains)
{
    mSampleRate = fmt.sampleRate();
    mChanCount = fmt.numChannels();
    // if mEqualizer already exists, it should be auto-reconfigured by process(),
    // because it takes the sample rate and chan count as parameters
    if (!mEqualizer) {
        mEqualizer = esp_equalizer_init(mChanCount, mSampleRate, 10, 0);
    }
    setAllGains(gains);
}
void EspEqualizerCore::setBandGain(uint8_t band, int8_t dbGain)
{
    if (!mEqualizer || band > 9) {
        return;
    }
    esp_equalizer_set_band_value(mEqualizer, dbGain, band, 0);
    esp_equalizer_set_band_value(mEqualizer, dbGain, band, 1);
}
void EspEqualizerCore::setAllGains(const int8_t *gains)
{
    if (!mEqualizer) {
        return;
    }
    for (uint8_t i = 0; i < 10; i++) {
        esp_equalizer_set_band_value(mEqualizer, gains[i], i, 0);
        esp_equalizer_set_band_value(mEqualizer, gains[i], i, 1);
    }
}
EqBandConfig EspEqualizerCore::bandConfig(uint8_t n) const
{
    static const uint16_t bandFreqs[10] = {
        31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
    };
    return {bandFreqs[n], 3};
}
void EspEqualizerCore::process16bitStereo(DataPacket& pkt, void *arg)
{
    auto& self = *static_cast<EspEqualizerCore*>(arg);
#ifdef EQ_PERF
    static float avgTime = 0.0;
    ElapsedTimer timer;
#endif
    esp_equalizer_process(self.mEqualizer, (unsigned char*)pkt.data, pkt.dataLen,
                          self.mSampleRate, self.mChanCount);
#ifdef EQ_PERF
    auto msTime = timer.msElapsed();
    avgTime = (avgTime * 99 + msTime) / 100;
    ESP_LOGI(TAG, "eq process %d (esp): %dms (%.2f)", pkt.dataLen / 4, msTime, avgTime);
#endif
}
EspEqualizerCore::~EspEqualizerCore()
{
    if (mEqualizer) {
        esp_equalizer_uninit(mEqualizer);
    }
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
