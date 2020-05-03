#ifndef DECODER_NODE_HPP
#define DECODER_NODE_HPP
#include "audioNode.hpp"
#include <mad.h>

class Decoder
{
protected:
    StreamFormat mOutputFormat;
    uint8_t& mVolume;
public:
    Decoder(uint8_t& vol): mVolume(vol){}
    virtual ~Decoder() {}
    virtual esp_codec_type_t type() const = 0;
    /** returns an approximate amount of data that should be provided until the next
     * decode has enough data to complete. May be negative in case the data in the buffer
     * is more than enough
     */
    virtual int inputBytesNeeded() = 0;
    /** Decodes the provided data in buf, and sets the value of `size` to the number
     * of consumed bytes. Outputs pcm data to the internal PCM buffer, returning
     * the number of bytes written to the PCM buffer in case a frame was decoded, or
     * a negative DecodeResult error code
     */
    virtual int decode(const char* buf, int size) = 0;
    virtual char* outputBuf() = 0;
    virtual void reset() = 0;
    StreamFormat outputFmt() const { return mOutputFormat; }
};

class DecoderNode: public AudioNode, public IAudioVolume
{
protected:
    enum { kInputBufSize = 3000 };
    Decoder* mDecoder = nullptr;
    bool mFormatChangeCtr;
    uint8_t mVolume = kVolumeDiv; // 100% volume
    bool createDecoder(esp_codec_type_t type);
    bool changeDecoder(esp_codec_type_t type);
public:
    enum { kVolumeDiv = 64 };
    DecoderNode(): AudioNode("decoder"){}
    virtual Type type() const { return kTypeDecoder; }
    virtual StreamError pullData(DataPullReq& dpr, int timeout);
    virtual void confirmRead(int size) {}
    virtual ~DecoderNode() {}
    // volume interface
    virtual uint8_t getVolume() const;
    virtual void setVolume(uint8_t vol);
    virtual void fadeOut() {}
    friend class Decoder;
};

#endif
