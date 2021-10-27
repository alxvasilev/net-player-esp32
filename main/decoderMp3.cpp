#include "decoderMp3.hpp"
#include <mad.h>

static const char* TAG = "mp3dec";

DecoderMp3::DecoderMp3()
{
    initMadState();
    ESP_LOGI(TAG, "Mp3 decoder uses approx %zu bytes of RAM", sizeof(DecoderMp3));
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
    mInputLen = 0;
    freeMadState();
    initMadState();
    mOutputFormat.reset();
}

int DecoderMp3::inputBytesNeeded()
{
    return sizeof(mInputBuf) - mInputLen;
}

int DecoderMp3::decode(const char* buf, int size)
{
    if (buf) {
        myassert(mInputLen + size <= kInputBufSize);
        memcpy(mInputBuf+mInputLen, buf, size);
        mInputLen += size;
    }
    mad_stream_buffer(&mMadStream, (const unsigned char*)mInputBuf, mInputLen);
    for(;;) {
        auto ret = mad_frame_decode(&mMadFrame, &mMadStream);
        if (ret) {
            if (mMadStream.error == MAD_ERROR_BUFLEN) {
                ESP_LOGI(TAG, "mad_frame_decode: MAD_ERROR_BUFLEN");
                return AudioNode::kNeedMoreData;
            } else if (MAD_RECOVERABLE(mMadStream.error)) {
                ESP_LOGI(TAG, "mad_frame_decode: recoverable '%s'", mad_stream_errorstr(&mMadStream));
                continue;
            } else { // unrecoverable error
                ESP_LOGI(TAG, "mad_frame_decode: UNrecoverable '%s'", mad_stream_errorstr(&mMadStream));
                return AudioNode::kErrDecode;
            }
        }
        mInputLen = mMadStream.bufend - mMadStream.next_frame;
        if (mInputLen) {
            memmove(mInputBuf, mMadStream.next_frame, mInputLen);
        }
        ESP_LOGD(TAG, "Successfully decoded frame of size %d\n", mMadStream.next_frame - mMadStream.buffer);
        mad_synth_frame(&mMadSynth, &mMadFrame);
        auto slen = output(mMadSynth.pcm);
        return (slen <= 0) ? (int)AudioNode::kErrDecode : slen;
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
    ESP_LOGW(TAG, "MPEG1 Layer %d, 16-bit %s, %d Hz, %lu bps",
        mMadFrame.header.layer, stmode, mMadFrame.header.samplerate,
        mMadFrame.header.bitrate);
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

    if (!mOutputFormat.samplerate) { // we haven't yet initialized output format info
        mOutputFormat.codec = kCodecMp3;
        mOutputFormat.samplerate = pcmData.samplerate;
        mOutputFormat.setChannels(pcmData.channels);
        mOutputFormat.setBits(16);
        logEncodingInfo();
    }

    if (pcmData.channels == 2) {
        auto left_ch = pcmData.samples[0];
        auto right_ch = pcmData.samples[1];
        int n = 0;
        for (char* sbytes = mOutputBuf; n < nsamples; n++) {
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
        for (char* sbytes = mOutputBuf; n < nsamples;) {
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
