#include "decoderFlac.hpp"
#include <flac.h>

static const char* TAG = "flac";

DecoderFlac::DecoderFlac(AudioNode& src): Decoder(src, kCodecFlac)
{
    mOutputBuf = (int16_t*)AudioNode::mallocTrySpiram(kOutputBufSize);
    if (!mOutputBuf) {
        ESP_LOGE(TAG, "Out of memory allocating %zu bytes for output buffer", kOutputBufSize);
        abort();
    }
    mDecoder = FLAC__stream_decoder_new();
    if (!mDecoder) {
        ESP_LOGE(TAG, "Out of memory allocating FLAC decoder");
        abort();
    }
    ESP_LOGI(TAG, "Flac decoder uses approx %zu bytes of RAM", kOutputBufSize); //FIXME
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
    outputFormat.clear();
    FLAC__stream_decoder_reset(mDecoder);
}
FLAC__StreamDecoderReadStatus DecoderFlac::readCb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void* userp)
{
    auto& self = *static_cast<DecoderFlac*>(userp);
    AudioNode::DataPullReq dpr(*bytes);
    auto event = self.mSrcNode.pullData(dpr);
    if (!event) {
        *bytes = dpr.size;
        memcpy(buffer, dpr.buf, dpr.size);
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    if (event == AudioNode::kStreamChanged) {
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    } else {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
}
void DecoderFlac::errorCb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    ESP_LOGE(TAG, "FLAC decoder notified about error: %d", status);
}

AudioNode::StreamError DecoderFlac::pullData(AudioNode::DataPullReq& output)
{
    do {
        mOutputLen = 0;
        if (!FLAC__stream_decoder_process_single(mDecoder)) {
            auto err = FLAC__stream_decoder_get_state(mDecoder);
            return (err == FLAC__STREAM_DECODER_END_OF_STREAM) ? AudioNode::kStreamChanged : AudioNode::kErrDecode;
        }
    } while(!mOutputLen);
    output.fmt = outputFormat;
    output.size = mOutputLen;
    output.buf = (char*)mOutputBuf;
    return AudioNode::kNoError;
}
FLAC__StreamDecoderWriteStatus DecoderFlac::writeCb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *userp)
{
    auto& self = *static_cast<DecoderFlac*>(userp);
    auto header = frame->header;
    auto nChans = header.channels;
    auto nSamples = header.blocksize;
    assert(nChans && nSamples);
    int outBytes = nChans * nSamples * 2;
    if (outBytes > kOutputBufSize) {
        ESP_LOGE(TAG, "Output of FLAC codec is too large to fit into output buffer, aborting decode");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    auto& fmt = self.outputFormat;
    if (!fmt) {
        fmt.setNumChannels(nChans);
        auto bps = header.bits_per_sample;
        assert(bps == 16);
        fmt.setBitsPerSample(header.bits_per_sample);
        fmt.setSampleRate(header.sample_rate);
        fmt.setCodec(kCodecFlac);
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

