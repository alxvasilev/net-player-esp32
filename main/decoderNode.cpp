#include "decoderNode.hpp"
#include "decoderMp3.hpp"
#include "decoderAac.hpp"
#include "decoderFlac.hpp"

bool DecoderNode::createDecoder(CodecType type)
{
    auto freeBefore = heapFreeTotal();
    switch (type) {
    case kCodecMp3:
        mDecoder = new DecoderMp3(*mPrev);
        break;
    case kCodecAac:
        mDecoder = new DecoderAac(*mPrev);
        break;
    case kCodecFlac:
        mDecoder = new DecoderFlac(*mPrev);
        break;
/*
    case kCodecOggFlac:
        mDecoder = new DecoderFlac(true);
        break;
    case kCodecOggVorbis:
        mDecoder = new DecoderVorbis();
        break;
*/
    default:
        return false;
    }
    ESP_LOGI(mTag, "\e[34mCreated %s decoder, approx %d bytes of RAM consumed (%d free internal)",
        codecTypeToStr(mDecoder->codec), freeBefore - heapFreeTotal(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}
void DecoderNode::deleteDecoder()
{
    if (!mDecoder) {
        return;
    }
    auto codec = mDecoder->codec;
    auto freeBefore = heapFreeTotal();
    delete mDecoder;
    mDecoder = nullptr;
    ESP_LOGI(mTag, "\e[34mDeleted %s decoder freed %d bytes", codecTypeToStr(codec), heapFreeTotal() - freeBefore);
}
AudioNode::StreamError DecoderNode::detectCodecCreateDecoder(DataPullReq& odp)
{
    odp.size = 0;
    auto err = mPrev->pullData(odp);
    if (err) {
        return err;
    }
    if (createDecoder(odp.codec)) {
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
            auto err = detectCodecCreateDecoder(odp);
            if (err) {
                return err;
            }
        }
        myassert(mDecoder);
        // Pull requested size is ignored here, codec always returns whole frames.
        // And in case codec returns an event, having it pre-initialized to zero,
        // it doesn't need to zero-out the size field
        odp.size = 0;
        auto err = mDecoder->pullData(odp);
        if (!err) {
            return kNoError;
        }
        if (odp.size) {
            printf("=============== ASSERT odp.size is not zero but %d, event %d\n", odp.size, err);
        }
        if (err == kStreamChanged) {
            if (odp.codec == mDecoder->codec) {
                mDecoder->reset();
            } else {
                deleteDecoder();
            }
            printf("decoderNode: kStreamChanged event with streamId: %u\n", odp.streamId);
        } else if (err == kStreamStopped) {
            deleteDecoder();
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




