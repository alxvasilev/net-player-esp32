#include "decoderNode.hpp"
#include "decoderMp3.hpp"
#include "decoderAac.hpp"
#include "decoderFlac.hpp"
#include "detectorOgg.hpp"

bool DecoderNode::createDecoder(CodecType codec)
{
    /* info may contain a buffer with some bytes prefetched from the stream in order to detect the codec,
     * in case it's encapsulated in a transport stream. For example - ogg, where the mime is just audio/ogg. In this case
     * We need to pass that pre-fetched data to the codec. We can't peek the first bytes via pullData with
     * no subsequent confirmRead(), because the needed amount of bytes may be non-contiguous in the ring
     * buffer. In that case, pullData() will return less than needed
     */
    auto freeBefore = heapFreeTotal();
    switch (codec) {
    case kCodecMp3:
        mDecoder = new DecoderMp3(*this, *mPrev);
        break;
    case kCodecAac:
        mDecoder = new DecoderAac(*this, *mPrev);
        break;
    case kCodecFlac:
        mDecoder = new DecoderFlac(*this, *mPrev, false);
        break;
    case kCodecOggFlac:
        mDecoder = new DecoderFlac(*this, *mPrev, true);
        break;
/*
    case kCodecOggVorbis:
        mDecoder = new DecoderVorbis();
        break;
*/
    default:
        return false;
    }
    ESP_LOGI(mTag, "\e[34mCreated %s decoder, approx %d bytes of RAM consumed (%d free internal)",
        codecTypeToStr(mDecoder->codec), freeBefore - heapFreeTotal(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    plSendEvent(kEventCodecChange, codec);
    return true;
}
void DecoderNode::deleteDecoder()
{
    mStartingNewStream = true;
    if (!mDecoder) {
        return;
    }
    auto codec = mDecoder->codec;
    auto freeBefore = heapFreeTotal();
    delete mDecoder;
    mDecoder = nullptr;
    ESP_LOGI(mTag, "\e[34mDeleted %s decoder freed %d bytes", codecTypeToStr(codec), heapFreeTotal() - freeBefore);
}
AudioNode::StreamError DecoderNode::detectCodecCreateDecoder(AudioNode::DataPullReq& odp)
{
    odp.size = 0;
    auto err = mPrev->pullData(odp);
    if (err) {
        return err;
    }
    assert(!odp.buf && !odp.size);
    if (odp.codec == kCodecOggTransport) {
        // allocates a buffer in odp and fetches some stream bytes in it
        auto err = detectOggCodec(*mPrev, odp.codec);
        if (err != kNoError) {
            ESP_LOGW(mTag, "Error %s detecting ogg-encapsulated codec", streamEventToStr(err));
            return err;
        }
    }
    bool ok = createDecoder(odp.codec);
    return ok ? kNoError : kErrNoCodec;
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
            if (mStartingNewStream) {
                mStartingNewStream = false;
                plSendEvent(kEventNewStream, mDecoder->codec | (mDecoder->mode << 16), odp.fmt.asCode());
                if (!mPrev->waitForPrefill()) {
                    return kStreamStopped;
                }
            }
            return kNoError;
        }
        if (odp.size) {
            printf("ASSERT odp.size is not zero but %d, event %d\n", odp.size, err);
        }
        if (err == kStreamChanged) {
            if (odp.codec == mDecoder->codec) {
                ESP_LOGI(mTag, "Stream changed, but codec is the same, resetting %s decoder", codecTypeToStr(mDecoder->codec));
                mDecoder->reset();
                mStartingNewStream = true;
            } else {
                deleteDecoder();
            }
            printf("decoderNode: kStreamChanged event with streamId: %u\n", odp.streamId);
        }
        else if (err == kStreamStopped || err == kStreamEnd) {
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




