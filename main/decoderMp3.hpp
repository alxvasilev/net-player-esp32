#ifndef DECODER_MP3_HPP
#define DECODER_MP3_HPP
#include "decoderNode.hpp"
#include <mad.h>

class DecoderMp3: public Decoder
{
protected:
    enum {
        kInputBufSize = 3000,
        kSamplesPerFrame = 1152,
        kOutputBufSize = kSamplesPerFrame * 4 // each mp3 packet decodes to 1152 samples, for 16 bit stereo, multiply by 4
    };
    struct mad_stream mMadStream;
    struct mad_frame mMadFrame;
    struct mad_synth mMadSynth;
    char mInputBuf[kInputBufSize];
    int mInputLen = 0;
    char mOutputBuf[kOutputBufSize];
    bool initStreamFormat(mad_header& header);
    int output(const mad_pcm& pcm);
    void doReset();
    void logEncodingInfo();
public:
    virtual esp_codec_type_t type() const { return ESP_CODEC_TYPE_MP3; }
    DecoderMp3();
    uint16_t scale(mad_fixed_t sample) {
        auto isNeg = sample & 0x80000000;
        sample >>= (29 - 15);
        return (isNeg) ? ((sample & 0xffff) | 0x8000)
                       :  (sample & 0x7fff);
    }
    virtual int inputBytesNeeded();
    virtual int decode(const char* buf, int size);
    virtual char* outputBuf() { return mOutputBuf; }
    virtual void reset();
};

#endif
