#ifndef DECODER_MP3_HPP
#define DECODER_MP3_HPP
#include "decoderNode.hpp"
#include <mad.h>

class DecoderMp3: public Decoder
{
protected:
    enum {
        kInputBufSize = 4096,
        kSamplesPerFrame = 1152,
        kOutputFrameSize = kSamplesPerFrame * 4 // each mp3 packet decodes to 1152 samples, for 16 bit stereo, multiply by 4
    };
    struct mad_stream mMadStream;
    struct mad_frame mMadFrame;
    struct mad_synth mMadSynth;
    unsigned char mInputBuf[kInputBufSize];
    int mInputLen = 0;
    StreamEvent output(const mad_pcm& pcm);
    void initMadState();
    void freeMadState();
    void logEncodingInfo();
    void reset();
public:
    virtual Codec::Type type() const { return Codec::kCodecMp3; }
    DecoderMp3(DecoderNode& parent, AudioNode& src);
    virtual ~DecoderMp3();
    virtual StreamEvent decode(AudioNode::PacketResult& dpr);
};

#endif
