#ifndef TRACK_RECORDER_HPP
#define TRACK_RECORDER_HPP
#include <string>
#include <stdio.h>
#include "audioNode.hpp"

class TrackRecorder
{
public:
    struct IEventHandler
    {
        virtual void onRecord(bool) = 0;
    };
protected:
    std::string mRootPath;
    std::string mStationName;
    std::string mCurrTrackName;
    FILE* mSinkFile = nullptr;
    bool mLastNotifiedRec = false;
    void commit();
    std::string sinkFileName() const { return mRootPath + "/stream.dat"; }
    std::string trackNameToPath(const std::string& trackName) const;
    bool createDirIfNotExist(const char* dirname) const;
    void notifyRecording();
    IEventHandler* mEventHandler = nullptr;
public:
    TrackRecorder(const char* rootPath);
    void setStation(const char* name);
    void onNewTrack(const char* trackName, StreamFormat fmt);
    void onData(const void* data, int dataLen);
    void abortTrack();
    void setEventHandler(IEventHandler* handler) { mEventHandler = handler; }
    bool isRecording() const { return mSinkFile != nullptr; }
};

#endif
