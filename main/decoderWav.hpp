#ifndef DECODER_WAV_HPP
#define DECODER_WAV_HPP

#include "decoderNode.hpp"
class DecoderWav: public Decoder
{
protected:
    typedef bool(DecoderWav::*OutputFunc)(char* data, int len);
    typedef void(DecoderWav::*OutputInSituFunc)(DataPacket& input);
    OutputFunc mOutputFunc = nullptr;
    OutputInSituFunc mOutputInSituFunc = nullptr;
    char mPartialInSampleBuf[8];
    int8_t mPartialInSampleBytes = 0;
    int8_t mInBytesPerSample = 0;
    int8_t mNumChans = 0; // cached from outputFormat for faster access
    int parseWavHeader(DataPacket& pkt);
    bool setupOutput();
    template<typename T>
    void selectOutput16or32();
    template <typename T, int Bps, bool BigEndian=false>
    bool outputWithNewPacket(char* input, int len);
    template <typename T>
    void outputSwapBeToLeInSitu(DataPacket& pkt);
    void outputNoChange(DataPacket& pkt) {}
public:
    virtual Codec::Type type() const { return outputFormat.codec().type; }
    DecoderWav(DecoderNode& parent, AudioNode& src, StreamFormat fmt);
    virtual ~DecoderWav() {}
    virtual StreamEvent decode(AudioNode::PacketResult& output);
    virtual void reset() {}
};
#endif
