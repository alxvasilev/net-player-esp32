#ifndef DECODER_MP3_HPP
#define DECODER_MP3_HPP
#include "decoderNode.hpp"
#include <flac.h>

class DecoderFlac: public Decoder
{
protected:
    enum {
        kMaxSamplesPerBlock = 4608,
        kInputBufSize = 30000,
        kOutputBufSize = 38000 // actually 37328
    };

    char* mInputBuf;
    int32_t* mOutputBuf;
    int mInputLen = 0;
    fx_flac_t* mFlacDecoder;
    void convertOutput(size_t nSamples);
public:
    virtual CodecType type() const { return kCodecFlac; }
    DecoderFlac();
    ~DecoderFlac();
    virtual int inputBytesNeeded();
    virtual int decode(const char* buf, int size);
    virtual char* outputBuf() { return mOutputBuf; }
    virtual void reset();
};

#endif