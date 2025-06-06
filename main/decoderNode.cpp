#include "decoderNode.hpp"
#include "decoderMp3.hpp"
#include "decoderAac.hpp"
#include "decoderFlac.hpp"
#include "decoderWav.hpp"
#include "decoderVorbis.hpp"
#include "detectorOgg.hpp"
#include "streamPackets.hpp"

bool DecoderNode::createDecoder(StreamFormat fmt)
{
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
    case Codec::kCodecVorbis:
        mDecoder = new DecoderVorbis(*this, *mPrev);
        break;

    default:
        return false;
    }
    ESP_LOGI(mTag, "\e[34mCreated %s decoder, approx %ld bytes of RAM consumed (%zu free internal)",
        fmt.codec().toString(), freeBefore - heapFreeTotal(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    plSendEvent(kEventCodecChange, fmt.codec().asNumCode());
    return true;
}
void DecoderNode::deleteDecoder()
{
    if (!mDecoder) {
        return;
    }
    auto codec = mDecoder->type();
    auto freeBefore = heapFreeTotal();
    delete mDecoder;
    mDecoder = nullptr;
    ESP_LOGI(mTag, "\e[34mDeleted %s decoder freed %ld bytes",
        Codec::numCodeToStr(codec), heapFreeTotal() - freeBefore);
}
StreamEvent DecoderNode::detectCodecCreateDecoder(NewStreamEvent* startPkt)
{
    // Save the properties of the new-stream packet that we swallow,
    // so we can set them on the one we generate
    mNewStreamPkt.reset(startPkt);
    Codec& codec = mNewStreamPkt->fmt.codec();
    if (codec.type == Codec::kCodecUnknown && codec.transport == Codec::kTransportOgg) {
        // allocates a buffer in odp and fetches some stream bytes in it
        bool preceded;
        auto dataPacket = mPrev->peekData(preceded);
        if (!dataPacket) {
            return preceded ? kErrDecode : kErrStreamStopped;
        }
        auto evt = detectOggCodec(*dataPacket, codec);
        if (evt) {
            ESP_LOGW(mTag, "Error %s detecting ogg-encapsulated codec", streamEventToStr(evt));
            return evt;
        }
    }
    bool ok = createDecoder(mNewStreamPkt->fmt);
    if (!ok) {
        ESP_LOGE(mTag, "createDecoder(%s) failed", codec.toString());
        return kErrNoCodec;
    }
    return kNoError;
}
void DecoderNode::nodeThreadFunc()
{
    ESP_LOGI(mTag, "Task started");
    mRingBuf.clearStopSignal();
    for (;;) {
        processMessages();
        if (mTerminate) {
            return;
        }
        myassert(mState == kStateRunning);
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            auto err = decode();
            if (err) { // err cannot be an event here
                plSendError(err, 0);
                stop(false);
                break;
            }
        }
    }
}
StreamEvent DecoderNode::forwardEvent(AudioNode::PacketResult& pr)
{
    assert(pr.packet.get());
    return mRingBuf.pushBack(pr.packet.release()) ? kNoError : kErrStreamStopped;
}
// returns errors or kNoError only, events are forwarded
StreamEvent DecoderNode::decode()
{
    AudioNode::PacketResult pr;
    if (!mDecoder) {
        StreamEvent evt;
        while ((evt = mPrev->pullData(pr)) == kEvtData) {
            ESP_LOGW(mTag, "Detect codec: Discarding %d bytes of stream data", pr.dataPacket().dataLen);
        }
        if (evt != kEvtStreamChanged) {
            return evt > 0 ? forwardEvent(pr) : evt;
        }
        myassert(pr.packet);
        evt = detectCodecCreateDecoder(static_cast<NewStreamEvent*>(pr.packet.release()));
        if (evt) { // evt can only be an error here
            return evt;
        }
    }
    myassert(mDecoder);
    auto evt = mDecoder->decode(pr);
    if (evt) {
        if (evt < 0) {
            deleteDecoder();
            return evt;
        }
        else if (evt == kEvtStreamChanged) {
            deleteDecoder();
            return detectCodecCreateDecoder(static_cast<NewStreamEvent*>(pr.packet.release()));
        }
        else if (evt == kEvtStreamEnd) {
            ESP_LOGI(mTag, "Stream end, deleting decoder");
            deleteDecoder();
        }
        return forwardEvent(pr);
    }
    return kNoError;
}
StreamEvent DecoderNode::pullData(PacketResult &pr)
{
    auto pkt = mRingBuf.popFront();
    if (!pkt) {
        return kErrStreamStopped;
    }
    return pr.set(pkt);
}
bool DecoderNode::codecOnFormatDetected(StreamFormat fmt, uint8_t sourceBps)
{
    StreamFormat sourceFmt(fmt);
    sourceFmt.setBitsPerSample(sourceBps);
    mPrev->notifyFormatDetails(sourceFmt);
    myassert(mNewStreamPkt);
    mNewStreamPkt->fmt = fmt;
    mNewStreamPkt->sourceBps = sourceBps;
    return mRingBuf.pushBack(mNewStreamPkt.release());
}
bool DecoderNode::codecPostOutput(StreamPacket *pkt)
{
    return mRingBuf.pushBack(pkt);
}
int32_t DecoderNode::heapFreeTotal()
{
    int32_t result = xPortGetFreeHeapSize();
    if (utils::haveSpiRam()) {
        result += heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
    return result;
}
