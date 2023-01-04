#ifndef DECODER_AAC_HPP
#define DECODER_AAC_HPP

#include "decoderNode.hpp"

typedef void *HAACDecoder;
class DecoderAac: public Decoder
{
protected:
    enum { kInputBufSize = 2048, kMinAllowedAacInputSize = 1024, kOutputBufSize = 2 * 2 *
           2048 // 1024 if no SBR support, i.e. HELIX_FEATURE_AUDIO_CODEC_AAC_SBR not defined
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
    DecoderAac(DecoderNode& parent, AudioNode& src);
    ~DecoderAac();
    virtual AudioNode::StreamError pullData(AudioNode::DataPullReq& output);
    virtual void reset() override;
};

#endif
