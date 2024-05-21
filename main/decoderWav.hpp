#ifndef DECODER_WAV_HPP
#define DECODER_WAV_HPP

#include "decoderNode.hpp"
class DecoderWav: public Decoder
{
protected:
    typedef bool(DecoderWav::*OutputFunc)(char* data, int len);
    OutputFunc mOutputFunc = nullptr;
    bool mOutputInSitu = false;
    int parseWavHeader(DataPacket& pkt);
    bool setupOutput();
    template <typename T, int Bps, bool BigEndian=false>
    bool output(char* input, int len);
    bool outputSwapBeToLe16(char* input, int len);
    bool outputSwapBeToLe32(char* input, int len);
    bool outputNoChange(char*, int) { return true; }
public:
    virtual Codec::Type type() const { return outputFormat.codec().type; }
    DecoderWav(DecoderNode& parent, AudioNode& src, StreamFormat fmt);
    virtual ~DecoderWav() {}
    virtual StreamEvent decode(AudioNode::PacketResult& output);
    virtual void reset() {}
};
#endif
