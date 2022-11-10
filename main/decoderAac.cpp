#include "decoderAac.hpp"

static const char* TAG = "aacdec";

DecoderAac::DecoderAac(AudioNode& src): Decoder(src, kCodecAac)
{
    mInputBuf = (unsigned char*)AudioNode::mallocTrySpiram(kInputBufSize + kOutputBufSize);
    if (!mInputBuf) {
        ESP_LOGE(TAG, "Out of memory allocating buffers");
        abort();
    }
    mOutputBuf = (int16_t*)(mInputBuf + kInputBufSize);
    initDecoder();
}
void DecoderAac::initDecoder()
{
    mOutputSize = 0;
    auto before = xPortGetFreeHeapSize();
    mDecoder = AACInitDecoder();
    if (!mDecoder) {
        ESP_LOGE(TAG, "Out of memory creating AAC decoder.");
    }
    ESP_LOGW(TAG, "Allocating AAC decoder took approx %d bytes of RAM (without buffers)", before - xPortGetFreeHeapSize());
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
    mOutputSize = 0;
    freeDecoder();
    initDecoder();
    outputFormat.clear();
    outputFormat.setCodec(kCodecAac);
}

AudioNode::StreamError DecoderAac::pullData(AudioNode::DataPullReq& output)
{
    int inputLen = 0;
    for(;;) {
        AudioNode::DataPullReq idpr(kInputBufSize - inputLen);
        auto event = mSrcNode.pullData(idpr);
        if (event) {
            return event;
        }
        myassert(idpr.size);
        myassert(idpr.size + inputLen <= kInputBufSize);
        memcpy(mInputBuf + inputLen, idpr.buf, idpr.size);
        inputLen += idpr.size;

        unsigned char* inPtr = mInputBuf;
        int remain = idpr.size;
        auto err = AACDecode(mDecoder, &inPtr, &remain, mOutputBuf);
        if (err == ERR_AAC_INDATA_UNDERFLOW) { // need more data
         // ESP_LOGI(TAG, "decoder underflow");
            mSrcNode.confirmRead(idpr.size);
            continue;
        }
        else if (err == 0) { // decode success
            int consumed = idpr.size - remain;
            myassert(consumed >= 0);
            mSrcNode.confirmRead(consumed);
            if (!mOutputSize) { // we haven't yet initialized output format info
                getStreamFormat();
            }
            output.buf = (char*)mOutputBuf;
            output.size = mOutputSize;
            output.fmt = outputFormat;
            return AudioNode::kNoError;
        }
        else { //err < 0 - error, try to re-sync
            // inPtr and remain are guaranteed to not be updated if AACDecode() failed
            ESP_LOGI(TAG, "Decode error %d, looking for next sync word", err);
            auto pos = AACFindSyncWord(mInputBuf + 1, inputLen - 1);
            if (pos >= 0) {
                mSrcNode.confirmRead(idpr.size);
                pos += 1; // adjust for +1 start in buffer
                ESP_LOGI(TAG, "Sync word found at %d, discarding data before it and repeating", pos);
                inputLen -= pos;
                memmove(mInputBuf, mInputBuf+pos, inputLen);
                continue;
            }
            // can't find frame start, discard everything in buffer and request more data
            ESP_LOGI(TAG, "Can't find sync word, discarding whole buffer and requesting more data");
            // this can be the first byte of a sync word, preserve it
            mSrcNode.confirmRead(mInputBuf[inputLen - 1] == 0xff ? idpr.size - 1 : idpr.size);
            inputLen = 0;
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
    mOutputSize = info.outputSamps * sizeof(uint16_t);
    ESP_LOGW(TAG, "AAC%s 16-bit %s, %d Hz, %d bps, %d samples/frame",
        info.sampRateOut == info.sampRateCore ? "" : " SBR",
        info.nChans == 2 ? "stereo" : "mono", info.sampRateOut,
        info.bitRate, info.outputSamps);
}
