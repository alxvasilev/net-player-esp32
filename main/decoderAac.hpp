#ifndef DECODER_AAC_HPP
#define DECODER_AAC_HPP

#include "decoderNode.hpp"
#include <aacdec.h>

class DecoderAac: public Decoder
{
protected:
    enum { kInputBufSize = 1024, kMinAllowedAacInputSize = 400, kOutputBufSize = 2 * 2 *
#ifdef HELIX_FEATURE_AUDIO_CODEC_AAC_SBR
           2048
#else
           1024
#endif
    };
    HAACDecoder mDecoder;
    unsigned char* mInputBuf;
    unsigned char* mNextFramePtr;
    int16_t* mOutputBuf;
    int mInputLen;
    int mOutputLen;
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
