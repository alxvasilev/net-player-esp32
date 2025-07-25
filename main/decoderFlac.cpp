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
    mNumReads = 0;
}
FLAC__StreamDecoderReadStatus DecoderFlac::readCb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void* userp)
{
//  printf("readCb\n");
    auto& self = *static_cast<DecoderFlac*>(userp);
    if (!self.mInputPacket) {
        auto event = self.mLastInputEvent = self.mSrcNode.pullData(*self.mLastInputPr);
        if (event) {
            *bytes = 0;
            if (event < 0) {
                return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
            }
            else if (event == kEvtStreamChanged || event == kEvtStreamChanged) {
                return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
            }
            else {
                self.mLastInputEvent = kNoError;
                return self.mParent.forwardEvent(*self.mLastInputPr)
                    ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
                    : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
            }
        }
        self.mInputPacket.reset((DataPacket*)self.mLastInputPr->packet.release());
        self.mInputPos = 0;
    }
    auto pktLen = self.mInputPacket->dataLen;
    size_t readAmount = std::min((size_t)pktLen - self.mInputPos, *bytes);
    if (readAmount <= 0) {
        ESP_LOGE(TAG, "Assertion(readAmount <= 0) failed, readAmount = %zu, pktLen = %d, mInputPos = %d, *bytes = %zu\n",
               readAmount, pktLen, self.mInputPos, *bytes);
        abort();
    }
    *bytes = readAmount;
    memcpy(buffer, self.mInputPacket->data + self.mInputPos, readAmount);
    self.mInputPos += readAmount;
    if (self.mInputPos >= pktLen) {
        self.mInputPacket.reset();
        self.mInputPos = 0;
    }
    if (++self.mNumReads > kMaxNumReads) {
        ESP_LOGW(TAG, "Codec did %d reads without producing any output", self.mNumReads);
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
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
template<typename T>
struct RestoreToNull {
    T*& mPtr;
    RestoreToNull(T*& ptr): mPtr(ptr) {}
    ~RestoreToNull() { mPtr = nullptr; }
};

StreamEvent DecoderFlac::decode(AudioNode::PacketResult& dpr)
{
    mLastInputPr = &dpr;
    RestoreToNull<AudioNode::PacketResult> autoNull(mLastInputPr);
    while (mNumReads < kMaxNumReads) {
        mLastInputEvent = kNoError;
        auto ok = FLAC__stream_decoder_process_single(mDecoder);
        if (mLastInputEvent) {
            return mLastInputEvent;
        }
        dpr.clear();
        if (!ok) {
            auto err = FLAC__stream_decoder_get_state(mDecoder);
            const char* errStr = (err >= 0) ? FLAC__StreamDecoderStateString[err] : "(invalid code)";
            ESP_LOGW(TAG, "Decoder returned error %s(%d)", errStr, err);
            return kErrDecode;
        }
    }
    if (mNumReads >= kMaxNumReads) {
        ESP_LOGW(TAG, "Many frames decoded without generating any output, returning error");
        return kErrDecode;
    }
    return kNoError;
}

template <typename T, bool isMono>
bool DecoderFlac::outputSamples(int nSamples, const FLAC__int32* const channels[])
{
    auto ch0 = channels[0];
    auto ch1 = channels[isMono ? 0 : 1]; // in mono mode, output the same channel on both left and right
    int nPackets, defltPktSamples;
    if ((nSamples & 0x3ff) == 0) {
        nPackets = nSamples >> 10;
        defltPktSamples = 1024;
    }
    else {
        nPackets = (nSamples + 512) / 1024;
        defltPktSamples = nSamples / nPackets;
    }
    int defltPktAlloc = defltPktSamples * 8;
    int defltPktLen = defltPktSamples * 2 * sizeof(T);
    int remainSamples = nSamples;
    int sidx = 0;
    while(remainSamples > 0) {
        int pktSamples, pktAlloc, pktLen;
        if (remainSamples >= defltPktSamples) {
            pktSamples = defltPktSamples;
            pktAlloc = defltPktAlloc;
            pktLen = defltPktLen;
        }
        else {
            pktSamples = remainSamples;
            pktAlloc = pktSamples * 8;
            pktLen = pktSamples * 2 * sizeof(T);
        }
        DataPacket::unique_ptr output(DataPacket::create(pktAlloc));
        output->flags |= StreamPacket::kFlagHasSpaceFor32Bit;
        T* wptr = (T*)output->data;
        int eidx = sidx + pktSamples;
        for (; sidx < eidx; sidx++) {
            *(wptr++) = ch0[sidx];
            *(wptr++) = ch1[sidx];
        }
        output->dataLen = pktLen;
        remainSamples -= pktSamples;
        if (!mParent.codecPostOutput(output.release())) {
            return false;
        }
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
    self.mNumReads = 0;
    auto bps = header.bits_per_sample;
    auto oldFmt = self.outputFormat;
    auto& fmt = self.outputFormat;
    fmt.setNumChannels(nChans);
    fmt.setBitsPerSample(bps);
    fmt.setSampleRate(header.sample_rate);
    if (fmt != oldFmt) {
        ESP_LOGI(TAG, "Output format is %lu-bit, %.1fkHz %s", bps, (float)fmt.sampleRate() / 1000, (nChans == 2) ? "stereo" : "mono");
        if (!self.selectOutputFunc(nChans, bps)) {
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        self.mParent.codecOnFormatDetected(self.outputFormat, bps);
    }
    if ((self.*self.mOutputFunc)(nSamples, buffer) == false) {
        ESP_LOGE(TAG, "output: ringbuf aborted, aborting decode");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
bool DecoderFlac::selectOutputFunc(int nChans, int bps)
{
    if (nChans == 2) {
        if (bps == 16) {
            mOutputFunc = &DecoderFlac::outputSamples<int16_t, false>;
        } else if (bps == 24 || bps == 32) {
            mOutputFunc = &DecoderFlac::outputSamples<int32_t, false>;
        } else {
            ESP_LOGE(TAG, "Unsupported bits per sample: %d", bps);
            return false;
        }
    }
    else if (nChans == 1) {
        if (bps == 16) {
            mOutputFunc = &DecoderFlac::outputSamples<int16_t, true>;
        } else if (bps == 24 || bps == 32) {
            mOutputFunc = &DecoderFlac::outputSamples<int32_t, true>;
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

