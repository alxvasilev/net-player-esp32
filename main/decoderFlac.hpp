#ifndef DECODER_FLAC_HPP
#define DECODER_FLAC_HPP
#include "decoderNode.hpp"
#include <FLAC/stream_decoder.h>

class DecoderFlac: public Decoder
{
protected:
    enum {
        kMaxSamplesPerBlock = 4608,
        kOutputSplitMaxSamples = 2048,
        kMaxNumReads = 40
    };
    typedef bool (DecoderFlac::*OutputFunc)(int nSamples, const FLAC__int32* const samples[]);
    int mOutputReadOfs = 0;
    int mNumReads = 0;
    DataPacket::unique_ptr mInputPacket;
    int mInputPos = 0;
    FLAC__StreamDecoder* mDecoder = nullptr;
    OutputFunc mOutputFunc = nullptr;
    AudioNode::PacketResult* mLastInputPr = nullptr;
    uint16_t mOutputChunkSize = 0;
    bool mHasOutput = false;
    StreamEvent mLastInputEvent = kNoError;
    void init();
    static FLAC__StreamDecoderReadStatus readCb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data);
    static void errorCb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data);
    static FLAC__StreamDecoderWriteStatus writeCb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *userp);
    static void metadataCb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data);
    template <typename T, bool isMono=false>
    bool outputSamples(int nSamples, const FLAC__int32* const samples[]);
    bool selectOutputFunc(int nChans, int bps);
public:
    virtual Codec::Type type() const { return Codec::kCodecFlac; }
    DecoderFlac(DecoderNode& parent, AudioNode& src, bool oggMode);
    ~DecoderFlac();
    virtual StreamEvent decode(AudioNode::PacketResult& dpr);
    virtual void reset();
};

#endif
