#ifndef TRACK_RECORDER_HPP
#define TRACK_RECORDER_HPP
#include <string>
#include <stdio.h>
#include "audioNode.hpp"

class TrackRecorder
{
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
public:
    TrackRecorder(const char* rootPath);
    ~TrackRecorder() { abortTrack(); }
    void setStation(const char* name);
    bool onNewTrack(const char* trackName, StreamFormat fmt);
    void onData(const void* data, int dataLen);
    void abortTrack();
    bool isRecording() const { return mSinkFile != nullptr; }
};

#endif
