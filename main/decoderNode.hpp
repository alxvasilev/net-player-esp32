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
    uint16_t mode = 0;
    CodecType codec;
    Decoder(DecoderNode& parent, AudioNode& src, CodecType aCodec)
    : mParent(parent), mSrcNode(src), codec(aCodec) {}
    virtual ~Decoder() {}
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
    bool mStartingNewStream = true;
    char* mStreamHdr = nullptr;
    int16_t mStreamHdrLen = 0;
    int16_t mStreamHdrReadPos = 0;
    // odp in case there is a stream event that needs to be propagated
    AudioNode::StreamError detectCodecCreateDecoder(DataPullReq& odp);
    bool createDecoder(DataPullReq& info);
    static int32_t heapFreeTotal(); // used to  calculate memory usage for codecs
    void deleteDecoder();
    void freeStreamHdrBuf();
public:
    enum { kEventCodecChange = AudioNode::kEventLast + 1 };
    DecoderNode(IAudioPipeline& parent): AudioNode(parent, "decoder"){}
    bool hasPrefetchedData() const { return mStreamHdr != nullptr; }
    void pullPrefetchedData(char* buf, size_t& size);
    virtual Type type() const { return kTypeDecoder; }
    virtual StreamError pullData(DataPullReq& dpr);
    virtual void confirmRead(int size) {}
    virtual ~DecoderNode() { deleteDecoder(); }
    virtual void reset() override { deleteDecoder(); }
    friend class Decoder;
};

#endif
