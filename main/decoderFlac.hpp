#ifndef DECODER_FLAC_HPP
#define DECODER_FLAC_HPP
#include "decoderNode.hpp"
#include <FLAC/stream_decoder.h>

class DecoderFlac: public Decoder
{
protected:
    enum {
        kMaxSamplesPerBlock = 4608,
        kOutputBufSize = 65536
    };
    typedef bool (DecoderFlac::*OutputFunc)(int nSamples, const FLAC__int32* const samples[]);
    uint8_t* mOutputBuf;
    uint16_t mOutputChunkSize = 0;
    int mOutputLen;
    int mOutputReadOfs = 0;
    AudioNode::DataPullReq* mDprPtr = nullptr;
    FLAC__StreamDecoder* mDecoder = nullptr;
    OutputFunc mOutputFunc = nullptr;
    AudioNode::StreamError mLastStreamEvent;
    void init();
    static FLAC__StreamDecoderReadStatus readCb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data);
    static void errorCb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data);
    static FLAC__StreamDecoderWriteStatus writeCb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *userp);
    static void metadataCb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data);
    template <typename T, int Shift=0>
    bool outputStereoSamples(int nSamples, const FLAC__int32* const samples[]);
    template <typename T, int Shift=0>
    bool outputMonoSamples(int nSamples, const FLAC__int32* const samples[]);
public:
    virtual CodecType type() const { return kCodecFlac; }
    DecoderFlac(DecoderNode& parent, AudioNode& src, bool oggMode);
    ~DecoderFlac();
    virtual AudioNode::StreamError pullData(AudioNode::DataPullReq& dpr);
    virtual void reset();
};

#endif
