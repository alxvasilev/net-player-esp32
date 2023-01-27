#include "decoderWav.hpp"
const char* TAG = "wavdec";

DecoderWav::DecoderWav(DecoderNode& parent, AudioNode& src, StreamFormat fmt)
: Decoder(parent, src)
{
    outputFormat = fmt;
    if (fmt.codec().type == Codec::kCodecWav) {
        mNeedWavHeader = true;
    } else {
        mNeedWavHeader = false;
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
    return AudioNode::kErrDecode;                                           \
}


AudioNode::StreamError DecoderWav::parseWavHeader(AudioNode::DataPullReq& dpr)
{
    char buf[sizeof(WavHeader)];
    printf("reading WAV header...\n");
    auto event = mSrcNode.readExact(dpr, sizeof(WavHeader), buf);
    if (event) {
        return event;
    }
    printf("got WAV header\n");
    WavHeader& wavHdr = *reinterpret_cast<WavHeader*>(buf);
    CHECK_CHUNK_TYPE(wavHdr.riffHdr.chunkHdr.type, "RIFF");
    CHECK_CHUNK_TYPE(wavHdr.riffHdr.riffType, "WAVE");
    CHECK_CHUNK_TYPE(wavHdr.fmtHeader.chunkHdr.type, "fmt ");
    auto& wfmt = wavHdr.fmtHeader;
    if (wfmt.formatId != WAVE_FORMAT_PCM && wfmt.formatId != WAVE_FORMAT_EXTENSIBLE) {
        ESP_LOGW(TAG, "Unexpected WAV format code 0x%X", wfmt.formatId);
        return AudioNode::kErrDecode;
    }
    auto bps = wfmt.bitsPerSample;
    if (bps != 8 && bps != 16 && bps != 24 && bps != 32) {
        ESP_LOGW(TAG, "Invalid bits per sample %u in WAV header", bps);
        return AudioNode::kErrDecode;
    }
    outputFormat.setBitsPerSample(bps);
    outputFormat.setSampleRate(wfmt.sampleRate);
    outputFormat.setNumChannels(wfmt.numChannels);
    if (wfmt.blockAlign != wfmt.numChannels * wfmt.bitsPerSample / 8) {
        ESP_LOGW(TAG, "blockAlign does not match bps * numChans");
        return AudioNode::kErrDecode;
    }
    mInputBytesPerSample = wfmt.blockAlign;
    if (!setupOutput()) {
        return AudioNode::kErrDecode;
    }
    ESP_LOGI(TAG, "Audio format: %d-bit, %.1f kHz", bps, (float)wfmt.sampleRate/1000);
    int extra = (int)wfmt.chunkHdr.size - 16;
    if (extra) {
        printf("reading extra %u bytes of WAVEFORMAT chunk...\n", extra);
        event = mSrcNode.readExact(dpr, extra, nullptr); // discard rest of fmt chunk
        if (event) {
            return event;
        }
    }
    // === discard the rest till the actual PCM data
    ChunkHeader* chunkHdr = reinterpret_cast<ChunkHeader*>(buf);
    for(;;) {
        event = mSrcNode.readExact(dpr, sizeof(ChunkHeader), buf); // read chunk header
        if (event) {
            return event;
        }
        if (strncasecmp(chunkHdr->type, "data", 4) == 0) {
            break;
        }
        // read and discard chunk
        printf("reading chunk %.*s to discard (%u bytes)...\n", 4, chunkHdr->type, chunkHdr->size);
        auto event = mSrcNode.readExact(dpr, chunkHdr->size, nullptr); // discard chunk data
        if (event) {
            return event;
        }
    }
    return AudioNode::kNoError;
}
bool DecoderWav::setupOutput()
{
    int bps = outputFormat.bitsPerSample();
    int nChans = outputFormat.numChannels();
    mInputBytesPerFrame = kSamplesPerRequest * nChans * bps / 8;
    if (bps == 16 || bps == 32) {
        mOutputBytesPerFrame = kSamplesPerRequest * nChans * bps / 8;
        mOutputBuf.freeBuf(); // must already be null
        mOutputFunc = nullptr;
        ESP_LOGI(TAG, "Forwarding PCM data without conversion");
    }
    else if (bps == 24) {
        mOutputBytesPerFrame = kSamplesPerRequest * 4 * nChans;
        mOutputBuf.resize(mOutputBytesPerFrame);
        mOutputFunc = &DecoderWav::output24to32;
    }
    else if (bps == 8) {
        mOutputBytesPerFrame = kSamplesPerRequest * 2 * nChans;
        mOutputBuf.resize(mOutputBytesPerFrame);
        mOutputFunc = &DecoderWav::output8to16;
    }
    else {
        return false;
    }
    return true;
}
AudioNode::StreamError DecoderWav::pullData(AudioNode::DataPullReq& dpr)
{
    if (mNeedWavHeader) {
        mNeedWavHeader = false;
        auto event = parseWavHeader(dpr);
        if (event) {
            return event;
        }
        printf("header parse completed, reading data\n");
    }
    dpr.size = mInputBytesPerFrame;
    auto event = mSrcNode.pullData(dpr);
    if (event) {
        return event;
    }
    mNeedConfirmRead = (mOutputFunc == nullptr);
    if (dpr.size != mInputBytesPerFrame) { // less returned, round to whole samples
        assert(dpr.size < mInputBytesPerFrame);
        dpr.size -= dpr.size % mInputBytesPerSample;
        if (dpr.size == 0) {
            event = mSrcNode.readExact(dpr, mInputBytesPerSample, mSingleSampleBuf);
            if (event) {
                return event;
            }
            dpr.buf = mSingleSampleBuf;
            dpr.size = mInputBytesPerSample;
            mNeedConfirmRead = false;
            printf("received less than 1 sample\n");
        }
    }

    if (!mOutputFunc) {
        dpr.fmt = outputFormat;
        return AudioNode::kNoError;
    }
    (this->*mOutputFunc)(dpr);
    if (dpr.buf != mSingleSampleBuf) {
        mSrcNode.confirmRead(dpr.size);
    }
    dpr.fmt = outputFormat;
    dpr.buf = mOutputBuf.buf();
    dpr.size = mOutputBuf.dataSize();
    return AudioNode::kNoError;
}
void DecoderWav::confirmRead(int size) {
    if (mNeedConfirmRead) {
        mSrcNode.confirmRead(size);
    }
}
void DecoderWav::output24to32(AudioNode::DataPullReq& dpr)
{
    mOutputBuf.setDataSize(dpr.size * 4 / 3);
    auto wptr = (int32_t*)mOutputBuf.buf();
    uint8_t* end = (uint8_t*)dpr.buf + dpr.size;
    for (auto rptr = (uint8_t*)dpr.buf; rptr < end;) {
        int32_t sample = *rptr++ << 8;
        sample |= *rptr++ << 16;
        sample |= *rptr++ << 24;
        *wptr++ = sample >> 8;
    }
}
void DecoderWav::output8to16(AudioNode::DataPullReq& dpr)
{
    mOutputBuf.setDataSize(dpr.size * 2);
    auto wptr = (int16_t*)mOutputBuf.buf();
    uint8_t* end = (uint8_t*)dpr.buf + dpr.size;
    for (auto rptr = (uint8_t*)dpr.buf; rptr < end;) {
        int16_t sample = *rptr++ << 8;
        *wptr++ = sample >> 8;
    }
}
