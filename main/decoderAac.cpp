#include "decoderAac.hpp"
#define HELIX_FEATURE_AUDIO_CODEC_AAC_SBR 1
#include <aacdec.h>

static const char* TAG = "aacdec";

DecoderAac::DecoderAac(DecoderNode& parent, AudioNode& src)
: Decoder(parent, src)
{
    initDecoder();
}
void DecoderAac::initDecoder()
{
    mInputLen = mOutputLen = 0;
    mNextFramePtr = mInputBuf;
    mDecoder = AACInitDecoder();
    if (!mDecoder) {
        ESP_LOGE(TAG, "Out of memory creating AAC decoder.");
    }
}
void DecoderAac::freeDecoder()
{
    if (mDecoder) {
        AACFreeDecoder(mDecoder);
        mDecoder = nullptr;
    }
}
DecoderAac::~DecoderAac()
{
    freeDecoder();
}

void DecoderAac::reset()
{
    freeDecoder();
    initDecoder();
    outputFormat.clear();
}

StreamEvent DecoderAac::decode(AudioNode::PacketResult& dpr)
{
    bool needMoreData = (mInputLen == 0);
    DataPacket::unique_ptr output;
    for(;;) {
        if (needMoreData || mInputLen < kMinAllowedAacInputSize) {
            needMoreData = false;
            if (mNextFramePtr != mInputBuf) {
                memmove(mInputBuf, mNextFramePtr, mInputLen);
                mNextFramePtr = mInputBuf;
            }
            int freeSpace = kInputBufSize - mInputLen;
            if (freeSpace <= 0) {
                ESP_LOGE(TAG, "Can't decode a frame, even though input buffer is full");
                mInputLen = 0;
                mNextFramePtr = mInputBuf;
                return kErrDecode;
            }
            auto event = mSrcNode.pullData(dpr);
            if (event) {
                return event;
            }
            // Existing data starts at mInputBuf
            myassert(dpr.packet);
            auto& packet = dpr.dataPacket();
            myassert(packet.dataLen);
            memcpy(mInputBuf + mInputLen, packet.data, packet.dataLen);
            mInputLen += packet.dataLen;
            dpr.clear();
        }
        if (!output) {
            // output is always 16 bit, reserve space for 32-bit
            output.reset(DataPacket::create(mOutputLen ? mOutputLen * 2 : kOutputMaxSize));
            output->flags |= StreamPacket::kFlagHasSpaceFor32Bit;
        }
        // printf("AACDecode: inLen=%d, offs=%d\n", mInputLen, mNextFramePtr - mInputBuf);
        auto err = AACDecode(mDecoder, &mNextFramePtr, &mInputLen, (int16_t*)output->data);
        if (err == ERR_AAC_INDATA_UNDERFLOW) { // need more data
            ESP_LOGD(TAG, "Decoder underflow");
            needMoreData = true;
            continue;
        }
        else if (err == 0) { // decode success
            if (!mOutputLen) { // we haven't yet initialized output format info
                getStreamFormat();
            }
            if (mOutputLen <= 2048) {
                output->dataLen = mOutputLen;
                return mParent.codecPostOutput(output.release()) ? kNoError : kErrStreamStopped;
            }
            else {
                output->dataLen = 2048;
                auto out2len = mOutputLen - 2048;
                DataPacket::unique_ptr output2(DataPacket::create(out2len * 2));
                memcpy(output2->data, output->data + 2048, out2len);
                output2->dataLen = out2len;
                output2->flags |= StreamPacket::kFlagHasSpaceFor32Bit;
                bool ok = mParent.codecPostOutput(output.release()) && mParent.codecPostOutput(output2.release());
                return ok ? kNoError : kErrStreamStopped;
            }
        }
        else { //err < 0 - error, try to re-sync
            // mNextFramePtr and mInputLen are guaranteed to not be updated if AACDecode() failed
            ESP_LOGW(TAG, "Decode error %d, looking for next sync word", err);
            auto pos = AACFindSyncWord(mNextFramePtr + 1, mInputLen - 1);
            if (pos >= 0) {
                pos += 1; // adjust for +1 start in buffer
                ESP_LOGI(TAG, "Sync word found at %d, discarding data before it and repeating", pos);
                mNextFramePtr += pos;
                mInputLen -= pos;
                continue;
            }
            // can't find frame start, discard everything in buffer and request more data
            ESP_LOGI(TAG, "Can't find sync word, discarding whole buffer and requesting more data");
            mNextFramePtr = mInputBuf;
            if (*(mNextFramePtr + mInputLen - 1) == 0xff) {
                // this can be the first byte of a sync word, preserve it
                *mInputBuf = 0xff;
                mInputLen = 1;
            } else {
                mInputLen = 0;
            }
            needMoreData = true;
            continue;
        }
        myassert(false); // shouldn't reach here
    }
}

void DecoderAac::getStreamFormat()
{
    AACFrameInfo info;
    AACGetLastFrameInfo(mDecoder, &info);
    outputFormat.setCodec(Codec::kCodecAac);
    outputFormat.setSampleRate(info.sampRateOut);
    outputFormat.setNumChannels(info.nChans);
    outputFormat.setBitsPerSample(16);
    mOutputLen = info.outputSamps * sizeof(int16_t);
    bool isSbr = (info.sampRateOut != info.sampRateCore);
    if (isSbr) {
        outputFormat.codec().mode = Codec::kAacModeSbr;
    }
    ESP_LOGW(TAG, "AAC%s 16-bit %s, %d Hz, %d bps, %d samples/frame",
        isSbr ? " SBR" : "",
        info.nChans == 2 ? "stereo" : "mono", info.sampRateOut,
        info.bitRate, info.outputSamps);
    mParent.codecOnFormatDetected(outputFormat, 16);
}
