#include "equalizerNode.hpp"
#include <nvsHandle.hpp>
#include <esp_equalizer.h>

static const char* TAG = "eq";

template <int N>
MyEqualizerCore<N>::MyEqualizerCore(const EqBandConfig* cfg)
: mEqualizerLeft(cfg), mEqualizerRight(cfg)
{}

EqualizerNode::EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs)
: AudioNode(parent, "equalizer"), mNvsHandle(nvs)
{
    mMyEqDefaultNumBands = mNvsHandle.readDefault("eq.nbands", (uint8_t)kMyEqDefaultNumBands);
    if (mMyEqDefaultNumBands < kMyEqMinBands || mMyEqDefaultNumBands > kMyEqMaxBands) {
        mMyEqDefaultNumBands = kMyEqDefaultNumBands;
    }
    mUseEspEq = mNvsHandle.readDefault("eq.useStock", 1);
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
#define CASE_N_BANDS(n) \
    case n: mCore.reset(new MyEqualizerCore<n>(nullptr)); break

void EqualizerNode::createCustomCore(uint8_t nBands, StreamFormat fmt)
{
    switch(nBands) {
        CASE_N_BANDS(10);
        CASE_N_BANDS(9);
        CASE_N_BANDS(8);
        CASE_N_BANDS(7);
        CASE_N_BANDS(6);
        CASE_N_BANDS(5);
        CASE_N_BANDS(4);
        default:
            ESP_LOGW(TAG, "createCustomCore: Unsupported number of bands: %d", nBands);
            assert(false);
    }
    mGains.reset(new int8_t[nBands]);
}
void EqualizerNode::equalizerReinit(StreamFormat fmt, bool forceLoadGains)
{
    ESP_LOGI(TAG, "Equalizer reinit");
    mFormat = fmt;
    bool justCreated = false;
    if (mUseEspEq && fmt.bitsPerSample() <= 16 && fmt.sampleRate() <= 48000) {
        if (!mCore || mCore->type() != IEqualizerCore::kTypeEsp) {
            mCore.reset(new EspEqualizerCore);
            mGains.reset(new int8_t[10]);
            justCreated = true;
        }
    }
    else { // need custom eq
        bool isDefault = (strcmp(mEqName, "default") == 0);
        if (!mCore || mCore->type() != IEqualizerCore::kTypeCustom ||
          (isDefault && (mCore->numBands() != mMyEqDefaultNumBands))) {
            size_t len = 0;
            uint8_t nBands = (!isDefault &&
                (mNvsHandle.readBlob(eqNameKey().c_str(), nullptr, len) == ESP_OK) &&
                (len >= kMyEqMinBands && len <= kMyEqMaxBands))
            ? len : mMyEqDefaultNumBands;
            createCustomCore(nBands, fmt);
            justCreated = true;
        }
    }
    if (justCreated || forceLoadGains) {
        auto nBands = mCore->numBands();
        size_t len = nBands;
        if ((mNvsHandle.readBlob(eqNameKey().c_str(), mGains.get(), len) == ESP_OK) && (len == nBands)) {
            ESP_LOGI(TAG, "Loaded equalizer gains from NVS for '%s'", mEqName);
        } else {
            memset(mGains.get(), 0, nBands);
        }
    }
    mCore->init(fmt, mGains.get());
    mProcessFunc = mCore->getProcessFunc(fmt, mProcFuncArg);
}
bool EqualizerNode::setMyEqNumBands(uint8_t n) {
    if (n < kMyEqMinBands || n > kMyEqMaxBands) {
        return false;
    }
    MutexLocker locker(mMutex);
    mMyEqDefaultNumBands = n;
    equalizerReinit(mFormat);
    mNvsHandle.write("eq.nbands", (uint8_t)n);
    return true;
}
bool EqualizerNode::setBandGain(uint8_t band, int8_t dbGain)
{
    MutexLocker locker(mMutex);
    assert(mCore);
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
bool EqualizerNode::switchPreset(const char *name)
{
    auto len = strlen(name);
    if (len >= sizeof(mEqName)) {
        ESP_LOGW(TAG, "switchPreset: Name is too long");
        return false;
    }
    memcpy(mEqName, name, len + 1);
    equalizerReinit(mFormat, true);
    return true;
}
void EqualizerNode::useEspEqualizer(bool use)
{
    MutexLocker locker(mMutex);
    if (mUseEspEq == use) {
        return;
    }
    mUseEspEq = use;
    equalizerReinit(mFormat, true);
}

template<int N>
MyEqualizerCore<N>::MyEqualizerCore()
{
    ESP_LOGI("eq", "Created %d-band custom equalizer", N);
}
template<int N>
void MyEqualizerCore<N>::process16bitStereo(AudioNode::DataPullReq& dpr, void* arg) {
    auto& self = *static_cast<MyEqualizerCore<N>*>(arg);
    auto end = (int16_t*)(dpr.buf + dpr.size);
    for (auto sample = (int16_t*)dpr.buf; sample < end;) {
        *sample = self.mEqualizerLeft.processAndNarrow(*sample);
        sample++;
        *sample = self.mEqualizerRight.processAndNarrow(*sample);
        sample++;
    }
}
template<int N>
void MyEqualizerCore<N>::process32bitStereo(AudioNode::DataPullReq& dpr, void* arg) {
    auto& self = *static_cast<MyEqualizerCore<N>*>(arg);
    auto end = (int32_t*)(dpr.buf + dpr.size);
    for (auto sample = (int32_t*)dpr.buf; sample < end;) {
        *sample = self.mEqualizerLeft.process(*sample);
        sample++;
        *sample = self.mEqualizerRight.process(*sample);
        sample++;
    }
}
template<int N>
IEqualizerCore::ProcessFunc MyEqualizerCore<N>::getProcessFunc(StreamFormat fmt, void*& arg)
{
    arg = this;
    return(fmt.bitsPerSample() <= 16) ? process16bitStereo : process32bitStereo;
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
void EspEqualizerCore::process16bitStereo(AudioNode::DataPullReq& dpr, void *arg)
{
    auto& self = *static_cast<EspEqualizerCore*>(arg);
    esp_equalizer_process(self.mEqualizer, (unsigned char*)dpr.buf, dpr.size, self.mSampleRate, self.mChanCount);
}
EspEqualizerCore::~EspEqualizerCore()
{
    if (mEqualizer) {
        esp_equalizer_uninit(mEqualizer);
    }
}
AudioNode::StreamError EqualizerNode::pullData(DataPullReq &dpr)
{
    auto event = mPrev->pullData(dpr);
    if (event) {
        return event;
    }
    MutexLocker locker(mMutex);
    if (dpr.fmt != mFormat) {
        equalizerReinit(dpr.fmt);
    }
    if (volLevelEnabled()) {
        volumeNotifyLevelCallback(); // notify previous levels to compensate output buffering delay
        volumeGetLevel(dpr);
    }
    if (volProcessingEnabled()) {
        volumeProcess(dpr);
    }
    if (!mBypass) {
        mProcessFunc(dpr, this->mProcFuncArg);
    }
    return kNoError;
}
