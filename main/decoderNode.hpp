#ifndef DECODER_NODE_HPP
#define DECODER_NODE_HPP
#include "audioNode.hpp"

class DecoderNode;
class Decoder
{
protected:
    DecoderNode& mParent;
    AudioNode& mSrcNode;
    std::unique_ptr<StreamDataItem> mCurrentChunk;
public:
    StreamFormat outputFormat;
    Decoder(DecoderNode& parent, AudioNode& src, std::unique_ptr<StreamItem>& firstChunk)
    : mParent(parent), mSrcNode(src), mCurrentChunk((StreamDataItem*)firstChunk.release()) {}
    virtual ~Decoder() {}
    virtual Codec::Type type() const = 0;
    virtual StreamError pullData(std::unique_ptr<StreamItem>& item) = 0;
    virtual void reset() = 0;
};
/*
class CodecDetector
{
public:
    // @returns if >= 0, it's a CodecType, if < 0, it's a StreamEvent
    static int16_t detectCodec(AudioNode& src, int timeout);
};
*/
class DecoderNode: public AudioNode
{
protected:
    Decoder* mDecoder = nullptr;
    bool mStartingNewStream = true;
    // odp in case there is a stream event that needs to be propagated
    bool detectCodecCreateDecoder(std::unique_ptr<StreamItem>& firstChunk);
    bool createDecoder(StreamFormat fmt, std::unique_ptr<StreamItem>& firstChunk);
    static int32_t heapFreeTotal(); // used to  calculate memory usage for codecs
    void deleteDecoder();
public:
    enum { kEventCodecChange = AudioNode::kEventLast + 1 };
    DecoderNode(IAudioPipeline& parent): AudioNode(parent, "decoder"){}
    virtual Type type() const { return kTypeDecoder; }
    virtual bool pullData(std::unique_ptr<StreamItem>& item);
    virtual ~DecoderNode() { deleteDecoder(); }
    virtual void reset() override { deleteDecoder(); }
    friend class Decoder;
};

#endif
