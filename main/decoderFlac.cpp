#include "decoderFlac.hpp"

static const char* TAG = "flac";

DecoderFlac::DecoderFlac(DecoderNode& parent, AudioNode& src, bool isOgg)
    : Decoder(parent, src), mCodecInfo(Codec::kCodecFlac,
      isOgg ? Codec::kTransportOgg : Codec::kTransportDefault)
{
    mOutputBuf = (uint8_t*)utils::mallocTrySpiram(kOutputBufSize);
    if (!mOutputBuf) {
        ESP_LOGE(TAG, "Out of memory allocating %zu bytes for output buffer", kOutputBufSize);
        abort();
    }
    mDecoder = FLAC__stream_decoder_new();
    if (!mDecoder) {
        ESP_LOGE(TAG, "Out of memory allocating FLAC decoder");
        abort();
    }
    auto ret = isOgg
         ? FLAC__stream_decoder_init_ogg_stream(mDecoder, readCb, nullptr, nullptr, nullptr, nullptr,
             writeCb, metadataCb, errorCb, this)
         : FLAC__stream_decoder_init_stream(mDecoder, readCb, nullptr, nullptr, nullptr, nullptr,
             writeCb, metadataCb, errorCb, this);
    if (ret != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        ESP_LOGE(TAG, "Error %s initializing FLAC decoder", FLAC__StreamDecoderInitStatusString[ret]);
    }
}
DecoderFlac::~DecoderFlac()
{
    FLAC__stream_decoder_delete(mDecoder);
    free(mOutputBuf);
}
void DecoderFlac::reset()
{
    ESP_LOGI(TAG, "Resetting decoder");
    outputFormat.clear();
    FLAC__stream_decoder_reset(mDecoder);
}
FLAC__StreamDecoderReadStatus DecoderFlac::readCb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void* userp)
{
//    printf("readCb\n");
    auto& self = *static_cast<DecoderFlac*>(userp);
    assert(self.mDprPtr);
    auto& dpr = *self.mDprPtr;
    dpr.size = *bytes;
    auto event = self.mLastStreamEvent = self.mSrcNode.pullData(dpr);
    if (event == AudioNode::kNoError) {
        *bytes = dpr.size;
        memcpy(buffer, dpr.buf, dpr.size);
        self.mSrcNode.confirmRead(dpr.size);
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    else if (event == AudioNode::kStreamChanged) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT; //FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    } else {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
}
void DecoderFlac::errorCb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    ESP_LOGE(TAG, "FLAC decode error: %s(%d)", FLAC__StreamDecoderErrorStatusString[status], status);
}
void DecoderFlac::metadataCb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
}
AudioNode::StreamError DecoderFlac::pullData(AudioNode::DataPullReq& dpr)
{
    if (mOutputReadOfs) {
        dpr.fmt = outputFormat;
        auto size = std::min(mOutputLen - mOutputReadOfs, (int)mOutputChunkSize);
        dpr.size = size;
        dpr.buf = (char*)mOutputBuf + mOutputReadOfs;
        mOutputReadOfs += size;
        if (mOutputReadOfs >= mOutputLen) {
            mOutputReadOfs = 0;
        }
//        printf("send next chunk: %d\n", dpr.size);
        return AudioNode::kNoError;
    }
    mDprPtr = &dpr;
    mOutputLen = 0;
    for (int i = 0; !mOutputLen && (i < 10); i++) {
        auto ok = FLAC__stream_decoder_process_single(mDecoder);
        if (!ok) {
            dpr.clearExceptStreamId();
            auto err = FLAC__stream_decoder_get_state(mDecoder);
            const char* errStr = (err >= 0) ? FLAC__StreamDecoderStateString[err] : "(invalid code)";
            ESP_LOGW(TAG, "Decoder returned error %s(%d)", errStr, err);
            return mLastStreamEvent ? mLastStreamEvent : AudioNode::kErrDecode;
        }
    }
    mDprPtr = nullptr; // we don't want a dangling invalid pointer, even if it's not used
    if (!mOutputLen) {
        ESP_LOGW(TAG, "Many frames decoded without generating any output, returning error");
        dpr.clearExceptStreamId();
        return AudioNode::kErrDecode;
    }
    dpr.fmt = outputFormat;
    dpr.buf = (char*)mOutputBuf;
    if (mOutputChunkSize) {
        mOutputReadOfs = dpr.size = mOutputChunkSize;
    } else {
        dpr.size = mOutputLen;
        mOutputReadOfs = 0;
    }
    return AudioNode::kNoError;
}

template <typename T, int Shift = 0>
bool DecoderFlac::outputStereoSamples(int nSamples, const FLAC__int32* const channels[])
{
    int outBytes = 2 * nSamples * sizeof(T);
    if (outBytes > kOutputBufSize) {
        printf("flac: output too large: max expected: %d, actual: %d (nSamples=%d)\n", kOutputBufSize, outBytes, nSamples);
        return false;
    }
    auto ch0 = channels[0];
    auto ch1 = channels[1];
    T* wptr = (T*)mOutputBuf;
    for (int i = 0; i < nSamples; i++) {
        *(wptr++) = ch0[i];
        *(wptr++) = ch1[i];
    }
    mOutputLen = outBytes;
    return true;
}
template <typename T, int Shift=0>
bool DecoderFlac::outputMonoSamples(int nSamples, const FLAC__int32* const samples[])
{
    int outBytes = nSamples * sizeof(T);
    if (outBytes > kOutputBufSize) {
        return false;
    }
    T* wptr = (T*)mOutputBuf;
    auto end = samples[0] + nSamples;
    for (auto sample = samples[0]; sample < end;) {
        *(wptr++) = *(sample++);
    }
    mOutputLen = outBytes;
    return true;
}
FLAC__StreamDecoderWriteStatus DecoderFlac::writeCb(const FLAC__StreamDecoder *decoder,
    const FLAC__Frame* frame, const FLAC__int32* const buffer[], void *userp)
{
    auto& self = *static_cast<DecoderFlac*>(userp);
    auto header = frame->header;
    auto nChans = header.channels;
    auto nSamples = header.blocksize;
    if (!nChans || !nSamples) {
        ESP_LOGE(TAG, "No channels or samples on output");
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    auto bps = header.bits_per_sample;
    auto oldFmt = self.outputFormat;
    auto& fmt = self.outputFormat;
    fmt.setNumChannels(nChans);
    fmt.setBitsPerSample(bps);
    fmt.setSampleRate(header.sample_rate);
    if (fmt != oldFmt) {
        ESP_LOGI(TAG, "Output format is %d-bit, %.1fkHz %s", bps, (float)fmt.sampleRate() / 1000, (nChans == 2) ? "stereo" : "mono");
        fmt.setCodec(self.mCodecInfo);
        if (nChans == 2) {
            if (bps == 16) {
                self.mOutputFunc = &DecoderFlac::outputStereoSamples<int16_t>;
            } else if (bps == 24) {
                self.mOutputFunc = &DecoderFlac::outputStereoSamples<int32_t, 8>;
            } else if (bps == 32) {
                self.mOutputFunc = &DecoderFlac::outputStereoSamples<int32_t>;
            }else if (bps == 8) {
                self.mOutputFunc = &DecoderFlac::outputStereoSamples<int16_t, 8>;
            } else {
                ESP_LOGE(TAG, "Unsupported bits per sample: %d", header.bits_per_sample);
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
        }
        else if (nChans == 1) {
            if (bps == 16) {
                self.mOutputFunc = &DecoderFlac::outputMonoSamples<int16_t>;
            } else if (bps == 24) {
                self.mOutputFunc = &DecoderFlac::outputMonoSamples<int32_t, 8>;
            } else if (bps == 32) {
                self.mOutputFunc = &DecoderFlac::outputMonoSamples<int32_t>;
            } else if (bps == 8) {
                self.mOutputFunc = &DecoderFlac::outputMonoSamples<int8_t, 8>;
            } else {
                ESP_LOGE(TAG, "Unsupported bits per sample: %d", header.bits_per_sample);
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
        } else {
            ESP_LOGE(TAG, "Unsupported number of channels: %d", nChans);
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        int sampleSize = ((bps <= 16) ? 2 : 4) * nChans;
        self.mOutputChunkSize = (nSamples / 2) * sampleSize;
        printf("blockSize: %d, outChunkSize = %d\n", nSamples, self.mOutputChunkSize);
    }
    if ((self.*self.mOutputFunc)(nSamples, buffer) == false) {
        ESP_LOGE(TAG, "Output of FLAC codec is too large to fit into output buffer, aborting decode");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

