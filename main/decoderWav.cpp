#include "decoderWav.hpp"
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
    return AudioNode::kErrDecode;                                           \
}


AudioNode::StreamError DecoderWav::parseWavHeader(AudioNode::DataPullReq& dpr)
{
    char buf[sizeof(WavHeader)];
    auto event = mSrcNode.readExact(dpr, sizeof(WavHeader), buf);
    if (event) {
        return event;
    }
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
    int extra = (int)wfmt.chunkHdr.size - 16;
    if (extra) {
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
        mOutputBuf.freeBuf(); // must already be null
        mOutputBytesPerFrame = kSamplesPerRequest * nChans * bps / 8;
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
        mOutputBytesPerFrame = kSamplesPerRequest * 4 * nChans;
        mOutputBuf.resize(mOutputBytesPerFrame);
        mOutputFunc = outputFormat.isBigEndian()
            ? &DecoderWav::transformAudioData<int32_t, 24, true>
            : &DecoderWav::transformAudioData<int32_t, 24, false>;
    }
    else if (bps == 8) {
        mOutputBytesPerFrame = kSamplesPerRequest * 2 * nChans;
        mOutputBuf.resize(mOutputBytesPerFrame);
        mOutputFunc = &DecoderWav::transformAudioData<int16_t, 8>;
    }
    else {
        return false;
    }
    ESP_LOGI(TAG, "Audio format: %d-bit %.1f kHz %s", bps, (float)outputFormat.sampleRate() / 1000,
        nChans == 1 ? "mono" : "stereo");
    return true;
}
AudioNode::StreamError DecoderWav::pullData(AudioNode::DataPullReq& dpr)
{
    if (!mInputBytesPerFrame) {
        auto codec = outputFormat.codec().type;
        if (codec == Codec::kCodecWav) {
            auto event = parseWavHeader(dpr);
            if (event) {
                return event;
            }
        } else if (codec == Codec::kCodecPcm) {
            if (!setupOutput()) {
                return AudioNode::kErrDecode;
            }
        } else {
            assert(false);
        }
        assert(mInputBytesPerFrame);
    }
    dpr.size = mInputBytesPerFrame;
    auto event = mSrcNode.pullData(dpr);
    if (event) {
        return event;
    }
    mNeedConfirmRead = !mOutputBuf;
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
        }
    }

    if (mOutputFunc) {
        (this->*mOutputFunc)(dpr);
    }
    dpr.fmt = outputFormat;
    assert(dpr.size);
    if (!mOutputBuf) { // caller needs to confirm the read
        return AudioNode::kNoError;
    }
    if (dpr.buf != mSingleSampleBuf) { // read to mSingleSampleBuf is auto-confirmed already
        mSrcNode.confirmRead(dpr.size);
    }
    dpr.buf = mOutputBuf.buf();
    dpr.size = mOutputBuf.dataSize();
    return AudioNode::kNoError;
}
void DecoderWav::confirmRead(int size) {
    if (mNeedConfirmRead) {
        mSrcNode.confirmRead(size);
    }
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
void DecoderWav::transformAudioData(AudioNode::DataPullReq& audio)
{
    enum { kInputBytesPerSample = Bps / 8 };
    mOutputBuf.setDataSize(audio.size * sizeof(T) / kInputBytesPerSample);
    auto wptr = (T*)mOutputBuf.buf();
    uint8_t* end = (uint8_t*)audio.buf + audio.size;
    for (auto rptr = (uint8_t*)audio.buf; rptr < end; wptr++, rptr += kInputBytesPerSample) {
        *wptr = transformSample<T, Bps, BigEndian>(rptr);
    }
}
void DecoderWav::outputSwapBeToLe16(AudioNode::DataPullReq& audio)
{
    auto end = (int16_t*)(audio.buf + audio.size);
    for (int16_t* sample = (int16_t*)audio.buf; sample < end; sample++) {
        *sample = ntohs(*sample);
    }
}
void DecoderWav::outputSwapBeToLe32(AudioNode::DataPullReq& audio)
{
    auto end = (int32_t*)(audio.buf + audio.size);
    for (int32_t* sample = (int32_t*)audio.buf; sample < end; sample++) {
        *sample = ntohl(*sample);
    }
}
