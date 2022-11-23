#ifndef DECODER_FLAC_HPP
#define DECODER_FLAC_HPP
#include "decoderNode.hpp"
#include <FLAC/stream_decoder.h>

class DecoderFlac: public Decoder
{
protected:
    enum {
        kMaxSamplesPerBlock = 4608,
        kOutputBufSize = kMaxSamplesPerBlock * 2 * 2
    };

    int16_t* mOutputBuf;
    int mOutputLen;
    AudioNode::DataPullReq* mDprPtr = nullptr;
    FLAC__StreamDecoder* mDecoder = nullptr;
    AudioNode::StreamError mLastStreamEvent;
    void init();
    void freeBuffers();
    static FLAC__StreamDecoderReadStatus readCb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data);
    static void errorCb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data);
    static FLAC__StreamDecoderWriteStatus writeCb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *userp);
    static void metadataCb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data);
public:
    virtual CodecType type() const { return kCodecFlac; }
    DecoderFlac(AudioNode& src);
    ~DecoderFlac();
    virtual AudioNode::StreamError pullData(AudioNode::DataPullReq& dpr);
    virtual void reset();
};

#endif
