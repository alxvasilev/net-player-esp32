#include "recorder.hpp"
#include <stdio.h>
#include <unistd.h>
#include <esp_log.h>
#include <sys/stat.h>
#include <string.h>
#include "utils.hpp"

static const char* TAG = "Rec";

TrackRecorder::TrackRecorder(const char *rootPath)
{
    if (createDirIfNotExist(rootPath)) {
        mRootPath = rootPath;
    }
}

void TrackRecorder::setStation(const char* name)
{
    if (mRootPath.empty()) {
        ESP_LOGE(TAG, "setStation: Root path does not exist, aborting");
        return;
    }
    abortTrack();
    mStationName.clear();
    if (!createDirIfNotExist((mRootPath + "/" + name).c_str())) {
        return;
    }
    mStationName = name;
}

bool TrackRecorder::createDirIfNotExist(const char* dirname) const
{
    struct stat info;
    if (stat(dirname, &info) == 0) { // something with that name already exists
        if (info.st_mode & S_IFDIR) { // and is a dir
            return true;
        }
        // exists, but is not a dir, delete it
        if (remove(dirname) != 0) {
            ESP_LOGE(TAG, "setStation: Error deleting file '%s' occupying the name of the station dir: %s", dirname, strerror(errno));
            return false;
        }
    }
    int ret = mkdir(dirname, 0777);
    if (ret) {
        ESP_LOGE(TAG, "Error creating directory '%s': %s", dirname, strerror(errno));
        return false;
    }
    return true;
}

std::string TrackRecorder::trackNameToPath(const char* trackName) const
{
    return mRootPath + "/" + mStationName + "/" + trackName;
}

void TrackRecorder::commit()
{
    if (!mSinkFile) {
        return;
    }
    assert(!mCurrTrackName.empty());
    int ret = fclose(mSinkFile);
    mSinkFile = nullptr;
    if (ret) {
        ESP_LOGE(TAG, "commit: Error closing stream sink file, will not save track");
        return;
    }
    auto name = trackNameToPath(mCurrTrackName.c_str());
    ret = rename(sinkFileName().c_str(), name.c_str());
    if (ret) {
        ESP_LOGE(TAG, "commit: Error renaming sink file to '%s': %s\nTrack will not be saved",
            name.c_str(), strerror(errno));
        return;
    }
    ESP_LOGI(TAG, "Recorded track '%s' on station '%s'", mCurrTrackName.c_str(), mStationName.c_str());
}

void TrackRecorder::onNewTrack(const char* trackName, StreamFormat fmt)
{
    if (mStationName.empty()) {
        return;
    }
    if (mSinkFile) {
        commit();
    }
    assert(!mSinkFile);
    mCurrTrackName.clear();

    struct stat info;
    if (stat(trackNameToPath(trackName).c_str(), &info) == 0) {
        ESP_LOGI(TAG, "onNewTrack: Track '%s' already exists, will not record it", trackName);
        return;
    }

    mSinkFile = fopen(sinkFileName().c_str(), "w+");
    if (!mSinkFile) {
        ESP_LOGE(TAG, "Error opening stream sink file '%s' for writing: %s", sinkFileName().c_str(), strerror(errno));
        return;
    }
    mCurrTrackName = trackName;
    mCurrTrackName += '.';
    mCurrTrackName.append(fmt.codecTypeStr());
    ESP_LOGI(TAG, "Starting to record track '%s' on station %s", mCurrTrackName.c_str(), mStationName.c_str());
}
void TrackRecorder::onData(const void* data, int dataLen)
{
    if (!mSinkFile) {
        return;
    }
    ElapsedTimer timer;
    int ret = fwrite(data, 1, dataLen, mSinkFile);
    if (ret != dataLen) {
        abortTrack();
        ESP_LOGE(TAG, "Error writing to stream sink file: %s", strerror(errno));
    }
    auto msElapsed = timer.msElapsed();
    if (msElapsed > 20) {
        ESP_LOGW(TAG, "SDCard write took %d ms", msElapsed);
    }
}
void TrackRecorder::abortTrack()
{
    if (mSinkFile) {
        fclose(mSinkFile);
        mSinkFile = nullptr;
    }
    mCurrTrackName.clear();
}

