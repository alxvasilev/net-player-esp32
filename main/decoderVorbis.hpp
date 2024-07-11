#ifndef DECODER_VORBIS_HPP
#define DECODER_VORBIS_HPP

#include "decoderNode.hpp"
#define VORB_DEBUG
#include "vorbisHighLevel.hpp"

class DecoderVorbis: public Decoder
{
protected:
    enum { kInitChunkSize = 16384, kTargetOutputSamples = 2048 };
    VorbisDecoder mVorbis;
    void updateOutputFormat();
public:
    virtual Codec::Type type() const { return Codec::kCodecVorbis; }
    DecoderVorbis(DecoderNode& parent, AudioNode& src);
    virtual StreamEvent decode(AudioNode::PacketResult& pr);
    virtual void reset() override;
};

#endif
