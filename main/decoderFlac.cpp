#include "decoderFlac.hpp"

static const char* TAG = "flac";

DecoderFlac::DecoderFlac(DecoderNode& parent, AudioNode& src, bool isOgg)
    : Decoder(parent, src, Codec(Codec::kCodecFlac,
        isOgg ? Codec::kTransportOgg : Codec::kTransportDefault))
{
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
}
void DecoderFlac::reset()
{
    ESP_LOGI(TAG, "Resetting decoder");
    outputFormat.clear();
    FLAC__stream_decoder_reset(mDecoder);
    mInputPos = 0;
    mInputPacket.reset();
}
FLAC__StreamDecoderReadStatus DecoderFlac::readCb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void* userp)
{
//  printf("readCb\n");
    auto& self = *static_cast<DecoderFlac*>(userp);
    assert(self.mInputPr);
    auto& pr = *self.mInputPr;
    if (!self.mInputPacket) {
        auto event = self.mInputEvent = self.mSrcNode.pullData(pr);
        if (event) {
            *bytes = 0;
            return (event == kEvtStreamEnd)
                    ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
                    : FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        else {
            self.mInputPacket.reset((DataPacket*)pr.packet.release());
            self.mInputPos = 0;
        }
    }
    auto pktLen = self.mInputPacket->dataLen;
    size_t readAmount = std::min((size_t)pktLen - self.mInputPos, *bytes);
    myassert(readAmount > 0);
    *bytes = readAmount;
    memcpy(buffer, self.mInputPacket->data + self.mInputPos, readAmount);
    self.mInputPos += readAmount;
    if (self.mInputPos >= pktLen) {
        self.mInputPacket.reset();
        self.mInputPos = 0;
    }
    if (++self.mNumReads > 30) {
        ESP_LOGW(TAG, "Codec did %d reads without producing an output", self.mNumReads);
    }
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
void DecoderFlac::errorCb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    ESP_LOGE(TAG, "FLAC decode error: %s(%d)", FLAC__StreamDecoderErrorStatusString[status], status);
}
void DecoderFlac::metadataCb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
}
StreamEvent DecoderFlac::decode(AudioNode::PacketResult& dpr)
{
    mInputPr = &dpr;
    mHasOutput = false;
    for (int i = 0; !mHasOutput && (i < 10); i++) {
        mNumReads = 0;
        auto ok = FLAC__stream_decoder_process_single(mDecoder);
        if (!ok) {
            auto err = FLAC__stream_decoder_get_state(mDecoder);
            const char* errStr = (err >= 0) ? FLAC__StreamDecoderStateString[err] : "(invalid code)";
            ESP_LOGW(TAG, "Decoder returned error %s(%d)", errStr, err);
            return mInputEvent ? mInputEvent : kErrDecode;
        }
        if (mInputEvent) { //FLAC__stream_decoder_get_state(mDecoder) == FLAC__STREAM_DECODER_END_OF_STREAM) {
            return mInputEvent;
        }
    }
    mInputPr = nullptr; // we don't want a dangling invalid pointer, even if it's not used
    if (!mHasOutput) {
        ESP_LOGW(TAG, "Many frames decoded without generating any output, returning error");
        dpr.clear();
        return kErrDecode;
    }
    return kNoError;
}

template <typename T, int Shift = 0>
bool DecoderFlac::outputStereoSamples(int nSamples, const FLAC__int32* const channels[])
{
    auto ch0 = channels[0];
    auto ch1 = channels[1];
    if (nSamples & 1) {
        ESP_LOGW(TAG, "Uneven number of output samples: %d\n", nSamples);
    }
    int pktNsamples = nSamples > kOutputSplitMaxSamples ? nSamples >> 1 : nSamples;
    auto outputLen = pktNsamples * sizeof(T) * 2;
    DataPacket::unique_ptr output(DataPacket::create(outputLen));
    T* wptr = (T*)output->data;
    for (int i = 0; i < pktNsamples; i++) {
        *(wptr++) = ch0[i];
        *(wptr++) = ch1[i];
    }
    mParent.codecPostOutput(output.release());
    if (pktNsamples < nSamples) {
        output.reset(DataPacket::create(outputLen));
        wptr = (T*)output->data;
        for (int i = pktNsamples; i < nSamples; i++) {
            *(wptr++) = ch0[i];
            *(wptr++) = ch1[i];
        }
        mParent.codecPostOutput(output.release());
    }
    return true;
}
template <typename T, int Shift=0>
bool DecoderFlac::outputMonoSamples(int nSamples, const FLAC__int32* const samples[])
{
    if (nSamples & 1) {
        ESP_LOGW(TAG, "Uneven number of output samples: %d\n", nSamples);
    }
    int pktNsamples = nSamples > kOutputSplitMaxSamples ? nSamples >> 1 : nSamples;
    int outputLen = pktNsamples * sizeof(T);
    DataPacket::unique_ptr output(DataPacket::create(outputLen));
    T* wptr = (T*)output->data;
    auto end = samples[0] + pktNsamples;
    for (auto sample = samples[0]; sample < end;) {
        *(wptr++) = *(sample++);
    }
    output->dataLen = outputLen;
    mParent.codecPostOutput(output.release());
    if (pktNsamples < nSamples) {
        output.reset(DataPacket::create(pktNsamples));
        wptr = (T*)output->data;
        auto end2 = samples[0] + nSamples;
        for (auto sample = end; sample < end2;) {
            *(wptr++) = *(sample++);
        }
        output->dataLen = outputLen;
        mParent.codecPostOutput(output.release());
    }
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
        if (!self.selectOutputFunc(nChans, bps)) {
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        self.mParent.codecOnFormatDetected(self.outputFormat, self.outputFormat.bitsPerSample());
    }
    self.mHasOutput = true;
    if ((self.*self.mOutputFunc)(nSamples, buffer) == false) {
        ESP_LOGE(TAG, "Output of FLAC codec is too large to fit into output buffer, aborting decode");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
bool DecoderFlac::selectOutputFunc(int nChans, int bps)
{
    if (nChans == 2) {
        if (bps == 16) {
            mOutputFunc = &DecoderFlac::outputStereoSamples<int16_t>;
        } else if (bps == 24) {
            mOutputFunc = &DecoderFlac::outputStereoSamples<int32_t, 8>;
        } else if (bps == 32) {
            mOutputFunc = &DecoderFlac::outputStereoSamples<int32_t>;
        }else if (bps == 8) {
            mOutputFunc = &DecoderFlac::outputStereoSamples<int16_t, 8>;
        } else {
            ESP_LOGE(TAG, "Unsupported bits per sample: %d", bps);
            return false;
        }
    }
    else if (nChans == 1) {
        if (bps == 16) {
            mOutputFunc = &DecoderFlac::outputMonoSamples<int16_t>;
        } else if (bps == 24) {
            mOutputFunc = &DecoderFlac::outputMonoSamples<int32_t, 8>;
        } else if (bps == 32) {
            mOutputFunc = &DecoderFlac::outputMonoSamples<int32_t>;
        } else if (bps == 8) {
            mOutputFunc = &DecoderFlac::outputMonoSamples<int8_t, 8>;
        } else {
            ESP_LOGE(TAG, "Unsupported bits per sample: %d", bps);
            return false;
        }
    } else {
        ESP_LOGE(TAG, "Unsupported number of channels: %d", nChans);
        return false;
    }
    return true;
}

