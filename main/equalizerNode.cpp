#include "equalizerNode.hpp"

AudioNode::StreamError EqualizerNode::pullData(DataPullReq &dpr, int timeout)
{
    MutexLocker locker(mMutex);
    auto ret = mPrev->pullData(dpr, timeout);
    if (ret < 0) {
        return ret;
    }
    if (dpr.fmt.samplerate != mSampleRate) {
        mSampleRate = dpr.fmt.samplerate;
        mEqualizerLeft.init(mSampleRate);
        if (dpr.fmt.isStereo()) {
            mEqualizerRight.init(mSampleRate);
        }
    }
    auto bitsPerSample = dpr.fmt.bits();
    ElapsedTimer tim;
    if (bitsPerSample == 16) {
        process<int16_t>(dpr.buf, dpr.size, dpr.fmt.isStereo());
    } else if (bitsPerSample == 32) {
        process<int32_t>(dpr.buf, dpr.size, dpr.fmt.isStereo());
    }
    ESP_LOGI(mTag, "Process time: %lld us", tim.usElapsed());
    return kNoError;
}
