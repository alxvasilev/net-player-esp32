#ifndef DECODER_AAC_HPP
#define DECODER_AAC_HPP

#include "decoderNode.hpp"
#include <aacdec.h>

class DecoderAac: public Decoder
{
protected:
    enum { kInputBufSize = 2048, kOutputBufSize = 2 *
#ifdef HELIX_FEATURE_AUDIO_CODEC_AAC_SBR
           2048
#else
           1024
#endif
    };
    HAACDecoder mDecoder;
    unsigned char* mInputBuf;
    int mInputLen = 0;
    int mNextFrameOffs = 0;
    int mOutputSize = 0;
    int16_t mOutputBuf[kOutputBufSize];
    void initDecoder();
    void freeDecoder();
    void getStreamFormat();
public:
    virtual CodecType type() const { return kCodecAac; }
    DecoderAac();
    ~DecoderAac();
    virtual int inputBytesNeeded();
    virtual int decode(const char* buf, int size);
    virtual char* outputBuf() { return (char*)mOutputBuf; }
    virtual void reset();
};

#endif
