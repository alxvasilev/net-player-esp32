#ifndef DECODER_NODE_HPP
#define DECODER_NODE_HPP
#include "audioNode.hpp"

class Decoder
{
protected:
    AudioNode& mSrcNode;
public:
    StreamFormat outputFormat;
    Decoder(AudioNode& src, CodecType codec): mSrcNode(src), outputFormat(codec) {}
    virtual ~Decoder() {}
    virtual CodecType type() const = 0;
    virtual AudioNode::StreamError pullData(AudioNode::DataPullReq& output) = 0;
    virtual void reset() = 0;
};
/*
class CodecDetector
{
public:
    // @returns if >= 0, it's a CodecType, if < 0, it's a StreamError
    static int16_t detectCodec(AudioNode& src, int timeout);
};
*/
class DecoderNode: public AudioNode
{
protected:
    Decoder* mDecoder = nullptr;
    AudioNode::StreamError detectCodecCreateDecoder(CodecType type);
    bool createDecoder(CodecType type);
public:
    DecoderNode(IAudioPipeline& parent): AudioNode(parent, "decoder"){}
    virtual Type type() const { return kTypeDecoder; }
    virtual StreamError pullData(DataPullReq& dpr);
    virtual void confirmRead(int size) {}
    virtual ~DecoderNode() {}
    friend class Decoder;
};

#endif
