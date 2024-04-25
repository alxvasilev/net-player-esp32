#ifndef DECODER_MP3_HPP
#define DECODER_MP3_HPP
#include "decoderNode.hpp"
#include <mad.h>
#include "streamRingQueue.hpp"

class DecoderMp3: public Decoder
{
protected:
    enum {
        kInputBufSize = kStreamChunkSize + 2048,
        kSamplesPerFrame = 1152,
        kOutputBufSize = kSamplesPerFrame * 4 // each mp3 packet decodes to 1152 samples, for 16 bit stereo, multiply by 4
    };
    struct mad_stream mMadStream;
    struct mad_frame mMadFrame;
    struct mad_synth mMadSynth;
    std::unique_ptr<char[]> mInputBuf;
    std::unique_ptr<StreamDataItem> mOutputChunk;
    int mInputLen = 0;
    bool initStreamFormat(mad_header& header);
    int output(const mad_pcm& pcm);
    void initMadState();
    void freeMadState();
    void logEncodingInfo();
    void reset();
public:
    virtual Codec::Type type() const { return Codec::kCodecMp3; }
    DecoderMp3(DecoderNode& parent, AudioNode& src, std::unique_ptr<StreamItem>& firstChunk);
    virtual ~DecoderMp3();
    virtual AudioNode::StreamEvent pullData(AudioNode::DataPullReq& dpr);
};

#endif
