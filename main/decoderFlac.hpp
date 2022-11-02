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
        kInputBufSize = kMaxSamplesPerBlock * 4,
        kOutputBufSize = kMaxSamplesPerBlock * 4
    };

    uint8_t* mInputBuf;
    int16_t* mOutputBuf;
    int mInputLen = 0;
    fx_flac_t* mFlacDecoder;
    void init();
    int getOutput();
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
