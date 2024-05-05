#include "decoderWav.hpp"
#include <arpa/inet.h>

const char* TAG = "wavdec";

DecoderWav::DecoderWav(DecoderNode& parent, AudioNode& src, StreamFormat fmt)
: Decoder(parent, src)
{
    outputFormat = fmt;
    if (fmt.codec().type == Codec::kCodecPcm) {
        assert(fmt.sampleRate() != 0 && fmt.bitsPerSample() != 0);
    }
}
#pragma pack (push, 1)
struct ChunkHeader
{
    char type[4];
    uint32_t size; // Bytes after this value
};
struct RiffHeader
{
    // RIFF Header
    ChunkHeader chunkHdr;
    char riffType[4]; // Contains "WAVE"
};
enum: uint16_t { WAVE_FORMAT_PCM = 0x01, WAVE_FORMAT_EXTENSIBLE = 0xfffe };
struct FmtHeader { // WAVEFORMATEX
    // Format Header
    ChunkHeader chunkHdr; // type is "fmt " (includes trailing space)
    uint16_t formatId; // Should be 1 or 0xFEFF for PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t avgBytesPerSec; // Number of bytes per second. sample_rate * num_channels * Bytes Per Sample
    uint16_t blockAlign; // num_channels * Bytes Per Sample
    uint16_t bitsPerSample; // Number of bits per sample
};

struct WavHeader
{
    RiffHeader riffHdr;
    FmtHeader fmtHeader;
};
#pragma pack(pop)

#define CHECK_CHUNK_TYPE(ptr, str)                                          \
if (strncmp(ptr, str, 4) != 0) {                                            \
    ESP_LOGW(TAG, "Chunk type is not the expected '%s' string", str);       \
    return 0;                                                               \
}


int DecoderWav::parseWavHeader(DataPacket& pkt)
{
    if (pkt.dataLen < sizeof(WavHeader) + 100) {
        ESP_LOGW(TAG, "First packet of WAV stream is smaller than the WAV header");
        return 0;
    }
    WavHeader& wavHdr = *reinterpret_cast<WavHeader*>(pkt.data);
    CHECK_CHUNK_TYPE(wavHdr.riffHdr.chunkHdr.type, "RIFF");
    CHECK_CHUNK_TYPE(wavHdr.riffHdr.riffType, "WAVE");
    CHECK_CHUNK_TYPE(wavHdr.fmtHeader.chunkHdr.type, "fmt ");
    auto& wfmt = wavHdr.fmtHeader;
    if (wfmt.formatId != WAVE_FORMAT_PCM && wfmt.formatId != WAVE_FORMAT_EXTENSIBLE) {
        ESP_LOGW(TAG, "Unexpected WAV format code 0x%X", wfmt.formatId);
        return 0;
    }
    auto bps = wfmt.bitsPerSample;
    if (bps != 8 && bps != 16 && bps != 24 && bps != 32) {
        ESP_LOGW(TAG, "Invalid bits per sample %u in WAV header", bps);
        return 0;
    }
    outputFormat.setBitsPerSample(bps);
    outputFormat.setSampleRate(wfmt.sampleRate);
    outputFormat.setNumChannels(wfmt.numChannels);
    if (wfmt.blockAlign != wfmt.numChannels * wfmt.bitsPerSample / 8) {
        ESP_LOGW(TAG, "blockAlign does not match bps * numChans");
        return 0;
    }
    //mInputBytesPerSample = wfmt.blockAlign;
    if (!setupOutput()) {
        return 0;
    }
    const char* end = pkt.data + pkt.dataLen;
    // point to first byte after the fmt chunk
    // wfmt.chunkHdr.size - 16 = number of extra bytes in fmt chunk
    const char* rptr = pkt.data + sizeof(WavHeader) + ((int)wfmt.chunkHdr.size - 16);
    if (rptr >= end) {
        ESP_LOGE(TAG, "Format chunk spans outside packet, to offset %d", rptr - pkt.data);
        return 0;
    }
    // === discard the rest till the actual PCM data
    for(;;) {
        auto chunkHdr = reinterpret_cast<const ChunkHeader*>(rptr);
        rptr += sizeof(ChunkHeader) + chunkHdr->size; // point to next chunk
        if (rptr >= end) {
            ESP_LOGE(TAG, "Next chunk spans outside packet, to offset %d", rptr - pkt.data);
            return 0;
        }
        if (strncasecmp(chunkHdr->type, "data", 4) == 0) {
            break;
        }
    }
    return rptr - pkt.data;
}
bool DecoderWav::setupOutput()
{
    int bps = outputFormat.bitsPerSample();
    int nChans = outputFormat.numChannels();
    if (bps == 16 || bps == 32) {
        mOutputInSitu = true;
        if (outputFormat.isBigEndian()) {
            mOutputFunc = (bps == 16)
                ? &DecoderWav::outputSwapBeToLe16
                : &DecoderWav::outputSwapBeToLe32;
            ESP_LOGI(TAG, "Converting PCM data from big-endian");
        }
        else {
            mOutputFunc = nullptr;
            ESP_LOGI(TAG, "Forwarding PCM data without conversion");
        }
    }
    else if (bps == 24) {
        mOutputInSitu = false;
        mOutputFunc = outputFormat.isBigEndian()
            ? &DecoderWav::output<int32_t, 24, true>
            : &DecoderWav::output<int32_t, 24, false>;
    }
    else if (bps == 8) {
        mOutputInSitu = false;
        mOutputFunc = &DecoderWav::output<int16_t, 8>;
    }
    else {
        return false;
    }
    ESP_LOGI(TAG, "Audio format: %d-bit %.1f kHz %s", bps, (float)outputFormat.sampleRate() / 1000,
        nChans == 1 ? "mono" : "stereo");
    mParent.codecOnFormatDetected(outputFormat);
    return true;
}
StreamEvent DecoderWav::decode(AudioNode::PacketResult& dpr)
{
    auto evt = mSrcNode.pullData(dpr);
    if (evt) {
        return evt;
    }
    auto& pkt = dpr.dataPacket();
    int sampleOffs = 0;
    if (!mOutputFunc) { // first packet
        auto codec = outputFormat.codec().type;
        if (codec == Codec::kCodecWav) {
            sampleOffs = parseWavHeader(pkt);
            if (!sampleOffs) {
                return kErrDecode;
            }
        } else if (codec == Codec::kCodecPcm) {
            if (!setupOutput()) {
                return kErrDecode;
            }
        } else {
            assert(false);
        }
        assert(mOutputFunc);
    }
    bool ok;
    if (mOutputInSitu) {
        (this->*mOutputFunc)(pkt.data + sampleOffs, pkt.dataLen - sampleOffs);
        ok = mParent.codecPostOutput(&pkt);
    }
    else {
        ok = (this->*mOutputFunc)(pkt.data + sampleOffs, pkt.dataLen - sampleOffs);
    }
    return ok ? kNoError : kErrStreamStopped;
}
template<typename T, int Bps, bool BigEndian>
T transformSample(uint8_t* input);

template<>
int32_t transformSample<int32_t, 24, false>(uint8_t* input)
{
    int32_t sample = *input << 8;
    sample |= *(input+1) << 16;
    sample |= *(input+2) << 24;
    return sample >> 8;
}

template<>
int32_t transformSample<int32_t, 24, true>(uint8_t* input)
{
    int32_t sample = *input << 24;
    sample |= *(input+1) << 16;
    sample |= *(input+2) << 8;
    return sample >> 8;
}

template<>
int16_t transformSample<int16_t, 8, false>(uint8_t* input)
{
    return (*input << 8) >> 8;
}

template <typename T, int Bps, bool BigEndian=false>
bool DecoderWav::output(char* input, int len)
{
    enum { kInputBytesPerSample = Bps / 8 };
    DataPacket::unique_ptr out(DataPacket::create(len * sizeof(T) / kInputBytesPerSample));
    auto wptr = (T*)out->data;
    uint8_t* end = (uint8_t*)input + len;
    for (auto rptr = (uint8_t*)input; rptr < end; wptr++, rptr += kInputBytesPerSample) {
        *wptr = transformSample<T, Bps, BigEndian>(rptr);
    }
    return mParent.codecPostOutput(out.release());
}
bool DecoderWav::outputSwapBeToLe16(char* input, int len)
{
    auto end = (int16_t*)(input + len);
    for (int16_t* sample = (int16_t*)input; sample < end; sample++) {
        *sample = ntohs(*sample);
    }
    return true;
}
bool DecoderWav::outputSwapBeToLe32(char* input, int len)
{
    auto end = (int32_t*)(input + len);
    for (int32_t* sample = (int32_t*)input; sample < end; sample++) {
        *sample = ntohl(*sample);
    }
    return true;
}
