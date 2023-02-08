#include "equalizerNode.hpp"
#include <nvsHandle.hpp>
//#include <esp_equalizer.h>

template <int N>
MyEqualizerCore<N>::MyEqualizerCore(const EqBandConfig* cfg)
: mEqualizerLeft(cfg), mEqualizerRight(cfg)
{}

EqualizerNode::EqualizerNode(IAudioPipeline& parent, NvsHandle& nvs)
: AudioNode(parent, "equalizer"), mNvsHandle(nvs)
{
    mNumBands = mNvsHandle.readDefault("eq.nbands", 10);
}

#define CASE_N_BANDS(n) \
    case n: mCore.reset(new MyEqualizerCore<n>(nullptr)); break

void EqualizerNode::createCoreForNBands(int n, StreamFormat fmt)
{
    switch(n) {
        CASE_N_BANDS(10);
        CASE_N_BANDS(9);
        CASE_N_BANDS(8);
        CASE_N_BANDS(7);
        CASE_N_BANDS(6);
        CASE_N_BANDS(5);
        CASE_N_BANDS(4);
        default: assert(false);
    }
    mGains.reset(new int8_t[n]);
}
std::string EqualizerNode::eqName()
{
    std::string result = "eq.";
    appendAny(result, mNumBands);
    result += ".g";
    return result;
}
void EqualizerNode::equalizerReinit(StreamFormat fmt)
{
    ESP_LOGI(mTag, "Equalizer reinit");
    mFormat = fmt;
    if (!mCore || mCore->bandCount() != mNumBands) {
        createCoreForNBands(mNumBands, fmt);
        size_t len = mNumBands;
        if (mNvsHandle.readBlob(eqName().c_str(), mGains.get(), len) == ESP_OK && len == mNumBands) {
            ESP_LOGI(mTag, "Loaded equalizer gains from NVS");
        } else {
            memset(mGains.get(), 0, mNumBands);
        }
    }
    mCore->init(fmt.sampleRate(), mGains.get());
    mProcessFunc = mCore->getProcessFunc(fmt, mProcFuncArg);
}
bool EqualizerNode::setNumBands(uint8_t n) {
    if (n < kMinBands || n > kMaxBands) {
        return false;
    }
    MutexLocker locker(mMutex);
    mNumBands = n;
    equalizerReinit(mFormat);
    return true;
}
void EqualizerNode::setBandGain(uint8_t band, int8_t dbGain)
{
    MutexLocker locker(mMutex);
    assert(mCore);
    mGains[band] = dbGain;
    mCore->setBandGain(band, dbGain);
}

void EqualizerNode::setAllGains(const int8_t* gains, int len)
{
    MutexLocker locker(mMutex);
    if (gains) {
        assert(len == mNumBands);
        memcpy(mGains.get(), gains, mNumBands);
    } else {
        memset(mGains.get(), 0, mNumBands);
    }
    mCore->setAllGains(mGains.get());
}
bool EqualizerNode::saveGains()
{
    MutexLocker locker(mMutex);
    return mNvsHandle.writeBlob(eqName().c_str(), mGains.get(), mNumBands) == ESP_OK;
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
