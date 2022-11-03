#include "decoderFlac.hpp"
#include <flac.h>

static const char* TAG = "flac";

DecoderFlac::DecoderFlac()
{
    auto decSize = fx_flac_size(kMaxSamplesPerBlock, 2);
    mFlacDecoder = (fx_flac_t*)malloc(decSize);
    if (!mFlacDecoder) {
        ESP_LOGE(TAG, "Out of memory allocating %zu bytes for decoder", decSize);
        return;
    }
    auto memSize = kInputBufSize + kOutputBufSize;
    auto mem = (uint8_t*)heap_caps_malloc(memSize, MALLOC_CAP_SPIRAM);
    if (!mem) {
        ESP_LOGE(TAG, "Out of memory allocating %zu bytes for buffers", memSize);
        freeBuffers();
        return;
    }
    mInputBuf = mem;
    mOutputBuf = (int16_t*)(mInputBuf + kInputBufSize);
    ESP_LOGI(TAG, "Flac decoder uses approx %zu bytes of RAM", decSize + memSize + sizeof(DecoderFlac));
    init();
}
DecoderFlac::~DecoderFlac()
{
    freeBuffers();
}
void DecoderFlac::freeBuffers()
{
    if (mFlacDecoder) {
        free(mFlacDecoder);
        mFlacDecoder = nullptr;
    }
    if (mInputBuf) {
        free(mInputBuf);
        mInputBuf = nullptr;
        mOutputBuf = nullptr;
    }
}
void DecoderFlac::init()
{
    mInputLen = 0;
    fx_flac_init(mFlacDecoder, kMaxSamplesPerBlock, 2);
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
        uint32_t nread = remain;
        auto ret = fx_flac_process(mFlacDecoder, readPtr, &nread);
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
            ESP_LOGI(TAG, "FLAC format: %d-bit, %d Hz, %d channels (samples per frame: %lld - %lld, frame size: %lld - %lld bytes)",
                mOutputFormat.bits(), mOutputFormat.samplerate, mOutputFormat.channels(),
                blkSizeMin, blkSizeMax, frmSizeMin, frmSizeMax);
        }
        else if (ret == FLAC_ERR) {
            ESP_LOGW(TAG, "Unrecoverable decode error");
            mInputLen = 0;
            return AudioNode::kErrDecode;
        }
        else if (ret == FLAC_END_OF_FRAME) {
            ESP_LOGD(TAG, "Successfully decoded frame of size %d\n", nread);
            result = getOutput();
            break;
        }
        else {
            ESP_LOGD(TAG, "Unrecognized decoder status: %d", ret);
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

int DecoderFlac::getOutput()
{
    auto nSamples = fx_flac_get_frame_nsamples(mFlacDecoder);
    assert(nSamples);
    int32_t** decSamples = fx_flac_get_output(mFlacDecoder);
    uint8_t nChan = mOutputFormat.channels();
    auto wptr = mOutputBuf;
    if (nChan == 2) {
        int outBytes = nSamples * 4;
        if (kOutputBufSize < outBytes) {
            return AudioNode::kErrBuffer;
        }
        int32_t* ch0 = decSamples[0];
        int32_t* ch1 = decSamples[1];
        for (int i = 0; i < nSamples; i++) {
            *(wptr++) = ch0[i];
            *(wptr++) = ch1[i];
        }
        return outBytes;
    } else if (nChan == 1) {
        int outBytes = nSamples * 2;
        if (kOutputBufSize < outBytes) {
            return AudioNode::kErrBuffer;
        }
        for (int i = 0; i < nSamples; i++) {
            *(wptr++) = (*decSamples)[i];
        }
        return outBytes;
    } else {
        ESP_LOGE(TAG, "Unsupported number of channels: %d", nChan);
        return AudioNode::kErrStreamFmt;
    }
}
