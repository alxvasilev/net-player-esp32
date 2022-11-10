#include "decoderMp3.hpp"
#include <mad.h>

static const char* TAG = "mp3dec";

DecoderMp3::DecoderMp3(AudioNode& src): Decoder(src, kCodecMp3)
{
    mInputBuf = (unsigned char*)AudioNode::mallocTrySpiram(kInputBufSize + kOutputBufSize);
    if (!mInputBuf) {
        ESP_LOGE(TAG, "Out of memory allocating I/O buffers");
        abort();
    }
    mOutputBuf = mInputBuf + kInputBufSize;
    initMadState();
    ESP_LOGI(TAG, "Mp3 decoder uses approx %zu bytes of RAM", sizeof(DecoderMp3) + kInputBufSize + kOutputBufSize);
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
    freeMadState();
    initMadState();
    outputFormat.clear();
}

AudioNode::StreamError DecoderMp3::pullData(AudioNode::DataPullReq &odpr)
{
    int dataLen = 0;
    for (;;) {
        AudioNode::DataPullReq idpr(kInputBufSize - dataLen);
        auto err = mSrcNode.pullData(idpr);
        if (err) {
            return err;
        }
        myassert(idpr.size);
        myassert(idpr.size + dataLen <= kInputBufSize);
        memcpy(mInputBuf + dataLen, idpr.buf, idpr.size);
        dataLen += idpr.size;
        mad_stream_buffer(&mMadStream, mInputBuf, dataLen);
        if (mad_frame_decode(&mMadFrame, &mMadStream)) { // returns 0 on success, -1 on error
            mSrcNode.confirmRead(idpr.size);
            if (mMadStream.error == MAD_ERROR_BUFLEN) {
                ESP_LOGI(TAG, "mad_frame_decode: MAD_ERROR_BUFLEN");
                continue;
            } else if (MAD_RECOVERABLE(mMadStream.error)) {
                ESP_LOGI(TAG, "mad_frame_decode: recoverable '%s'", mad_stream_errorstr(&mMadStream));
                dataLen = 0;
                continue;
            } else { // unrecoverable error
                ESP_LOGI(TAG, "mad_frame_decode: Unrecoverable '%s'", mad_stream_errorstr(&mMadStream));
                return AudioNode::kErrDecode;
            }
        } else {
            int consumed = idpr.size - (mMadStream.bufend - mMadStream.next_frame);
            assert(consumed >= 0);
            mSrcNode.confirmRead(consumed);
            ESP_LOGD(TAG, "Successfully decoded frame of size %d\n", mMadStream.next_frame - mMadStream.buffer);
            mad_synth_frame(&mMadSynth, &mMadFrame);
            auto slen = output(mMadSynth.pcm);
            if (slen <= 0) {
                return AudioNode::kErrDecode;
            }
            odpr.buf = (char*)mOutputBuf;
            odpr.size = slen;
            odpr.fmt = outputFormat;
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

    if (!outputFormat.sampleRate()) { // we haven't yet initialized output format info
        outputFormat.setSampleRate(pcmData.samplerate);
        outputFormat.setNumChannels(pcmData.channels);
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
