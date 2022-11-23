#include "decoderFlac.hpp"
#include <flac.h>

static const char* TAG = "flac";

DecoderFlac::DecoderFlac(AudioNode& src): Decoder(src, kCodecFlac)
{
    mOutputBuf = (int16_t*)utils::mallocTrySpiram(kOutputBufSize);
    if (!mOutputBuf) {
        ESP_LOGE(TAG, "Out of memory allocating %zu bytes for output buffer", kOutputBufSize);
        abort();
    }
    mDecoder = FLAC__stream_decoder_new();
    if (!mDecoder) {
        ESP_LOGE(TAG, "Out of memory allocating FLAC decoder");
        abort();
    }
    auto ret = FLAC__stream_decoder_init_stream(mDecoder, readCb, nullptr, nullptr, nullptr, nullptr,
         writeCb, metadataCb, errorCb, this);
    assert(ret == FLAC__STREAM_DECODER_INIT_STATUS_OK);
}
DecoderFlac::~DecoderFlac()
{
    FLAC__stream_decoder_delete(mDecoder);
    free(mOutputBuf);
}
void DecoderFlac::reset()
{
    printf("============== FLAC reset\n");
    outputFormat.clear();
    FLAC__stream_decoder_reset(mDecoder);
}
FLAC__StreamDecoderReadStatus DecoderFlac::readCb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void* userp)
{
    printf("readCb called\n");
    auto& self = *static_cast<DecoderFlac*>(userp);
    assert(self.mDprPtr);
    auto& dpr = *self.mDprPtr;
    dpr.size = *bytes;
    auto event = self.mLastStreamEvent = self.mSrcNode.pullData(dpr);
    if (!event) {
        *bytes = dpr.size;
        memcpy(buffer, dpr.buf, dpr.size);
        self.mSrcNode.confirmRead(dpr.size);
        char fourcc[5];
        auto fcclen = std::min(4, dpr.size);
        memcpy(fourcc, dpr.buf, fcclen);
        fourcc[fcclen] = 0;
        printf("FLAC: readcb: returning %d bytes(fourcc: %s)\n", dpr.size, fourcc);
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    printf("FLAC: readcb: returning event %s\n",  AudioNode::streamEventToStr(event));
    if (event == AudioNode::kStreamChanged) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT; //FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    } else {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
}
void DecoderFlac::errorCb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    ESP_LOGE(TAG, "FLAC decode error: %d", status);
}
void DecoderFlac::metadataCb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
}
static const char* kMsgManyFramesZeroOutput = "Many frames decoded without generating any output";
AudioNode::StreamError DecoderFlac::pullData(AudioNode::DataPullReq& dpr)
{
    mDprPtr = &dpr;
    mOutputLen = 0;
    for (int i = 0; i < 2; i++) {
        for (int i = 0; !mOutputLen && (i < 6); i++) {
            auto ok = FLAC__stream_decoder_process_single(mDecoder);
            printf("flac decode frame: ok = %d(outLen: %d)\n", ok, mOutputLen);
            if (!ok) {
                dpr.clear();
                auto err = FLAC__stream_decoder_get_state(mDecoder);
                printf("flac state after error: %d, free internal RAM: %d\n", err, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                return (err == FLAC__STREAM_DECODER_END_OF_STREAM || err == FLAC__STREAM_DECODER_ABORTED)
                        ? mLastStreamEvent
                        : AudioNode::kErrDecode;
            }
        }
        if (!mOutputLen) {
            ESP_LOGW(TAG, "%s, resetting decoder...", kMsgManyFramesZeroOutput);
            reset();
        } else {
            break;
        }
    }
    if (!mOutputLen) {
        ESP_LOGW(TAG, "%s, codec reset didn't help, returning error", kMsgManyFramesZeroOutput);
        dpr.size = 0;
        dpr.buf = nullptr;
        return AudioNode::kErrDecode;
    }
    dpr.fmt = outputFormat;
    dpr.size = mOutputLen;
    dpr.buf = (char*)mOutputBuf;
    mDprPtr = nullptr; // we don't want a dangling invalid pointer, even if it's not used
    return AudioNode::kNoError;
}
FLAC__StreamDecoderWriteStatus DecoderFlac::writeCb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *userp)
{
    auto& self = *static_cast<DecoderFlac*>(userp);
    auto header = frame->header;
    auto nChans = header.channels;
    auto nSamples = header.blocksize;
    if (!nChans || !nSamples) {
        printf("FLAC assert: nChans=%d, nSamples=%d\n", nChans, nSamples);
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    assert(nSamples);
    int outBytes = nChans * nSamples * 2;
    if (outBytes > kOutputBufSize) {
        ESP_LOGE(TAG, "Output of FLAC codec is too large to fit into output buffer, aborting decode");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    auto& fmt = self.outputFormat;
    if (!fmt.sampleRate()) {
        fmt.setNumChannels(nChans);
        auto bps = header.bits_per_sample;
        assert(bps == 16);
        fmt.setBitsPerSample(header.bits_per_sample);
        fmt.setSampleRate(header.sample_rate);
    }
    if (nChans == 2) {
        auto ch0 = buffer[0];
        auto ch1 = buffer[1];
        auto wptr = self.mOutputBuf;
        for (int i = 0; i < nSamples; i++) {
            *(wptr++) = ch0[i];
            *(wptr++) = ch1[i];
        }
    } else if (nChans == 1) {
        memcpy(self.mOutputBuf, buffer, outBytes);
    } else {
        ESP_LOGE(TAG, "Unsupported number of channels: %d", nChans);
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    self.mOutputLen = outBytes;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

