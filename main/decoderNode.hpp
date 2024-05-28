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
    bool mWaitingPrefill = false;
    StreamId mInStreamId = 0;
    StreamRingQueue<24> mRingBuf;
    // pr in case there is a stream event that needs to be propagated
    StreamEvent detectCodecCreateDecoder(GenericEvent& startPkt);
    bool createDecoder(StreamFormat fmt);
    StreamEvent decode();
    StreamEvent forwardEvent(StreamEvent evt, AudioNode::PacketResult& pr);
    static int32_t heapFreeTotal(); // used to  calculate memory usage for codecs
    void deleteDecoder();
public:
    enum { kEventCodecChange = AudioNode::kEventLast + 1 };
    enum { kStackSize = 9000, kPrio = 20, kCore = 0 };
    DecoderNode(IAudioPipeline& parent): AudioNodeWithTask(parent, "decoder", kStackSize, kPrio, kCore){}
    virtual Type type() const { return kTypeDecoder; }
    virtual void nodeThreadFunc();
    virtual StreamEvent pullData(PacketResult &pr);
    virtual ~DecoderNode() { deleteDecoder(); }
    virtual void reset() override { deleteDecoder(); }
    virtual void onStopRequest() { mRingBuf.setStopSignal(); }
    virtual void onStopped() { mRingBuf.clear(); deleteDecoder(); }
    bool codecOnFormatDetected(StreamFormat fmt, uint8_t sourceBps); // called by codec when it know the sample format, and before posting any data packet
    bool codecPostOutput(DataPacket* pkt); // called by codec to output a decoded packet
    friend class Decoder;
};

#endif
