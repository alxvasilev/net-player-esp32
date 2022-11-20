#include "decoderNode.hpp"
#include "decoderMp3.hpp"
#include "decoderAac.hpp"
#include "decoderFlac.hpp"

bool DecoderNode::createDecoder(CodecType type)
{
    auto freeBefore = heapFreeTotal();
    switch (type) {
    case kCodecMp3:
        ESP_LOGI(mTag, "Creating MP3 decoder");
        mDecoder = new DecoderMp3(*mPrev);
        break;
    case kCodecAac:
        ESP_LOGI(mTag, "Creating AAC decoder");
        mDecoder = new DecoderAac(*mPrev);
        break;
    case kCodecFlac:
        ESP_LOGI(mTag, "Creating FLAC decoder");
        mDecoder = new DecoderFlac(*mPrev);
        break;
    case kCodecOggFlac:
        ESP_LOGI(mTag, "Creating Ogg/FLAC decoder");
//      mDecoder = new DecoderFlac(true);
        break;
    case kCodecOggVorbis:
        ESP_LOGI(mTag, "Creating Ogg/Vorbis decoder");
//      mDecoder = new DecoderVorbis();
        break;
    default:
        return false;
    }
    ESP_LOGI(mTag, "Decoder uses approx %d bytes of RAM", freeBefore - heapFreeTotal());
    return true;
}

AudioNode::StreamError DecoderNode::detectCodecCreateDecoder(CodecType type, DataPullReq& odp)
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
            DataPullReq idp(0);
            auto err = mPrev->pullData(idp);

            if (err) {
                odp = idp;
                return err;
            }
            err = detectCodecCreateDecoder(idp.codec, odp);
            if (err) {
                if (err == kStreamChanged) {
                    myassert(!mDecoder);
                    continue;
                } else {
                    return err;
                }
            }
            myassert(mDecoder);
        }
        // Pull requested size is ignored here, codec always returns whole frames.
        // And in case codec returns an event, having it pre-initialized to zero,
        // it doesn't need to zero-out the size field
        odp.size = 0;
        auto err = mDecoder->pullData(odp);
        if (!err) {
            return kNoError;
        }
        myassert(odp.size == 0);
        if (err == kStreamChanged) {
            if (odp.codec == mDecoder->codec) {
                mDecoder->reset();
            } else {
                delete mDecoder;
                mDecoder = nullptr;
            }
        }
        return err;
    }
}
int32_t DecoderNode::heapFreeTotal()
{
    int32_t result = xPortGetFreeHeapSize();
    if (utils::haveSpiRam()) {
        result += heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
    return result;
}




