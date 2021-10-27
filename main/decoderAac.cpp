#include "decoderAac.hpp"

static const char* TAG = "aacdec";

DecoderAac::DecoderAac()
{
    mInputBuf = (unsigned char*)malloc(kInputBufSize);
    if (!mInputBuf) {
        ESP_LOGE(TAG, "Out of memory allocating input buffer");
        return;
    }
    initDecoder();
}
void DecoderAac::initDecoder()
{
    mInputLen = 0;
    mNextFrameOffs = 0;
    mOutputSize = 0;
    auto before = xPortGetFreeHeapSize();
    mDecoder = AACInitDecoder();
    if (!mDecoder) {
        ESP_LOGE(TAG, "Out of memory creating AAC decoder.");
    }
    ESP_LOGW(TAG, "Allocating AAC decoder took %d bytes of RAM", before - xPortGetFreeHeapSize() + sizeof(DecoderAac));
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
    mOutputFormat.reset();
}

int DecoderAac::inputBytesNeeded()
{
    return kInputBufSize - mInputLen;
}

int DecoderAac::decode(const char* buf, int size)
{
    if (buf) {
        myassert(mInputLen + size <= kInputBufSize);
        memcpy(mInputBuf+mInputLen, buf, size);
        mInputLen += size;
      //ESP_LOGI(TAG, "called with %d bytes", size);
    }
    for(;;) {
        if (mInputLen < 2) {
            return AudioNode::kNeedMoreData;
        }
        unsigned char* inPtr = mInputBuf;
        int remain = mInputLen;
        auto err = AACDecode(mDecoder, &inPtr, &remain, mOutputBuf);
        if (err == ERR_AAC_INDATA_UNDERFLOW) {
         // ESP_LOGI(TAG, "decoder underflow");
            return AudioNode::kNeedMoreData;
        } else if (err == 0) { // decode success
            if (remain) {
                memmove(mInputBuf, inPtr, remain); // move remaining data to start of buffer
            }
            mInputLen = remain;
            if (!mOutputSize) { // we haven't yet initialized output format info
                getStreamFormat();
            }
            return mOutputSize;
        } else { //err < 0 - error, try to re-sync
            // inPtr and remain are guaranteed to not be updated if AACDecode() failed
            assert(mInputLen > 1);
            ESP_LOGI(TAG, "Decode error %d, looking for next sync word", err);
            auto pos = AACFindSyncWord(mInputBuf+1, mInputLen-1);
            if (pos >= 0) {
                pos += 1;
                ESP_LOGI(TAG, "Sync word found at %d, discarding data before it and repeating", pos);
                memmove(mInputBuf, mInputBuf+pos, mInputLen-pos);
                assert(buf[0] == 0xff && buf[1] == 0xf0);
                continue;
            }
            // can't find frame start, discard everything in buffer and request more data
            ESP_LOGI(TAG, "Can't find sync word, discarding whole buffer and requesting more data");
            if (mInputBuf[mInputLen-1] == 0xff) { // this can be the first byte of a sync word, preserve it
                mInputBuf[0] = 0xff;
                mInputLen = 1;
            } else {
                mInputLen = 0;
            }
            return AudioNode::kNeedMoreData;
        }
        myassert(false); // shouldn't reach here
    }
}

void DecoderAac::getStreamFormat()
{
    mOutputFormat.codec = kCodecAac;
    AACFrameInfo info;
    AACGetLastFrameInfo(mDecoder, &info);
    mOutputFormat.samplerate = info.sampRateOut;
    mOutputFormat.setChannels(info.nChans);
    mOutputFormat.setBits(16);
    mOutputSize = info.outputSamps * sizeof(uint16_t);
    ESP_LOGW(TAG, "AAC%s 16-bit %s, %d Hz, %d bps, %d samples/frame",
        info.sampRateOut == info.sampRateCore ? "" : " SBR",
        info.nChans == 2 ? "stereo" : "mono", info.sampRateOut,
        info.bitRate, info.outputSamps);
}
