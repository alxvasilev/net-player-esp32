#include "decoderVorbis.hpp"

static const char* TAG = "vorbis";

DecoderVorbis::DecoderVorbis(DecoderNode& parent, AudioNode& src)
: Decoder(parent, src)
{}

void DecoderVorbis::reset()
{
    mVorbis.reset();
    outputFormat.clear();
}
bool assignComment(const char* prefix, int pfxLen, unique_ptr_mfree<const char>& dest, const char* src)
{
    if (strncasecmp(src, prefix, pfxLen)) {
        return false;
    }
    src += pfxLen;
    for(;;) {
        if (!*src) {
            return true;
        }
        if (!isspace(*src)) {
            dest.reset(strdup(src));
            return true;
        }
        src++;
    }
}
StreamEvent DecoderVorbis::reinit(AudioNode::PacketResult& pr, bool isInitial)
{
    myassert(isInitial == !outputFormat);
    int bytesIn;
    if (!isInitial) {
        mVorbis.reset(false);
        bytesIn = mVorbis.pendingBytes();
    }
    else {
        bytesIn = 0;
    }
    for (;;) {
        auto event = mSrcNode.pullData(pr);
        if (event) {
            return event;
        }
        auto& pkt = pr.dataPacket();
        mVorbis.write(pkt.data, pkt.dataLen);
        bytesIn += pkt.dataLen;
        if (bytesIn >= kInitChunkSize) {
            break;
        }
    }
    auto ret = mVorbis.init();
    if (ret <= 0) {
        if (ret == 0) {
            ESP_LOGE(TAG, "Too little data (%d bytes) to init decoder", bytesIn);
        }
        else {
            ESP_LOGE(TAG, "Error %d initializing decoder", ret);
        }
        return kErrDecode;
    }
    if (isInitial) {
        outputFormat = getOutputFormat();
        mParent.codecOnFormatDetected(outputFormat, 16);
    }
    else {
        auto newFmt = getOutputFormat();
        if (newFmt != outputFormat) {
            ESP_LOGE(TAG, "Output format changed between chained files within the same stream");
            return kErrDecode;
        }
    }

    char** ptr = mVorbis.streamComments().user_comments;
    if (*ptr) {
        unique_ptr_mfree<const char> title;
        unique_ptr_mfree<const char> artist;
        do {
            if (assignComment("TITLE=", 6, title, *ptr)) {
                if (artist) {
                    break;
                }
            }
            else if (assignComment("ARTIST=", 7, artist, *ptr)) {
                if (title) {
                    break;
                }
            }
            ptr++;
        } while (*ptr);
        if (title || artist) {
            mParent.codecPostOutput(new TitleChangeEvent(title.release(), artist.release()));
        }
    }
    return kNoError;
}
StreamEvent DecoderVorbis::decode(AudioNode::PacketResult& pr)
{
    if (!outputFormat) {
        auto event = reinit(pr, true);
        if (event) {
            return event;
        }
    }
    DataPacket::unique_ptr pkt(DataPacket::create<true>(kTargetOutputSamples * 8));
    pkt->flags |= StreamPacket::kFlagHasSpaceFor32Bit;
    int written = 0;
    while (written < kTargetOutputSamples) {
        int nSamples;
        while ((nSamples = mVorbis.numOutputSamples()) < 1) {
            int ret = mVorbis.decode();
            if (ret < 0) {
                if (ret == OV_EOF) {
                    ESP_LOGI(TAG, "End of vorbis stream, resetting codec");
                    return reinit(pr, false);
                }
                ESP_LOGW(TAG, "Decode error %d", ret);
                return kErrDecode;
            }
            else if (ret == 0) {
                auto event = mSrcNode.pullData(pr);
                if (event) {
                    return event;
                }
                auto& inPkt = pr.dataPacket();
                mVorbis.write(inPkt.data, inPkt.dataLen);
                continue;
            }
        }
        int remain = std::min(nSamples, kTargetOutputSamples - written);
        int outSamples = mVorbis.getSamples<int16_t, 16>((int16_t*)pkt->data + written, remain);
        if (outSamples != remain) {
            ESP_LOGW(TAG, "Output number of samples is different that declared by decoder");
            assert(outSamples < remain);
        }
        written += outSamples;
    }
    pkt->dataLen = written * 2;
    mParent.codecPostOutput(pkt.release());
    return kNoError;
}
StreamFormat DecoderVorbis::getOutputFormat()
{
    const auto& info = mVorbis.streamInfo();
    return StreamFormat(Codec(Codec::kCodecVorbis, Codec::kTransportOgg), info.rate, 16, info.channels);
}
