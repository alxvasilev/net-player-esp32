#ifndef DECODER_NODE_HPP
#define DECODER_NODE_HPP
#include "audioNode.hpp"
#include "streamRingQueue.hpp"

class DecoderNode;
class Decoder
{
protected:
    DecoderNode& mParent;
    AudioNode& mSrcNode;
public:
    StreamFormat outputFormat;
    Decoder(DecoderNode& parent, AudioNode& src, StreamFormat outFmt = 0)
    : mParent(parent), mSrcNode(src), outputFormat(outFmt) {}
    virtual ~Decoder() {}
    virtual Codec::Type type() const = 0;
    virtual StreamEvent decode(AudioNode::PacketResult& pr) = 0;
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
class DecoderNode: public AudioNodeWithTask
{
protected:
    Decoder* mDecoder = nullptr;
    NewStreamEvent::unique_ptr mNewStreamPkt;
    StreamRingQueue<24> mRingBuf;
    // pr in case there is a stream event that needs to be propagated
    StreamEvent detectCodecCreateDecoder(NewStreamEvent* startPkt);
    bool createDecoder(StreamFormat fmt);
    StreamEvent decode();
    static int32_t heapFreeTotal(); // used to  calculate memory usage for codecs
    void deleteDecoder();
public:
    enum { kEventCodecChange = AudioNode::kEventLast + 1 };
    enum { kStackSize = 10000, kPrio = 20, kCore = 0 };
    DecoderNode(IAudioPipeline& parent): AudioNodeWithTask(parent, "decoder", true, kStackSize, kPrio, kCore){}
    virtual Type type() const { return kTypeDecoder; }
    virtual void nodeThreadFunc();
    virtual StreamEvent pullData(PacketResult &pr);
    virtual ~DecoderNode() { deleteDecoder(); terminate(true); }
    virtual void reset() override { deleteDecoder(); }
    virtual void onStopRequest() { mRingBuf.setStopSignal(); }
    virtual void onStopped() { mRingBuf.clear(); deleteDecoder(); }
    using AudioNode::plSendEvent;
    StreamEvent forwardEvent(AudioNode::PacketResult& pr); // currently used externally only by FLAC
    bool codecOnFormatDetected(StreamFormat fmt, uint8_t sourceBps); // called by codec when it know the sample format, and before posting any data packet
    bool codecPostOutput(StreamPacket* pkt); // called by codec to output a decoded or title change packet
    friend class Decoder;
};

#endif
