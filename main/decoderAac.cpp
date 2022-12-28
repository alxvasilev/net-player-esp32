#include "decoderAac.hpp"

#ifdef CONFIG_ESP32_SPIRAM_SUPPORT
    #define HELIX_FEATURE_AUDIO_CODEC_AAC_SBR 1
#endif

#include <aacdec.h>

static const char* TAG = "aacdec";

DecoderAac::DecoderAac(DecoderNode& parent, AudioNode& src): Decoder(parent, src, kCodecAac)
{
    mInputBuf = (unsigned char*)utils::mallocTrySpiram(kInputBufSize + kOutputBufSize);
    if (!mInputBuf) {
        ESP_LOGE(TAG, "Out of memory allocating buffers");
        abort();
    }
    mOutputBuf = (int16_t*)(mInputBuf + kInputBufSize);
    initDecoder();
}
void DecoderAac::initDecoder()
{
    mInputLen = mOutputLen = 0;
    mNextFramePtr = mInputBuf;
    mDecoder = AACInitDecoder();
    if (!mDecoder) {
        ESP_LOGE(TAG, "Out of memory creating AAC decoder.");
    }
}
void DecoderAac::freeDecoder()
{
    if (mDecoder) {
        AACFreeDecoder(mDecoder);
        mDecoder = nullptr;
    }
}
DecoderAac::~DecoderAac()
{
    freeDecoder();
    free(mInputBuf);
}

void DecoderAac::reset()
{
    freeDecoder();
    initDecoder();
    outputFormat.clear();
}

AudioNode::StreamError DecoderAac::pullData(AudioNode::DataPullReq& dpr)
{
    bool needMoreData = (mInputLen == 0);
    for(;;) {
        if (needMoreData || mInputLen < kMinAllowedAacInputSize) {
            needMoreData = false;
            if (mNextFramePtr != mInputBuf) {
                memmove(mInputBuf, mNextFramePtr, mInputLen);
                mNextFramePtr = mInputBuf;
            }
            int reqSize = kInputBufSize - mInputLen;
            if (reqSize <= 0) {
                ESP_LOGE(TAG, "Can't decode a frame, even though input buffer is full");
                mInputLen = 0;
                mNextFramePtr = mInputBuf;
                return AudioNode::kErrDecode;
            }
            dpr.size = reqSize;
            auto event = mSrcNode.pullData(dpr);
            if (event) {
                return event;
            }
            // Existing data starts at mInputBuf
            myassert(dpr.size && dpr.size <= reqSize);
            memcpy(mInputBuf + mInputLen, dpr.buf, dpr.size);
            mSrcNode.confirmRead(dpr.size);
            mInputLen += dpr.size;
        }
        // printf("AACDecode: inLen=%d, offs=%d\n", mInputLen, mNextFramePtr - mInputBuf);
        auto err = AACDecode(mDecoder, &mNextFramePtr, &mInputLen, mOutputBuf);
        if (err == ERR_AAC_INDATA_UNDERFLOW) { // need more data
            ESP_LOGD(TAG, "Decoder underflow");
            needMoreData = true;
            continue;
        }
        else if (err == 0) { // decode success
            if (!mOutputLen) { // we haven't yet initialized output format info
                getStreamFormat();
            }
            dpr.buf = (char*)mOutputBuf;
            dpr.size = mOutputLen;
            dpr.fmt = outputFormat;
            return AudioNode::kNoError;
        }
        else { //err < 0 - error, try to re-sync
            // mNextFramePtr and mInputLen are guaranteed to not be updated if AACDecode() failed
            ESP_LOGW(TAG, "Decode error %d, looking for next sync word", err);
            auto pos = AACFindSyncWord(mNextFramePtr + 1, mInputLen - 1);
            if (pos >= 0) {
                pos += 1; // adjust for +1 start in buffer
                ESP_LOGI(TAG, "Sync word found at %d, discarding data before it and repeating", pos);
                mNextFramePtr += pos;
                mInputLen -= pos;
                continue;
            }
            // can't find frame start, discard everything in buffer and request more data
            ESP_LOGI(TAG, "Can't find sync word, discarding whole buffer and requesting more data");
            mNextFramePtr = mInputBuf;
            if (*(mNextFramePtr + mInputLen - 1) == 0xff) {
                // this can be the first byte of a sync word, preserve it
                *mInputBuf = 0xff;
                mInputLen = 1;
            } else {
                mInputLen = 0;
            }
            needMoreData = true;
            continue;
        }
        myassert(false); // shouldn't reach here
    }
}

void DecoderAac::getStreamFormat()
{
    AACFrameInfo info;
    AACGetLastFrameInfo(mDecoder, &info);
    outputFormat.setSampleRate(info.sampRateOut);
    outputFormat.setNumChannels(info.nChans);
    outputFormat.setBitsPerSample(16);
    mOutputLen = info.outputSamps * sizeof(uint16_t);
    ESP_LOGW(TAG, "AAC%s 16-bit %s, %d Hz, %d bps, %d samples/frame",
        info.sampRateOut == info.sampRateCore ? "" : " SBR",
        info.nChans == 2 ? "stereo" : "mono", info.sampRateOut,
        info.bitRate, info.outputSamps);
}
