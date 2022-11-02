#include "decoderFlac.hpp"
#include <mad.h>

static const char* TAG = "flac";

DecoderFlac::DecoderFlac()
{
    auto decSize = fx_flac_size(8192, 2);
    auto memSize = decSize + kInputBufSize + kOutputBufSize;
    auto mem = (char*)heap_caps_malloc(memSize, MALLOC_CAP_SPIRAM);
    if (!mem) {
        ESP_LOGE(TAG, "Out of memory allocating %zu bytes for buffers", memSize);
        mFlacDecoder = mInputBuf = mOutputBuf = nullptr;
        return;
    }
    mFlacDecoder = (fx_flac_t*)mem;
    mInputBuf = mem + decSize;
    mOutputBuf = mInputBuf + kInputBufSize;
    ESP_LOGI(TAG, "Flac decoder uses approx %zu bytes of RAM", memSize + sizeof(DecoderFlac));
    init();
}
DecoderFlac::~DecoderFlac()
{
    if (mFlacDecoder) {
        free(mFlacDecoder);
        mFlacDecoder = mInputBuf = mOutputBuf = nullptr;
    }
}
void DecoderFlac::init()
{
    mInputLen = 0;
    fx_flac_init(mFlacDecoder, kMaxBlockSize, 2);
}

void DecoderFlac::reset()
{
    mOutputFormat.reset();
    init();
}

int DecoderFlac::inputBytesNeeded()
{
    return kInputBufSize - mInputLen;
}

int DecoderFlac::decode(const char* buf, int size)
{
    if (buf) {
        myassert(mInputLen + size <= kInputBufSize);
        memcpy(mInputBuf + mInputLen, buf, size);
        mInputLen += size;
    }
    auto readPtr = mInputBuf;
    auto remain = mInputLen;
    int result = 0;
    for(;;) {
        auto nread = remain;
        auto written = kOutputBufSize / sizeof(int32_t);
        auto ret = fx_flac_process(mFlacDecoder, mInputBuf, &nread, mOutputBuf, &written));
        if (!nread) {
            result = AudioNode::kNeedMoreData;
            break;
        }
        readPtr += nread;
        remain -= nread;
        myassert(remain >= 0);
        if (ret == FLAC_END_OF_METADATA) {
            mOutputFormat.codec = kCodecFlac;
            mOutputFormat.samplerate = fx_flac_get_streaminfo(mFlacDecoder, FLAC_KEY_SAMPLE_RATE);
            mOutputFormat.setChannels(fx_flac_get_streaminfo(mFlacDecoder, FLAC_KEY_N_CHANNELS));
            mOutputFormat.setBits(fx_flac_get_streaminfo(mFlacDecoder, FLAC_KEY_SAMPLE_SIZE));
            auto blkSizeMin = fx_flac_get_streaminfo(mFlacDecoder, FLAC_KEY_MIN_BLOCK_SIZE);
            auto blkSizeMax = fx_flac_get_streaminfo(mFlacDecoder, FLAC_KEY_MAX_BLOCK_SIZE);
            auto frmSizeMin = fx_flac_get_streaminfo(mFlacDecoder, FLAC_KEY_MIN_FRAME_SIZE);
            auto frmSizeMax = fx_flac_get_streaminfo(mFlacDecoder, FLAC_KEY_MAX_FRAME_SIZE);
            ESP_LOGW(TAG, "FLAC format: %d-bit, %d Hz, %d channels (block size: %lld - %lld, frame size: %lld - %lld)",
                mOutputFormat.bits, mOutputFormat.samplerate, mOutputFormat.channels,
                blkSizeMin, blkSizeMax, frmSizeMin, frmSizeMax);
        }
        else if (ret == FLAC_ERR) {
            ESP_LOGW(TAG, "Unrecoverable decode error");
            mInputLen = 0;
            return AudioNode::kErrDecode;
        }
        else if (ret == FLAC_END_OF_FRAME) {
            myassert(written);
            ESP_LOGD(TAG, "Successfully decoded frame of size %d\n", nread);
            convertOutput(written);
            result = written * sizeof(uint16_t);
            break;
        }
        else {
            result = AudioNode::kNeedMoreData;
            break;
        }
    }
    mInputLen = remain;
    if (mInputLen) {
        memmove(mInputBuf, readPtr, remain);
    }
    return result;
}

void DecoderFlac::convertOutput(size_t nSamples)
{
    int16_t* end = (int16_t*)(mOutputBuf + nSamples);
    int16_t* rptr = (int16_t*)mOutputBuf + 1;
    uint16_t* wptr = (uint16_t*)mOutputBuf;
    for (; rptr < end; rptr += 2, wptr++) {
        *wptr = *rptr;
    }
}
