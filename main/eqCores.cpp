#include <esp_equalizer.h>
#include "eqCores.hpp"
#include <esp_log.h>
#include "streamPackets.hpp"

static const char* TAG = "eq";
template <bool IsStereo>
MyEqualizerCore<IsStereo>::MyEqualizerCore(uint8_t numBands, uint32_t sampleRate)
: mEqualizer(numBands, sampleRate)
{
    ESP_LOGI(TAG, "Created %d-band custom %s equalizer", numBands, IsStereo ? "stereo" : "mono");
}

template<bool IsStereo>
void MyEqualizerCore<IsStereo>::process(void* data, int dataLen)
{
#ifdef EQ_PERF
    static float msAvg = 0;
    ElapsedTimer timer;
#endif
    mEqualizer.process((float*)data, dataLen / ((IsStereo ? 2 : 1) * sizeof(float)));
#ifdef EQ_PERF
    auto ms = timer.msElapsed();
    msAvg = (msAvg * 99 + ms) / 100;
    ESP_LOGI(TAG, "eq process(my) %d: %d (%.2f) ms", dataLen / 8, ms, msAvg);
#endif
}

EspEqualizerCore::EspEqualizerCore(StreamFormat fmt)
: mSampleRate(fmt.sampleRate()), mChanCount(fmt.numChannels())
{
    mEqualizer = esp_equalizer_init(mChanCount, mSampleRate, 10, 0);
    ESP_LOGI("eq", "Created ESP equalizer core");
}
void EspEqualizerCore::setBandGain(uint8_t band, int8_t dbGain)
{
    myassert(mEqualizer && band < 10);
    mGains[band] = dbGain;
    esp_equalizer_set_band_value(mEqualizer, dbGain, band, 0);
    esp_equalizer_set_band_value(mEqualizer, dbGain, band, 1);
}
void EspEqualizerCore::updateAllFilters()
{
    auto g = gains();
    for (uint8_t i = 0; i < 10; i++) {
        esp_equalizer_set_band_value(mEqualizer, g[i], i, 0);
        esp_equalizer_set_band_value(mEqualizer, g[i], i, 1);
    }
}
EqBandConfig EspEqualizerCore::bandConfig(uint8_t n) const
{
    static const uint16_t bandFreqs[10] = {
        31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
    };
    return {.freq = bandFreqs[n], .Q = 707};
}
void EspEqualizerCore::process(void* data, int dataLen)
{
#ifdef EQ_PERF
    static float avgTime = 0.0;
    ElapsedTimer timer;
#endif
    esp_equalizer_process(mEqualizer, (unsigned char*)data, dataLen, mSampleRate, mChanCount);
#ifdef EQ_PERF
    auto msTime = timer.msElapsed();
    avgTime = (avgTime * 99 + msTime) / 100;
    ESP_LOGI(TAG, "eq process %d (esp): %dms (%.2f)", dataLen / 4, msTime, avgTime);
#endif
}
EspEqualizerCore::~EspEqualizerCore()
{
    if (mEqualizer) {
        esp_equalizer_uninit(mEqualizer);
    }
}
template class MyEqualizerCore<true>;
template class MyEqualizerCore<false>;
