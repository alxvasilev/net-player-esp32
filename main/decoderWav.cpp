#include "decoderWav.hpp"
#include <arpa/inet.h>

const char* TAG = "wavdec";

DecoderWav::DecoderWav(DecoderNode& parent, AudioNode& src, StreamFormat fmt)
: Decoder(parent, src, fmt)
{
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
        if (strncasecmp(chunkHdr->type, "data", 4) == 0) {
            return rptr + sizeof(ChunkHeader) - pkt.data;
        }
        rptr += sizeof(ChunkHeader) + chunkHdr->size; // point to next chunk
        if (rptr >= end) {
            ESP_LOGE(TAG, "Next chunk spans outside packet, to offset %d", rptr - pkt.data);
            return 0;
        }
    }
}
template<typename T>
void DecoderWav::selectOutput16or32() {
    if (outputFormat.isBigEndian()) {
        mOutputFunc = &DecoderWav::outputWithNewPacket<T, sizeof(T) * 8, true>;
        mOutputInSituFunc = &DecoderWav::outputSwapBeToLeInSitu<T>;
    }
    else {
        mOutputFunc = &DecoderWav::outputWithNewPacket<T, sizeof(T) * 8, false>;
        mOutputInSituFunc = &DecoderWav::outputNoChange;
    }
}
bool DecoderWav::setupOutput()
{
    mPartialInSampleBytes = 0;
    int bps = outputFormat.bitsPerSample();
    int nChans = mNumChans = outputFormat.numChannels();
    mInBytesPerSample = (bps * nChans) / 8;
    if (bps == 16) {
        selectOutput16or32<int16_t>();
    }
    else if (bps == 32) {
        selectOutput16or32<int32_t>();
    }
    else if (bps == 24) {
        mOutputInSituFunc = nullptr;
        mOutputFunc = outputFormat.isBigEndian()
            ? &DecoderWav::outputWithNewPacket<int32_t, 24, true>
            : &DecoderWav::outputWithNewPacket<int32_t, 24, false>;
    }
    else if (bps == 8) {
        mOutputInSituFunc = nullptr;
        mOutputFunc = &DecoderWav::outputWithNewPacket<int16_t, 8>;
    }
    else {
        return false;
    }
    ESP_LOGI(TAG, "Audio format: %d-bit %s-endian %.1fkHz %s",
        bps, outputFormat.isBigEndian() ? "big" : "little", (float)outputFormat.sampleRate() / 1000,
        nChans == 1 ? "mono" : "stereo");
    mParent.codecOnFormatDetected(outputFormat, outputFormat.bitsPerSample());
    return true;
}
StreamEvent DecoderWav::decode(AudioNode::PacketResult& dpr)
{
    auto evt = mSrcNode.pullData(dpr);
    if (evt) {
        return evt;
    }
    auto& pkt = dpr.dataPacket();
    if (!mOutputFunc) { // first packet
        auto codec = outputFormat.codec().type;
        if (codec == Codec::kCodecWav) {
            int sampleOffs = parseWavHeader(pkt);
            if (!sampleOffs) {
                return kErrDecode;
            }
            myassert(sampleOffs > 0);
            myassert(mOutputFunc);
            int len = pkt.dataLen - sampleOffs;
            myassert(len > 0);
            memmove(pkt.data, pkt.data + sampleOffs, len);
            pkt.dataLen = len;
        }
        else if (codec == Codec::kCodecPcm) {
            if (!setupOutput()) {
                return kErrDecode;
            }
        } else {
            assert(false);
        }
        myassert(mOutputFunc);
        myassert(!mPartialInSampleBytes);
        myassert(mInBytesPerSample);
    }
    bool ok;
    if (mOutputInSituFunc && !mPartialInSampleBytes) {
        mPartialInSampleBytes = pkt.dataLen % mInBytesPerSample;
        if (mPartialInSampleBytes) {
            pkt.dataLen -= mPartialInSampleBytes;
            memcpy(mPartialInSampleBuf, pkt.data + pkt.dataLen, mPartialInSampleBytes);
        }
        (this->*mOutputInSituFunc)(pkt);
        ok = mParent.codecPostOutput((DataPacket*)dpr.packet.release());
    }
    else {
        //pkt.logData(40, "input", 40);
        ok = (this->*mOutputFunc)(pkt.data, pkt.dataLen);
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
// byte order transforms
template<>
int16_t transformSample<int16_t, 16, true>(uint8_t* sample)
{
    return ntohs(*((int16_t*)sample));
}
template<>
int32_t transformSample<int32_t, 32, true>(uint8_t* sample)
{
    return ntohs(*((int32_t*)sample));
}
// dummy transforms
template<>
int16_t transformSample<int16_t, 16, false>(uint8_t* sample)
{
    return *(int16_t*)sample;
}
template<>
int32_t transformSample<int32_t, 32, false>(uint8_t* sample)
{
    return *(int32_t*)sample;
}

template <typename T, int Bps, bool BigEndian=false>
bool DecoderWav::outputWithNewPacket(char* input, int len)
{
    enum { kInBytesPerChannel = Bps / 8 };
    // reserve space for adding a partial sample from previous packet (mPartialInSampleBuffer)
    DataPacket::unique_ptr out(DataPacket::create(((len / mInBytesPerSample + 2) * 4 * mNumChans)));
    out->flags |= StreamPacket::kFlagHasSpaceFor32Bit;
    auto wptr = (T*)out->data;
    if (mPartialInSampleBytes) {
        int firstByteCount = mInBytesPerSample - mPartialInSampleBytes;
        memcpy(mPartialInSampleBuf + mPartialInSampleBytes, input, firstByteCount);
        input += firstByteCount;
        len -= firstByteCount;
        *(wptr++) = transformSample<T, Bps, BigEndian>((uint8_t*)mPartialInSampleBuf);
        if (mNumChans == 2) { // two channels per sample
            *(wptr++) = transformSample<T, Bps, BigEndian>((uint8_t*)mPartialInSampleBuf + kInBytesPerChannel);
        }
    }
    mPartialInSampleBytes = len % mInBytesPerSample;
    if (mPartialInSampleBytes) {
        len -= mPartialInSampleBytes;
        memcpy(mPartialInSampleBuf, input + len, mPartialInSampleBytes);
    }
    myassert((len % mInBytesPerSample) == 0);
    uint8_t* end = (uint8_t*)input + len;
    for (auto rptr = (uint8_t*)input; rptr < end; rptr += kInBytesPerChannel) {
        *(wptr++) = transformSample<T, Bps, BigEndian>(rptr);
    }
    out->dataLen = (char*)wptr - out->data;
    return mParent.codecPostOutput(out.release());
}

template <typename T>
void DecoderWav::outputSwapBeToLeInSitu(DataPacket& pkt)
{
    auto end = (T*)(pkt.data + pkt.dataLen);
    for (T* sample = (T*)pkt.data; sample < end; sample++) {
        *sample = transformSample<T, sizeof(T) * 8, true>((uint8_t*)sample);
    }
}
