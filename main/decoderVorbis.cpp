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
StreamEvent DecoderVorbis::decode(AudioNode::PacketResult& dpr)
{
    if (!outputFormat) {
        int bytesIn = 0;
        for (;;) {
            auto event = mSrcNode.pullData(dpr);
            if (event) {
                return event;
            }
            auto& pkt = dpr.dataPacket();
            mVorbis.write(pkt.data, pkt.dataLen);
            bytesIn += pkt.dataLen;
            if (bytesIn >= kInitChunkSize) {
                break;
            }
        }
        auto ret = mVorbis.init();
        if (ret <= 0) {
            if (ret == 0) {
                ESP_LOGE(TAG, "First chunk of size %d is too small to init decoder", bytesIn);
            }
            else {
                ESP_LOGE(TAG, "Error %d initializing decoder", ret);
            }
            return kErrDecode;
        }
        updateOutputFormat();

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
    DataPacket::unique_ptr pkt(DataPacket::create<true>(kTargetOutputSamples * 4));
    pkt->flags |= StreamPacket::kFlagHasSpaceFor32Bit;
    int written = 0;
    while (written < kTargetOutputSamples) {
        int nSamples;
        while ((nSamples = mVorbis.numOutputSamples()) < 1) {
        decode:
            int ret = mVorbis.decode();
            if (ret < 0) {
                if (ret == OV_EOF) {
                    ESP_LOGI(TAG, "End of vorbis stream, resetting codec");
                    mVorbis.reset(false);
                    outputFormat.clear();
                    return kNoError;
                }
                ESP_LOGW(TAG, "Decode error %d", ret);
                return kErrDecode;
            }
            else if (ret == 0) {
            /*    if (mVorbis.eos()) {
                    ESP_LOGI(TAG, "Decode needs more data, but reached end of stream, resetting");
                    reset();
                    return kNoError;
                } */
                auto event = mSrcNode.pullData(dpr);
                if (event) {
                    return event;
                }
                auto& inPkt = dpr.dataPacket();
                mVorbis.write(inPkt.data, inPkt.dataLen);
                goto decode;
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
void DecoderVorbis::updateOutputFormat()
{
    const auto& info = mVorbis.streamInfo();
    outputFormat.setCodec(Codec(Codec::kCodecVorbis, Codec::kTransportOgg));
    outputFormat.setSampleRate(info.rate);
    outputFormat.setNumChannels(info.channels);
    outputFormat.setBitsPerSample(16);
    mParent.codecOnFormatDetected(outputFormat, 16);
}
