#include "decoderMp3.hpp"
#include <mad.h>

static const char* TAG = "mp3dec";

DecoderMp3::DecoderMp3(DecoderNode& parent, AudioNode& src): Decoder(parent, src)
{
    initMadState();
}
DecoderMp3::~DecoderMp3()
{
    freeMadState();
}
void DecoderMp3::initMadState()
{
    mad_stream_init(&mMadStream);
    mad_synth_init(&mMadSynth);
    mad_frame_init(&mMadFrame);
}
void DecoderMp3::freeMadState()
{
    mad_stream_finish(&mMadStream);
    mad_synth_finish(&mMadSynth);
    mad_frame_finish(&mMadFrame);
}

void DecoderMp3::reset()
{
    mad_stream_init(&mMadStream);
    freeMadState();
    initMadState();
    outputFormat.clear();
}

StreamEvent DecoderMp3::decode(AudioNode::PacketResult& dpr)
{
    bool needMoreData = !mMadStream.buffer;
    for (;;) {
        if (needMoreData) {
            needMoreData = false;
            auto currLen = mMadStream.bufend - mMadStream.next_frame;
            if (currLen > 0) {
                memmove(mInputBuf, mMadStream.next_frame, currLen);
            }
            auto freeSpace = kInputBufSize - currLen;
            if (!freeSpace) {
                ESP_LOGE(TAG, "Input buffer full, but can't decode frame");
                return kErrDecode;
            }
            auto event = mSrcNode.pullData(dpr);
            if (event) {
                return event;
            }
            auto& dataPacket = dpr.dataPacket();
            if (dataPacket.dataLen > freeSpace) {
                ESP_LOGE(TAG, "Input packet is larger than the free space in the staging buffer");
                return kErrDecode;
            }
            memcpy(mInputBuf + currLen, dataPacket.data, dataPacket.dataLen);
            mad_stream_buffer(&mMadStream, mInputBuf, currLen + dataPacket.dataLen);
        }
        auto ret = mad_frame_decode(&mMadFrame, &mMadStream);
        if (ret) { // returns 0 on success, -1 on error
            if (mMadStream.error == MAD_ERROR_BUFLEN) {
             // ESP_LOGI(TAG, "mad_frame_decode: MAD_ERROR_BUFLEN");
                needMoreData = true;
                continue;
            } else if (MAD_RECOVERABLE(mMadStream.error)) {
                ESP_LOGW(TAG, "mad_frame_decode: recoverable '%s'", mad_stream_errorstr(&mMadStream));
                continue;
            } else { // unrecoverable error
                ESP_LOGW(TAG, "mad_frame_decode: Unrecoverable '%s'", mad_stream_errorstr(&mMadStream));
                return kErrDecode;
            }
        }
        else {
            ESP_LOGD(TAG, "Successfully decoded frame of size %d\n", mMadStream.next_frame - mMadStream.buffer);
            mad_synth_frame(&mMadSynth, &mMadFrame);
            return output(mMadSynth.pcm);
        }
    }
}
void DecoderMp3::logEncodingInfo()
{
    const char* stmode;
    switch (mMadFrame.header.mode) {
        case MAD_MODE_SINGLE_CHANNEL: stmode = "mono"; break;
        case MAD_MODE_DUAL_CHANNEL: stmode = "dual-channel stereo"; break;
        case MAD_MODE_JOINT_STEREO: stmode = "joint stereo"; break;
        case MAD_MODE_STEREO: stmode = "stereo"; break;
        default: stmode = "unknown"; break;
    }
    ESP_LOGW(TAG, "MPEG1 Layer %d, 16-bit %s, %.1fkHz, %lukbps",
        mMadFrame.header.layer, stmode, (float)mMadFrame.header.samplerate / 1000,
        mMadFrame.header.bitrate / 1000);
}

StreamEvent DecoderMp3::output(const mad_pcm& pcmData)
{
    int nsamples = pcmData.length;
    if (!outputFormat.sampleRate()) { // we haven't yet initialized output format info
        outputFormat.setCodec(Codec::kCodecMp3);
        auto sr = pcmData.samplerate;
        if (sr != 11025 && sr != 22050 && sr != 44100 && sr != 48000) {
            ESP_LOGE(TAG, "Invalid/unsupported sample rate: %d\n", sr);
            return kErrDecode;
        }
        outputFormat.setSampleRate(sr);
        auto chans = pcmData.channels;
        if (chans > 2) {
            ESP_LOGE(TAG, "Too many channels: %d", chans);
            return kErrDecode;
        }
        outputFormat.setNumChannels(chans);
        outputFormat.setBitsPerSample(24);
        logEncodingInfo();
        if (!mParent.codecOnFormatDetected(outputFormat, 16)) {
            return kErrStreamStopped;
        }
    }
    DataPacket::unique_ptr output;
    // 24 bit output
    if (pcmData.channels == 2) {
        int dataLen = nsamples * 8;
        output.reset(DataPacket::create(dataLen));
        auto left_ch = pcmData.samples[0];
        auto right_ch = pcmData.samples[1];
        int32_t* end = (int32_t*)(output->data + dataLen);
        for (int32_t* wptr = (int32_t*)output->data; wptr < end; ) {
            *(wptr++) = *(left_ch++) >> 6;
            *(wptr++) = *(right_ch++) >> 6;
        }
    }
    else if (pcmData.channels == 1) {
        int dataLen = nsamples * 4;
        output.reset(DataPacket::create(dataLen));
        auto rptr = pcmData.samples[0];
        auto end = (int32_t*)(output->data + dataLen);
        for (int32_t* wptr = (int32_t*)output->data; wptr < end; wptr++) {
            *(wptr++) = *(rptr++) >> 6;
        }
    }
    else {
        ESP_LOGE(TAG, "Unsupported number of channels %d", pcmData.channels);
        return kErrDecode;
    }
    return mParent.codecPostOutput(output.release()) ? kEvtData : kErrStreamStopped;
}
