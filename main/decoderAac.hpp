#ifndef DECODER_AAC_HPP
#define DECODER_AAC_HPP

#include "decoderNode.hpp"
#include <aacdec.h>

class DecoderAac: public Decoder
{
protected:
    enum { kInputBufSize = 2048, kOutputBufSize = 2 * 2 *
#ifdef HELIX_FEATURE_AUDIO_CODEC_AAC_SBR
           2048
#else
           1024
#endif
    };
    HAACDecoder mDecoder;
    unsigned char* mInputBuf;
    int16_t* mOutputBuf;
    int mOutputSize = 0;
    void initDecoder();
    void freeDecoder();
    void getStreamFormat();
public:
    virtual CodecType type() const { return kCodecAac; }
    DecoderAac(AudioNode& src);
    ~DecoderAac();
    virtual AudioNode::StreamError pullData(AudioNode::DataPullReq& output);
    virtual void reset() override;
};

#endif
