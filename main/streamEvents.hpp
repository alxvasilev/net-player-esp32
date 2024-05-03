#ifndef STREAM_EVENTS_HPP
#define STREAM_EVENTS_HPP

#include "audioNode.hpp"

struct StreamEvent: IVirtDestructor {
    int64_t streamPos;
    union {
        StreamFormat fmt;
        void* data;
    };
    uint16_t streamId;
    AudioNode::StreamError type;
    StreamEvent(AudioNode::StreamError aType, StreamFormat aFmt, uint32_t aStreamId)
        :streamPos(-1), fmt(aFmt), streamId(aStreamId), type(aType) {}
    StreamEvent(AudioNode::StreamError aType, uint32_t aStreamId)
        :streamPos(-1), data(nullptr), streamId(aStreamId), type(aType) {}
protected:
    StreamEvent(AudioNode::StreamError aType, void* aData, int64_t aPos=-1)
        :streamPos(aPos), data(aData), streamId(0), type(aType) {}
};
struct TitleChangeEvent: public StreamEvent {
    std::string title;
    TitleChangeEvent(const char* aTitle, int64_t aPos)
        : StreamEvent(AudioNode::kEvtTitleChanged, &title, aPos) {
        title.assign(aTitle);
    }
};
#endif
