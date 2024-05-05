#include <sys/unistd.h>
#include <string.h>
#include "streamDefs.hpp"

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
        case kCodecUnknown: return "none";
        default: return "(unknown)";
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
const char* streamEventToStr(StreamEvent evt) {
    switch (evt) {
        case kEvtStreamEnd: return "kEvtStreamEnd";
        case kEvtStreamChanged: return "kEvtStreamChanged";
        case kEvtTitleChanged: return "kEvtTitleChanged";
        case kErrStreamStopped: return "kErrStreamStopped";
        case kErrNoCodec: return "kErrNoCodec";
        case kErrDecode: return "kErrDecode";
        case kErrStreamFmt: return "kErrStreamFmt";
        case kEvtData: return "kEvtData";
        default: return "(invalid)";
    }
}
