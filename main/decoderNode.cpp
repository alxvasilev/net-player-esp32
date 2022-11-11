#include "decoderNode.hpp"
#include "decoderMp3.hpp"
#include "decoderAac.hpp"
#include "decoderFlac.hpp"

bool DecoderNode::createDecoder(CodecType type)
{
    switch (type) {
    case kCodecMp3:
        ESP_LOGI(mTag, "Creating MP3 decoder");
        mDecoder = new DecoderMp3(*mPrev);
        return true;
    case kCodecAac:
        ESP_LOGI(mTag, "Creating AAC decoder");
        mDecoder = new DecoderAac(*mPrev);
        return true;
    case kCodecFlac:
        ESP_LOGI(mTag, "Creating FLAC decoder");
        mDecoder = new DecoderFlac(*mPrev);
        return true;
    case kCodecOggFlac:
        ESP_LOGI(mTag, "Creating Ogg/FLAC decoder");
//      mDecoder = new DecoderFlac(true);
        return true;
    case kCodecOggVorbis:
        ESP_LOGI(mTag, "Creating Ogg/Vorbis decoder");
//      mDecoder = new DecoderVorbis();
        return true;
    default:
        return false;
    }
}

AudioNode::StreamError DecoderNode::detectCodecCreateDecoder(CodecType type)
{
    if (createDecoder(type)) {
        return kNoError;
    }
    return kErrNoCodec;
    /*
    int16_t detected;
    if (type == kCodecOggTransport) {
        detected = OggCodecDetector::detect(*mPrev);
    }
    else {
        return kErrNoCodec;
    }
    if (detected < 0) {
        ESP_LOGW(mTag, "Error %d detecting %s-encapsulated codec", detected, StreamFormat::codecTypeToStr(type));
        return (StreamError)detected;
    }
    return createDecoder((CodecType)detected) ? kNoError : kErrNoCodec;
    */
}

AudioNode::StreamError DecoderNode::pullData(DataPullReq& odp)
{
    for (;;) {
        // get only stream format, no data, but wait for data to be available (so we know the stream format)
        if (!mDecoder) {
            printf("No decoder, creating one\n");
            DataPullReq idp(0);
            auto err = mPrev->pullData(idp);
            printf("Format get returned %d\n", err);

            if (err && err != kStreamChanged) {
                return err;
            }
            err = detectCodecCreateDecoder(idp.fmt.codec());
            if (err) {
                return err;
            }
            myassert(mDecoder);
        }
        auto err = mDecoder->pullData(odp);
        if (!err) {
            return kNoError;
        }
        else if (err == kStreamChanged) {
            if (odp.fmt.codec() == mDecoder->outputFormat.codec()) {
                mDecoder->reset();
                return kStreamChanged;
            } else {
                delete mDecoder;
                mDecoder = nullptr;
                return kStreamChanged;
            }
        }
        else {
            return err;
        }
    }
}
