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

AudioNode::StreamError detectOggCodec(AudioNode& src, AudioNode::DataPullReq& info)
{
    enum { kPrefetchAmount = sizeof(OggPageHeader) + kMaxNumSegments + 10 }; // we need the first ~7 bytes in the segment
    unique_ptr_mfree<char> buf((char*)utils::mallocTrySpiram(kPrefetchAmount));
    int nRead = 0;
    do {
        AudioNode::DataPullReq dpr(kPrefetchAmount - nRead);
        auto err = src.pullData(dpr);
        if (err != AudioNode::kNoError) {
            return err;
        }
        assert(nRead + dpr.size <= kPrefetchAmount);
        memcpy(buf.get() + nRead, dpr.buf, dpr.size);
        src.confirmRead(dpr.size);
        nRead += dpr.size;
    } while(nRead < kPrefetchAmount);
    OggPageHeader& hdr = *((OggPageHeader*)buf.get());
    if (hdr.fourCC != fourccLittleEndian("OggS")) {
        ESP_LOGW("OGG", "Fourcc %x doesn't match 'OggS' (%x)", hdr.fourCC, fourccLittleEndian("OggS"));
        return AudioNode::kErrDecode;
    }
    if (hdr.numSegments > kMaxNumSegments) {
        ESP_LOGW("OGG", "More than expected segments in Ogg page: %d", hdr.numSegments);
        return AudioNode::kErrDecode;
    }
    const char* magic = buf.get() + sizeof(OggPageHeader) + hdr.numSegments + 1;
    if (strncmp(magic, "FLAC", 4) == 0) {
        info.codec = kCodecOggFlac;
    } else if (strncmp(magic, "vorbis", 7) == 0) {
        info.codec = kCodecOggVorbis;
    } else {
        return AudioNode::kErrNoCodec;
    }
    info.buf = buf.release();
    info.size = kPrefetchAmount;
    return AudioNode::kNoError;
}

#endif
