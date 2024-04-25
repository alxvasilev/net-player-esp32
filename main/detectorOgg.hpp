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

StreamError detectOggCodec(Codec& codec, StreamDataItem& chunk)
{
    enum { kPrefetchAmount = sizeof(OggPageHeader) + kMaxNumSegments + 10 }; // we need the first ~7 bytes in the segment
    if (chunk.dataSize < kPrefetchAmount) {
        ESP_LOGW("OGG", "Not enough bytes in first stream chunk: required %d, available: %u",
            kPrefetchAmount, chunk.dataSize);
        return kErrInvalidFirstChunk;
    }
    OggPageHeader& hdr = *((OggPageHeader*)chunk.data);
    if (hdr.fourCC != fourccLittleEndian("OggS")) {
        ESP_LOGW("OGG", "Fourcc %x doesn't match 'OggS' (%x)", hdr.fourCC, fourccLittleEndian("OggS"));
        return kErrDecode;
    }
    if (hdr.numSegments > kMaxNumSegments) {
        ESP_LOGW("OGG", "More than expected segments in Ogg page: %d", hdr.numSegments);
        return kErrDecode;
    }
    const char* magic = chunk.data + sizeof(OggPageHeader) + hdr.numSegments + 1;
    if (strncmp(magic, "FLAC", 4) == 0) {
        codec.type = Codec::kCodecFlac;
    } else if (strncmp(magic, "vorbis", 7) == 0) {
        codec.type = Codec::kCodecVorbis;
    } else {
        codec.type = Codec::kCodecUnknown;
        return kErrNoCodec;
    }
    return kNoError;
}

#endif
