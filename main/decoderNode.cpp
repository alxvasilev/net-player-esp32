#include "decoderNode.hpp"
#include "decoderMp3.hpp"
#include "decoderAac.hpp"
#include "decoderFlac.hpp"
#include "decoderWav.hpp"
#include "detectorOgg.hpp"
bool DecoderNode::createDecoder(StreamFormat fmt, std::unique_ptr<StreamItem>& firstChunk)
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
        mDecoder = new DecoderMp3(*this, *mPrev, firstChunk);
        break;
    case Codec::kCodecAac:
        mDecoder = new DecoderAac(*this, *mPrev, firstChunk);
        break;
    case Codec::kCodecFlac:
        mDecoder = new DecoderFlac(*this, *mPrev, fmt.codec().transport == Codec::kTransportOgg, firstChunk);
        break;
    case Codec::kCodecWav:
        mDecoder = new DecoderWav(*this, *mPrev, StreamFormat(Codec::kCodecWav), firstChunk);
        break;
    case Codec::kCodecPcm:
        mDecoder = new DecoderWav(*this, *mPrev, fmt, firstChunk);
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
bool DecoderNode::detectCodecCreateDecoder(const std::unique_ptr<StreamItem>& start)
{
    assert(start);
    mStartingNewStream = true;
    if (start->type != kStreamStart) {
        return plNotifyError(StreamError::kErrInvalidFirstChunk);
    }
    auto fmt = static_cast<StreamStartItem*>(start.get())->fmt;
    std::unique_ptr<StreamItem> firstChunk;
    if (!mPrev->pullData(firstChunk)) {
        return false;
    }
    assert(firstChunk.get());
    if (firstChunk->type != kStreamData) {
        return plNotifyError(kErrInvalidFirstChunk);
    }
    Codec& codec = fmt.codec();
    if (codec.type == Codec::kCodecUnknown && codec.transport == Codec::kTransportOgg) {
        auto err = detectOggCodec(codec, *(StreamDataItem*)firstChunk.get());
        if (err) {
            return plNotifyError(err);
        }
    }
    if (!createDecoder(fmt, firstChunk)) {
        return plNotifyError(kErrNoCodec);
    }
    plSendEvent(kEventNewStream, mDecoder->outputFormat.asNumCode());
    return true;
}
bool DecoderNode::pullData(std::unique_ptr<StreamItem>& item)
{
    for (;;) {
        // get only stream format, no data, but wait for data to be available (so we know the stream format)
        if (!mDecoder) {
            if (!mPrev->pullData(item)) {
                return false;
            }
            return detectCodecCreateDecoder(item);
        }
        myassert(mDecoder);
        item.reset();
        StreamError err = mDecoder->pullData(item);
        if (!err) {
            auto type = item->type;
            if (type == kStreamData) {
                if (mStartingNewStream) {
                    mStartingNewStream = false;
                    plSendEvent(kEventNewStream, mDecoder->outputFormat.asNumCode());
                }
            }
            else if (type == kStreamEnd) {
                deleteDecoder();
            }
            else if (type == kStreamStart) {
                deleteDecoder();
                return detectCodecCreateDecoder(item);
            }
            return true;
        }
        else {
            if (err != kErrUpstream) {
                plNotifyError(err);
            }
            return false;
        }
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
