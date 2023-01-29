#ifndef DECODER_WAV_HPP
#define DECODER_WAV_HPP

#include "decoderNode.hpp"
class DecoderWav: public Decoder
{
protected:
    typedef void(DecoderWav::*OutputFunc)(AudioNode::DataPullReq& dpr);
    enum { kSamplesPerRequest = 1024 };
    uint8_t mInputBytesPerSample = 0;
    uint16_t mOutputBytesPerFrame = 0;
    uint16_t mInputBytesPerFrame = 0;
    DynBuffer mOutputBuf;
    OutputFunc mOutputFunc = nullptr;
    char mSingleSampleBuf[2 * 4]; // needed when source can return only a fraction of a contiguous sample
    bool mNeedConfirmRead = true; // when we readExact() to mSingleSampleBuf, confirmRead() is implied
    AudioNode::StreamError fillBuf(AudioNode::DataPullReq& dpr, char* buf, int size);
    AudioNode::StreamError parseWavHeader(AudioNode::DataPullReq& dpr);
    bool setupOutput();
    template <typename T, int Bps, bool BigEndian=false>
    void transformAudioData(AudioNode::DataPullReq& audio);
    void outputSwapBeToLe16(AudioNode::DataPullReq& audio);
    void outputSwapBeToLe32(AudioNode::DataPullReq& audio);
public:
    virtual Codec::Type type() const { return outputFormat.codec().type; }
    DecoderWav(DecoderNode& parent, AudioNode& src, StreamFormat fmt);
    virtual ~DecoderWav() {}
    virtual AudioNode::StreamError pullData(AudioNode::DataPullReq& dpr);
    virtual void confirmRead(int size) override;
    virtual void reset() {}
};
#endif
