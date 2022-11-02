#ifndef DECODER_FLAC_HPP
#define DECODER_FLAC_HPP
#include "decoderNode.hpp"

struct fx_flac;
typedef struct fx_flac fx_flac_t;

class DecoderFlac: public Decoder
{
protected:
    enum {
        kMaxSamplesPerBlock = 4608,
        kInputBufSize = 30000,
        kOutputBufSize = 38000 // actually 37328
    };

    uint8_t* mInputBuf;
    int32_t* mOutputBuf;
    int mInputLen = 0;
    fx_flac_t* mFlacDecoder;
    void init();
    void convertOutput(size_t nSamples);
public:
    virtual CodecType type() const { return kCodecFlac; }
    DecoderFlac();
    ~DecoderFlac();
    virtual int inputBytesNeeded();
    virtual int decode(const char* buf, int size);
    virtual char* outputBuf() { return (char*)mOutputBuf; }
    virtual void reset();
};

#endif
