#ifndef DECODER_NODE_HPP
#define DECODER_NODE_HPP
#include "audioNode.hpp"

class DecoderNode;
class Decoder
{
protected:
    DecoderNode& mParent;
    AudioNode& mSrcNode;
public:
    StreamFormat outputFormat;
    Decoder(DecoderNode& parent, AudioNode& src)
    : mParent(parent), mSrcNode(src) {}
    virtual ~Decoder() {}
    virtual Codec::Type type() const = 0;
    virtual AudioNode::StreamError pullData(AudioNode::DataPullReq& output) = 0;
    virtual void confirmRead(int size) {}
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
    bool mStartingNewStream = true;
    // odp in case there is a stream event that needs to be propagated
    AudioNode::StreamError detectCodecCreateDecoder(DataPullReq& odp);
    bool createDecoder(StreamFormat fmt);
    static int32_t heapFreeTotal(); // used to  calculate memory usage for codecs
    void deleteDecoder();
public:
    enum { kEventCodecChange = AudioNode::kEventLast + 1 };
    DecoderNode(IAudioPipeline& parent): AudioNode(parent, "decoder"){}
    virtual Type type() const { return kTypeDecoder; }
    virtual StreamError pullData(DataPullReq& dpr);
    virtual void confirmRead(int size);
    virtual ~DecoderNode() { deleteDecoder(); }
    virtual void reset() override { deleteDecoder(); }
    friend class Decoder;
};

#endif
