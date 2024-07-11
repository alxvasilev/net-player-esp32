#include <sys/unistd.h>
#include <string.h>
#include "streamDefs.hpp"
int StreamFormat::prefillAmount() const
{
    switch (codec().type) {
        case Codec::kCodecMp3:
            return 48 * 1024;
        case Codec::kCodecAac:
            return 32 * 1024;
        case Codec::kCodecFlac: {
            auto val = ((sampleRate() > 64000) ? 200 : 100) * 1024;
            return bitsPerSample() > 16 ? val * 2 : val;
        }
        case Codec::kCodecWav:
        case Codec::kCodecPcm: {
            auto val = bitsPerSample() * sampleRate() * numChannels() / 8;
            return val ? val : 160 * 1024;
        }
        default:
            return 64 * 1024;
    }
}
int16_t StreamFormat::netRecvSize() const
{
    switch (codec().type) {
        case Codec::kCodecWav:
            return (sampleRate() > 48000 || bitsPerSample() > 16) ? 8192 : 4096;
        default:
            return (codec().transport == Codec::kTransportOgg) ? 4096 : 2048;
    }
}
const char* Codec::toString() const
{
    switch (type) {
        case kCodecMp3: return "mp3";
        case kCodecAac: {
            if (transport == kTransportMpeg) {
                return mode ? "m4a(sbr)" : "m4a";
            } else {
                return mode ? "aac(sbr)" : "aac";
            }
        }
        case kCodecFlac: return transport ? "ogg/flac" : "flac";
        case kCodecOpus: return "opus"; // transport is always ogg
        case kCodecVorbis: return "vorbis"; // transport is always ogg
        case kCodecWav: return "wav";
        case kCodecPcm: return "pcm";
        case kCodecUnknown:
        default:
            if (transport == kTransportOgg) {
                return "unknown/ogg";
            }
            else if (transport == kTransportMpeg) {
                return "unknown/mpeg";
            }
            return "(unknown)";
    }
}
const char* Codec::fileExt() const {
    switch (type) {
        case kCodecMp3: return "mp3";
        case kCodecAac: return (transport == kTransportMpeg) ? "m4a" : "aac";
        case kCodecFlac: return "flac";
        case kCodecOpus: return "opus";
        case kCodecVorbis: return "ogg";
        case kCodecWav: return "wav";
        default: return "unk";
    }
}
#define STRMEVT_CASE(name) case name: return #name
const char* streamEventToStr(StreamEvent evt) {
    switch (evt) {
        STRMEVT_CASE(kEvtStreamEnd);
        STRMEVT_CASE(kEvtStreamChanged);
        STRMEVT_CASE(kEvtTitleChanged);
        STRMEVT_CASE(kErrStreamStopped);
        STRMEVT_CASE(kErrNoCodec);
        STRMEVT_CASE(kErrDecode);
        STRMEVT_CASE(kErrNotFound);
        STRMEVT_CASE(kErrStreamFmt);
        STRMEVT_CASE(kEvtData);
        default: return "(invalid)";
    }
}
