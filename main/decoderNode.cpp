#include "decoderNode.hpp"
#include "decoderMp3.hpp"
#include "decoderAac.hpp"
#include "decoderFlac.hpp"
#include "decoderWav.hpp"
#include "detectorOgg.hpp"
#include "streamEvents.hpp"

bool DecoderNode::createDecoder(StreamFormat fmt)
{
    /* info may contain a buffer with some bytes prefetched from the stream in order to detect the codec,
     * in case it's encapsulated in a transport stream. For example - ogg, where the mime is just audio/ogg. In this case
     * We need to pass that pre-fetched data to the codec. We can't peek the first bytes via pullData with
     * no subsequent confirmRead(), because the needed amount of bytes may be non-contiguous in the ring
     * buffer. In that case, pullData() will return less than needed
     */
    auto freeBefore = heapFreeTotal();
    switch (fmt.codec().type) {
    case Codec::kCodecMp3:
        mDecoder = new DecoderMp3(*this, *mPrev);
        break;
    case Codec::kCodecAac:
        mDecoder = new DecoderAac(*this, *mPrev);
        break;
    case Codec::kCodecFlac:
        mDecoder = new DecoderFlac(*this, *mPrev, fmt.codec().transport == Codec::kTransportOgg);
        break;
    case Codec::kCodecWav:
        mDecoder = new DecoderWav(*this, *mPrev, StreamFormat(Codec::kCodecWav));
        break;
    case Codec::kCodecPcm:
        mDecoder = new DecoderWav(*this, *mPrev, fmt);
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
        fmt.codec().toString(), freeBefore - heapFreeTotal(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    plSendEvent(kEventCodecChange, fmt.codec().asNumCode());
    return true;
}
void DecoderNode::deleteDecoder()
{
    mStartingNewStream = true;
    if (!mDecoder) {
        return;
    }
    auto codec = mDecoder->type();
    auto freeBefore = heapFreeTotal();
    delete mDecoder;
    mDecoder = nullptr;
    ESP_LOGI(mTag, "\e[34mDeleted %s decoder freed %d bytes",
        Codec::numCodeToStr(codec), heapFreeTotal() - freeBefore);
}
AudioNode::StreamError DecoderNode::detectCodecCreateDecoder(AudioNode::DataPullReq& odp)
{
    StreamError err;
    while ((err = mPrev->pullData(odp)) == kNoError) {
        ESP_LOGW(mTag, "detectCodec: Discarding %d bytes of stream data", odp.size);
    }
    if (err != kEvtStreamChanged) {
        return err;
    }
    assert(odp.hasEvent());
    StreamEvent& evt = *odp.event();
    Codec& codec = evt.fmt.codec();

    if (codec.type == Codec::kCodecUnknown && codec.transport == Codec::kTransportOgg) {
        // allocates a buffer in odp and fetches some stream bytes in it
        auto err = detectOggCodec(*mPrev, codec);
        if (err != kNoError) {
            ESP_LOGW(mTag, "Error %s detecting ogg-encapsulated codec", streamEventToStr(err));
            return err;
        }
    }
    bool ok = createDecoder(evt.fmt);
    if (!ok) {
        ESP_LOGE(mTag, "createDecoder(%s) failed", codec.toString());
        return kErrNoCodec;
    }
    return kNoError;
}

AudioNode::StreamError DecoderNode::pullData(DataPullReq& odp)
{
    for (;;) {
        // get only stream format, no data, but wait for data to be available (so we know the stream format)
        if (!mDecoder) {
            printf("detect and create decoder\n");
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
                plSendEvent(kEventNewStream, mDecoder->outputFormat.asNumCode());
                if (!mPrev->waitForPrefill()) {
                    return kErrStreamStopped;
                }
            }
            return kNoError;
        }
        if (err == kEvtStreamChanged) {
            mStartingNewStream = true;
            deleteDecoder();
        }
        else if (err == kErrStreamStopped || err == kEvtStreamEnd) {
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

void DecoderNode::confirmRead(int size)
{
    if (mDecoder) {
        mDecoder->confirmRead(size);
    }
}
