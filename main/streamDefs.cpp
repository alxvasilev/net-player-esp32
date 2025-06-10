#include <sys/unistd.h>
#include <string.h>
#include "streamDefs.hpp"
#include "utils-parse.hpp"
#include <esp_log.h>
#define logErr(fmt,...) ESP_LOGI("mime-parse", fmt, ##__VA_ARGS__)

int StreamFormat::prefillAmount() const
{
    enum { kHalfSecs = 3 };
    switch (codec().type) {
        case Codec::kCodecMp3:
            // kbits/sec * (1024 / 8) * (halfSec / 2)
            return (256 * 1024 * kHalfSecs) >> 4;
        case Codec::kCodecVorbis:
            return (160 * 1024 * kHalfSecs) >> 4;
        case Codec::kCodecAac:
            return (100 * 1024 * kHalfSecs) >> 4;
        case Codec::kCodecFlac: {
            auto kbps = (sampleRate() > 48000) ? 1400 : 1000;
            if(bitsPerSample() > 16) {
                kbps = (kbps * 3) >> 1;
            }
            return (kHalfSecs * 1024 * kbps) >> 4;
        }
        case Codec::kCodecWav:
        case Codec::kCodecPcm: {
            auto val = (bitsPerSample() * sampleRate() * numChannels() * kHalfSecs) >> 4;
            return val ? val : (16 * 2 * 2 * 44100 * kHalfSecs) >> 4;
        }
        case Codec::KCodecSbc:
            return 2048; // Bluetooth sink should do minimum buffering
        default:
            return (256 * 1024 * kHalfSecs) >> 4;
    }
}
int16_t StreamFormat::rxChunkSize() const
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
        case KCodecSbc: return "sbc";
        case kCodecUnknown:
        default:
            if (transport == kTransportOgg) {
                return "unk/ogg";
            }
            else if (transport == kTransportMpeg) {
                return "unk/mpeg";
            }
            return "(unk)";
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
StreamFormat StreamFormat::fromMimeType(const char* content_type)
{
    if (strcasecmp(content_type, "audio/mp3") == 0 ||
        strcasecmp(content_type, "audio/mpeg") == 0) {
        return Codec::kCodecMp3;
    }
    else if (strcasecmp(content_type, "audio/aac") == 0 ||
        strcasecmp(content_type, "audio/x-aac") == 0 ||
//      strcasecmp(content_type, "audio/mp4") == 0 ||
        strcasecmp(content_type, "audio/aacp") == 0) {
        return Codec::kCodecAac;
    }
    else if (strcasecmp(content_type, "audio/flac") == 0 ||
             strcasecmp(content_type, "audio/x-flac") == 0) {
        return Codec::kCodecFlac;
    }
    else if (strcasecmp(content_type, "audio/ogg") == 0 ||
             strcasecmp(content_type, "application/ogg") == 0) {
        return Codec(Codec::kCodecUnknown, Codec::kTransportOgg);
    }
    else if (strcasecmp(content_type, "audio/wav") == 0 ||
             strcasecmp(content_type, "audio/x-wav") == 0) {
        return Codec::kCodecWav;
    }
    else if (strncasecmp(content_type, "audio/L16", 9) == 0) {
        return parseLpcmContentType(content_type, 16);
    }
    else if (strncasecmp(content_type, "audio/L24", 9) == 0) {
        return parseLpcmContentType(content_type, 24);
    }
    else if (strcasecmp(content_type, "audio/opus") == 0) {
        return Codec::kCodecOpus;
    }
    else if (strcasecmp(content_type, "audio/x-mpegurl") == 0 ||
        strcasecmp(content_type, "application/vnd.apple.mpegurl") == 0 ||
        strcasecmp(content_type, "vnd.apple.mpegURL") == 0) {
        return Codec::kPlaylistM3u8;
    }
    else if (strncasecmp(content_type, "audio/x-scpls", strlen("audio/x-scpls")) == 0) {
        return Codec::kPlaylistPls;
    }
    return Codec::kCodecUnknown;
}
StreamFormat StreamFormat::parseLpcmContentType(const char* ctype, int bps)
{
    static const char* kMsg = "Error parsing audio/Lxx";
    ctype = strchr(ctype, ';');
    if (!ctype) {
        logErr("%s: No semicolon found", kMsg);
        return Codec::kCodecUnknown;
    }
    ctype++;
    auto len = strlen(ctype) + 1;
    auto copy = (char*)malloc(len);
    memcpy(copy, ctype, len);
    KeyValParser params(copy, len, true);
    if (!params.parse(';', '=', KeyValParser::kTrimSpaces)) {
        logErr("%s params", kMsg);
        return Codec::kCodecUnknown;
    }
    auto sr = params.intVal("rate", 0);
    if (sr == 0) {
        logErr("%s samplerate", kMsg);
        return Codec::kCodecUnknown;
    }
    auto chans = params.intVal("channels", 1);
    StreamFormat fmt(Codec::kCodecPcm, sr, 16, chans);
    auto endian = params.strVal("endianness");
    if (!endian || strcasecmp(endian.str, "little-endian") != 0) {
        fmt.setBigEndian(true);
    }
    return fmt;
}
