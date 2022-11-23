#include "decoderMp3.hpp"
#include <mad.h>

static const char* TAG = "mp3dec";

DecoderMp3::DecoderMp3(AudioNode& src): Decoder(src, kCodecMp3)
{
    mInputBuf = (unsigned char*)utils::mallocTrySpiram(kInputBufSize + kOutputBufSize);
    if (!mInputBuf) {
        ESP_LOGE(TAG, "Out of memory allocating I/O buffers");
        abort();
    }
    mOutputBuf = mInputBuf + kInputBufSize;
    initMadState();
}
DecoderMp3::~DecoderMp3()
{
    freeMadState();
    free(mInputBuf);
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

AudioNode::StreamError DecoderMp3::pullData(AudioNode::DataPullReq &dpr)
{
    bool needMoreData = !mMadStream.buffer;
    for (;;) {
        if (needMoreData) {
            needMoreData = false;
            auto currLen = mMadStream.bufend - mMadStream.next_frame;
            if (currLen > 0) {
                memmove(mInputBuf, mMadStream.next_frame, currLen);
            }
            auto reqSize = kInputBufSize - currLen;
            if (reqSize <= 0) {
                ESP_LOGE(TAG, "Input buffer full, but can't decode frame");
                return AudioNode::kErrDecode;
            }
            dpr.size = reqSize;
            auto event = mSrcNode.pullData(dpr);
            if (event) {
                return event;
            }
            myassert(dpr.size && dpr.size <= reqSize);
            mSrcNode.confirmRead(dpr.size);
            memcpy(mInputBuf + currLen, dpr.buf, dpr.size);
            mad_stream_buffer(&mMadStream, mInputBuf, currLen + dpr.size);
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
                return AudioNode::kErrDecode;
            }
        } else {
            ESP_LOGD(TAG, "Successfully decoded frame of size %d\n", mMadStream.next_frame - mMadStream.buffer);
            mad_synth_frame(&mMadSynth, &mMadFrame);
            auto slen = output(mMadSynth.pcm);
            if (slen <= 0) {
                return AudioNode::kErrDecode;
            }
            dpr.buf = (char*)mOutputBuf;
            dpr.size = slen;
            dpr.fmt = outputFormat;
            return AudioNode::kNoError;
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

static inline uint16_t scale(mad_fixed_t sample) {
    auto isNeg = sample & 0x80000000;
    sample >>= (29 - 15);
    return isNeg ? (sample | 0x8000) : (sample & 0x7fff);
}

int DecoderMp3::output(const mad_pcm& pcmData)
{
    int nsamples = pcmData.length;
    if (nsamples > kSamplesPerFrame) {
        ESP_LOGW(TAG, "Too many samples %d decoded from frame, insufficient space in output buffer", nsamples);
    }

    if (!outputFormat.sampleRate()) { // we haven't yet initialized output format info
        auto sr = pcmData.samplerate;
        if (sr != 11025 && sr != 22050 && sr != 44100 && sr != 48000) {
            ESP_LOGE(TAG, "Invalid/unsupported sample rate: %d\n", sr);
            return AudioNode::kErrDecode;
        }
        outputFormat.setSampleRate(sr);
        auto chans = pcmData.channels;
        if (chans > 2) {
            ESP_LOGE(TAG, "Too many channels: %d", chans);
            return AudioNode::kErrDecode;
        }
        outputFormat.setNumChannels(chans);
        outputFormat.setBitsPerSample(16);
        logEncodingInfo();
    }

    if (pcmData.channels == 2) {
        auto left_ch = pcmData.samples[0];
        auto right_ch = pcmData.samples[1];
        int n = 0;
        for (unsigned char* sbytes = mOutputBuf; n < nsamples; n++) {
            uint16_t sample = scale(*left_ch++);
            *(sbytes++) = (sample & 0xff);
            *(sbytes++) = ((sample >> 8) & 0xff);

            sample = scale(*right_ch++);
            *(sbytes++) = (sample & 0xff);
            *(sbytes++) = ((sample >> 8) & 0xff);
        }
        return nsamples * 4;
    } else if (pcmData.channels == 1) {
        auto samples = pcmData.samples[0];
        int n = 0;
        for (unsigned char* sbytes = mOutputBuf; n < nsamples;) {
            uint32_t sample = scale(*samples++);
            *(sbytes++) = (sample & 0xff);
            *(sbytes++) = ((sample >> 8) & 0xff);
        }
        return nsamples * 2;
    } else {
        ESP_LOGE(TAG, "Unsupported number of channels %d", pcmData.channels);
        return AudioNode::kErrDecode;
    }
}
