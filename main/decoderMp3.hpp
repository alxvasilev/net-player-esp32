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
    void initMadState();
    void freeMadState();
    void logEncodingInfo();
public:
    virtual CodecType type() const { return kCodecMp3; }
    DecoderMp3();
    ~DecoderMp3();
    virtual int inputBytesNeeded();
    virtual int decode(const char* buf, int size);
    virtual char* outputBuf() { return mOutputBuf; }
    virtual void reset();
};

#endif
