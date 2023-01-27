#ifndef DETECTOR_OGG_HPP
#define DETECTOR_OGG_HPP

#include "audioNode.hpp"

#pragma pack(push, 1)
struct OggPageHeader {
    uint32_t fourCC;
    uint8_t version;
    uint8_t hdrType;
    uint64_t granulePos;
    uint32_t serial;
    uint32_t pageSeqNo;
    uint32_t checksum;
    uint8_t numSegments;
    uint8_t segmentLens[];
};
#pragma pack(pop)

enum { kMaxNumSegments = 10 };

uint32_t fourccLittleEndian(const char* str)
{
    return *str | (*(str+1) << 8) | (*(str+2) << 16) | (*(str+3) << 24);
}

AudioNode::StreamError detectOggCodec(AudioNode& src, Codec& codec)
{
    enum { kPrefetchAmount = sizeof(OggPageHeader) + kMaxNumSegments + 10 }; // we need the first ~7 bytes in the segment
    char tmpbuf[kPrefetchAmount];
    auto data = src.peek(kPrefetchAmount, tmpbuf);
    if (!data) {
        AudioNode::DataPullReq dpr(kPrefetchAmount); // just read and discard the data to get the event
        auto err = src.pullData(dpr);
        assert(err != AudioNode::kNoError);
        return err;
    }
    OggPageHeader& hdr = *((OggPageHeader*)data);
    if (hdr.fourCC != fourccLittleEndian("OggS")) {
        ESP_LOGW("OGG", "Fourcc %x doesn't match 'OggS' (%x)", hdr.fourCC, fourccLittleEndian("OggS"));
        return AudioNode::kErrDecode;
    }
    if (hdr.numSegments > kMaxNumSegments) {
        ESP_LOGW("OGG", "More than expected segments in Ogg page: %d", hdr.numSegments);
        return AudioNode::kErrDecode;
    }
    const char* magic = data + sizeof(OggPageHeader) + hdr.numSegments + 1;
    if (strncmp(magic, "FLAC", 4) == 0) {
        codec.type = Codec::kCodecFlac;
    } else if (strncmp(magic, "vorbis", 7) == 0) {
        codec.type = Codec::kCodecVorbis;
    } else {
        codec.type = Codec::kCodecUnknown;
        return AudioNode::kErrNoCodec;
    }
    return AudioNode::kNoError;
}

#endif
