#ifndef DECODER_AAC_HPP
#define DECODER_AAC_HPP

#include "decoderNode.hpp"

typedef void *HAACDecoder;
class DecoderAac: public Decoder
{
protected:
    enum { kInputBufSize = 4096, kMinAllowedAacInputSize = 1024, kOutputMaxSize = 2 * 4 *
           2048 // 1024 if no SBR support, i.e. HELIX_FEATURE_AUDIO_CODEC_AAC_SBR not defined
    };
    HAACDecoder mDecoder;
    unsigned char mInputBuf[kInputBufSize];
    unsigned char* mNextFramePtr;
    int mInputLen;
    int mOutputLen;
    void initDecoder();
    void freeDecoder();
    void getStreamFormat();
public:
    virtual Codec::Type type() const { return Codec::kCodecAac; }
    DecoderAac(DecoderNode& parent, AudioNode& src);
    ~DecoderAac();
    virtual StreamEvent decode(AudioNode::PacketResult& pr);
    virtual void reset() override;
};

#endif
