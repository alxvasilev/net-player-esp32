#include "TrackQueue.h"
#include <pb_decode.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include "BellUtils.h" // for BELL_SLEEP_MS
#include "SpircHandler.h"
#include <httpClient.hpp>
#include "Logger.h"
#include "Utils.h"
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif
#include "protobuf/metadata.pb.h"

#define TAGQ "cspot-queue"

using namespace cspot;
namespace TrackDataUtils {
bool countryListContains(char* countryList, const char* country) {
  uint16_t countryList_length = strlen(countryList);
  for (int x = 0; x < countryList_length; x += 2) {
    if (countryList[x] == country[0] && countryList[x + 1] == country[1]) {
      return true;
    }
  }
  return false;
}

bool doRestrictionsApply(Restriction* restrictions, int count,
                         const char* country) {
  for (int x = 0; x < count; x++) {
    if (restrictions[x].countries_allowed != nullptr) {
      return !countryListContains(restrictions[x].countries_allowed, country);
    }

    if (restrictions[x].countries_forbidden != nullptr) {
      return countryListContains(restrictions[x].countries_forbidden, country);
    }
  }

  return false;
}

bool canPlayTrack(Track& trackInfo, int altIndex, const char* country) {
  if (altIndex < 0) {

  } else {
    for (int x = 0; x < trackInfo.alternative[altIndex].restriction_count;
         x++) {
      if (trackInfo.alternative[altIndex].restriction[x].countries_allowed !=
          nullptr) {
        return countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_allowed,
            country);
      }

      if (trackInfo.alternative[altIndex].restriction[x].countries_forbidden !=
          nullptr) {
        return !countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_forbidden,
            country);
      }
    }
  }
  return true;
}
}  // namespace TrackDataUtils

void TrackInfo::fromPbTrack(Track* pbTrack)
{
    name = std::string(pbTrack->name);
    if (pbTrack->artist_count > 0) {
        // Handle artist data
        artist = std::string(pbTrack->artist[0].name);
    }

    if (pbTrack->has_album) {
        // Handle album data
        album = std::string(pbTrack->album.name);
        if (pbTrack->album.has_cover_group &&
            pbTrack->album.cover_group.image_count > 0) {
            auto imageId = pbArrayToVector(pbTrack->album.cover_group.image[0].file_id);
            imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
        }
    }

    number = pbTrack->has_number ? pbTrack->number : 0;
    discNumber = pbTrack->has_disc_number ? pbTrack->disc_number : 0;
    duration = pbTrack->duration;
}

void TrackInfo::fromPbEpisode(Episode* pbEpisode)
{
    name = std::string(pbEpisode->name);
    if (pbEpisode->covers->image_count > 0) {
        // Handle episode info
        auto imageId = pbArrayToVector(pbEpisode->covers->image[0].file_id);
        imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
    }

    number = pbEpisode->has_number ? pbEpisode->number : 0;
    discNumber = 0;
    duration = pbEpisode->duration;
}
void TrackInfo::clear()
{
    name.clear();
    // loaded by parseMetaData
    album.clear();
    artist.clear();
    imageUrl.clear();
    duration = 0;
    number = 0;
    discNumber = 0;
    audioKey.clear();
    cdnUrl.clear();
}
TrackInfo::Loader::Loader(TrackInfo& track, TrackQueue& queue)
: mTrack(track), mQueue(queue)
{}

void TrackInfo::Loader::signalLoadStep()
{
    mQueue.mEvents.setBits(TrackQueue::kEvtTrackLoadStep);
}
bool TrackInfo::Loader::resetForLoadRetry(bool resetRetries)
{
    if (resetRetries) {
        mLoadRetryCtr = 0;
    }
    else {
        if (mLoadRetryCtr >= kMaxLoadRetries) {
            return false;
        }
        mLoadRetryCtr++;
    }
    mTrack.clear();
    mActualGid.clear();
    mFileId.clear();
    mState = mStateMetaData = mStateAudioKey = mStateCdnUrl = kStatePending;
    delayBeforeRetry();
    return true;
}
TrackInfo::Loader::~Loader()
{
    auto& sess = mQueue.mSpirc.mCtx.mSession;
    if (mPendingMercuryRequest != 0) {
        sess.unregister(mPendingMercuryRequest);
    }
    if (mPendingAudioKeyRequest != 0) {
        sess.unregisterAudioKey(mPendingAudioKeyRequest);
    }
}

bool TrackInfo::Loader::parseMetadata(void* pbTrackOrEpisode)
{
    int filesCount = 0;
    AudioFile* selectedFiles = nullptr;
    const char* countryCode = mQueue.mSpirc.mCtx.mConfig.countryCode.c_str();

    if (mTrack.type == TrackReference::Type::TRACK) {
        auto pbTrack = (Track*)pbTrackOrEpisode;
        TRKLDR_LOGD("Track info:\n\tname: %s\n\tartist: %s\n\tduration: %ld\n\t",
                    strOrNull(pbTrack->name),
                    pbTrack->artist ? strOrNull(pbTrack->artist->name) : "(null)",
                    pbTrack->duration);
        // Check if we can play the track, if not, try alternatives
        if (TrackDataUtils::doRestrictionsApply(pbTrack->restriction, pbTrack->restriction_count, countryCode)) {
            // Go through alternatives
            for (int x = 0; x < pbTrack->alternative_count; x++) {
                if (!TrackDataUtils::doRestrictionsApply(
                    pbTrack->alternative[x].restriction,
                    pbTrack->alternative[x].restriction_count, countryCode)) {
                        selectedFiles = pbTrack->alternative[x].file;
                        filesCount = pbTrack->alternative[x].file_count;
                        mActualGid = pbArrayToVector(pbTrack->alternative[x].gid);
                        break;
                }
            }
        }
        else {
            // We can play the track
            selectedFiles = pbTrack->file;
            filesCount = pbTrack->file_count;
            mActualGid = pbArrayToVector(pbTrack->gid);
        }

        if (mActualGid.size() > 0) {
            // Load track information
            mTrack.fromPbTrack(pbTrack);
        }
    }
    else {
        // Handle episodes
        auto pbEpisode = (Episode*)pbTrackOrEpisode;
        TRKLDR_LOGD("Episode info:\n\tname: %s\n\tduration: %ld\n\trestrictions: %zu",
            strOrNull(pbEpisode->name), pbEpisode->duration, pbEpisode->restriction_count);

        // Check if we can play the episode
        if (!TrackDataUtils::doRestrictionsApply(pbEpisode->restriction, pbEpisode->restriction_count, countryCode)) {
            selectedFiles = pbEpisode->file;
            filesCount = pbEpisode->file_count;
            mActualGid = pbArrayToVector(pbEpisode->gid);
            // Load track information
            mTrack.fromPbEpisode(pbEpisode);
        }
    }

    // Find playable file
    for (int x = 0; x < filesCount; x++) {
        TRKLDR_LOGD("\tFile format: %d", selectedFiles[x].format);
        if (selectedFiles[x].format == mQueue.mSpirc.mCtx.mConfig.audioFormat) {
            mFileId = pbArrayToVector(selectedFiles[x].file_id);
            break;  // If file found stop searching
        }

        // Fallback to OGG Vorbis 96kbps
        if (mFileId.size() == 0 && selectedFiles[x].format == AudioFormat_OGG_VORBIS_96) {
            mFileId = pbArrayToVector(selectedFiles[x].file_id);
        }
    }

    // No viable files found for playback
    if (mFileId.size() == 0) {
        TRKLDR_LOGE("\tFile not available for playback");
        // no alternatives for song
        return false;
    }
    return true;
}

void TrackInfo::Loader::stepLoadAudioKey()
{
  // Request audio key
  mPendingAudioKeyRequest = mQueue.mSpirc.mCtx.mSession.requestAudioKey(mActualGid, mFileId,
      [this, wptr = weak_from_this()](bool success, const std::vector<uint8_t>& audioKey) {
        if (wptr.expired()) {
            return;
        }
        std::scoped_lock lock(mQueue.mTracksMutex);
        if (success) {
          TRKLDR_LOGD("Got audio key");
          this->mTrack.audioKey = std::vector<uint8_t>(audioKey.begin() + 4, audioKey.end());
          mStateAudioKey = kStateComplete;
        }
        else {
          TRKLDR_LOGE("Failed to get audio key for file '%s'", mTrack.name.c_str());
          mStateAudioKey = kStateFailed;
        }
        signalLoadStep();
      });
  mStateAudioKey = kStateLoading;
}

void TrackInfo::Loader::stepLoadCDNUrl()
{
    myassert(!mQueue.accessKey.empty());
    mStateCdnUrl = kStateLoading;
    TRKLDR_LOGD("fetching CDN URL for file '%s'...", mTrack.name.c_str());
    try {
        std::string requestUrl = string_format(
            "https://api.spotify.com/v1/storage-resolve/files/audio/interactive/%s?alt=json&product=9",
            bytesToHexString(mFileId).c_str());
        auto result = httpGet<std::string>(requestUrl.c_str(), {{"Authorization", ("Bearer " + mQueue.accessKey).c_str()}});

#ifdef BELL_ONLY_CJSON
        cJSON* jsonResult = cJSON_Parse(result.data());
        mTrack.cdnUrl = cJSON_GetArrayItem(cJSON_GetObjectItem(jsonResult, "cdnurl"), 0)->valuestring;
        cJSON_Delete(jsonResult);
#else
        auto jsonResult = nlohmann::json::parse(result);
        mTrack.cdnUrl = jsonResult["cdnurl"][0];
#endif

        TRKLDR_LOGD("Received CDN URL for '%s': %s", mTrack.name.c_str(), mTrack.cdnUrl.c_str());
        mStateCdnUrl = kStateComplete;
        signalLoadStep();
  }
  catch (...) {
        TRKLDR_LOGE("Cannot fetch CDN URL for '%s'", mTrack.name.c_str());
        mStateCdnUrl = kStateFailed;
        signalLoadStep();
  }
}

void TrackInfo::Loader::stepLoadMetadata()
{
    // Prepare request ID
    std::string requestUrl = string_format("hm://metadata/3/%s/%s",
        mTrack.type == TrackReference::Type::TRACK ? "track" : "episode",
        bytesToHexString(mTrack.gid).c_str());

    auto responseHandler = [this, wptr = weak_from_this()](MercurySession::Response& res)
    {
        if (wptr.expired()) {
            return;
        }
        std::scoped_lock lock(mQueue.mTracksMutex);
        if (res.parts.size() == 0) {
            // Invalid metadata, cannot proceed
            mStateMetaData = kStateFailed;
            signalLoadStep();
            return;
        }
        bool parseOk;
        // Parse the metadata
        if (mTrack.type == TrackReference::Type::TRACK) {
            Track pbTrack = Track_init_zero;
            pbDecode(pbTrack, Track_fields, res.parts[0]);
            parseOk = parseMetadata(&pbTrack);
            pb_release(Track_fields, &pbTrack);
        }
        else {
            Episode pbEpisode = Episode_init_zero;
            pbDecode(pbEpisode, Episode_fields, res.parts[0]);
            parseOk = parseMetadata(&pbEpisode);
            pb_release(Episode_fields, &pbEpisode);
        }
        mStateMetaData = parseOk ? kStateComplete : kStateFailed;
        signalLoadStep();
    };
  mStateMetaData = kStateLoading;
  // Execute the request
  mPendingMercuryRequest = mQueue.mSpirc.mCtx.mSession.execute(
      MercurySession::RequestType::GET, requestUrl, responseHandler);
}
void TrackInfo::Loader::delayBeforeRetry() const
{
    int delay = (mLoadRetryCtr < 2) ? 500 : std::min(mLoadRetryCtr * 2000, 10000);
    mQueue.mEvents.waitForOneNoReset(TrackQueue::kEvtTerminateReq, delay);
}
TrackInfo::SharedPtr TrackInfo::Cache::get(const TrackReference& ref)
{
    auto it = find(ref);
    if (it != end()) {
        TRKLDR_LOGI("Cache hit");
        return *it;
    }
    auto ret = emplace(new TrackInfo(ref, mQueue));
    TRKLDR_LOGD("Cache miss");
    return *ret.first;
}
TrackQueue::TrackQueue(SpircHandler& spirc)
    : mEvents(kEvtTerminateReq|kStateStopped|kStateTerminated),
      mSpirc(spirc), mAccessKeyFetcher(spirc.mCtx), mCache(*this)
{
}

TrackQueue::~TrackQueue()
{
  stopTask();
  std::scoped_lock lock(mTracksMutex);
}

bool TrackQueue::loadTrack(int idx)
{
    std::scoped_lock lock(mTracksMutex);
    if (idx < 0 || idx >= mTracks.size()) {
        return false; // it's valid to try an invalid index, fail silently
    }
    auto& track = mTracks[idx];
    if (track.info) {
        if (track.info->loadState() == TrackInfo::Loader::kStateFailed) { // retry a previous fail
            if (!track.info->mLoader->resetForLoadRetry(true)) { // loader is guaranteed to exist if state is FAILED
                ESP_LOGE(TAGQ, "Gave up retrying failed load of track %d", idx);
                return false;
            }
            ESP_LOGW(TAGQ, "Re-trying failed loading of track %d", idx);
        }
        else { // in progress or done
            ESP_LOGD(TAGQ, "loadTrack[%d]: already loading or loaded: state: %s",
                     idx, TrackInfo::Loader::stateToStr(track.info->loadState()));
            return true;
        }
    }
    else { // no TrackInfo at all, create and start loading
        track.info = mCache.get(track);
    }
    return track.info->mLoader->load(idx);
}
void TrackQueue::taskFunc()
{
    ESP_LOGI(TAGQ, "Task started");
    mEvents.clearBits(0xff);
    try {
        for (;;) {
            if (mTaskStopReq) { // track update requires that we pause
                mEvents.setBits(kStateStopped);
                while(mTaskStopReq) {
                    printf("=======waiting any event to release stop\n");
                    waitForEvents(0xff, false); // any event
                }
                mEvents.clearBits(kStateStopped);
            }
            ESP_LOGI(TAGQ, "Monitoring playlist/curr track for changes...");
            waitForEvents(kEvtTracksUpdated);
            ESP_LOGW(TAGQ, "Playlist or current track changed");
            if (mCurrentTrackIdx < 0) {
                continue;
            }
            // Make sure we have the newest access key
            accessKey = mAccessKeyFetcher.getAccessKey();
            ESP_LOGI(TAGQ, "Preloading track info around current index %d", mCurrentTrackIdx);
            loadTrack(mCurrentTrackIdx);
            vTaskDelay(pdMS_TO_TICKS(100)); // give a chance to currentTrack() to return here
            ESP_LOGI(TAGQ, "Loading curr+1 info...");
            loadTrack(mCurrentTrackIdx + 1);
            ESP_LOGI(TAGQ, "Loading track+2 info...");
            loadTrack(mCurrentTrackIdx + 2);
            ESP_LOGI(TAGQ, "Loading track-1 info...");
            loadTrack(mCurrentTrackIdx - 1);
            ESP_LOGI(TAGQ, "Done pre-loading track info of tracks around current");
        }
    }
    catch(std::exception& ex) {
        ESP_LOGE(TAGQ, "TrackQueue terminating due to exception '%s'", ex.what());
    }
    mEvents.setBits(kStateTerminated);
}
void TrackQueue::unrefDistantTracks()
{
    int bound = mCurrentTrackIdx - kTrackLoadDistanceFromCurr;
    if (bound > 0) {
        for (int i = 0; i < bound; i++) {
            mTracks[i].info.release();
        }
    }
    bound = mCurrentTrackIdx + kTrackLoadDistanceFromCurr;
    for (int i = bound; i < mTracks.size(); i++) {
        mTracks[i].info.release();
    }
}
void TrackQueue::startTask()
{
    createTask("cspot-tracks", true, 4096, 1, 2, this, [](void* arg) {
        static_cast<TrackQueue*>(arg)->taskFunc();
    });
}
void TrackQueue::stopTask()
{
    mTaskStopReq = true;
    mEvents.waitForOneNoReset(kStateStopped, -1);
}
void TrackQueue::terminateTask()
{
    mEvents.setBits(kEvtTerminateReq);
    mTaskTermReq = true;
    mEvents.waitForOneNoReset(kStateTerminated, -1);
}

TrackInfo::SharedPtr TrackQueue::currentTrack()
{
    std::scoped_lock lock(mTracksMutex);
    for(;;) {
        if (mCurrentTrackIdx >= 0 && mCurrentTrackIdx < mTracks.size()) {
            auto& track = mTracks[mCurrentTrackIdx];
            if (track.info && track.info->loadState() == TrackInfo::Loader::kStateComplete) {
                return track.info;
            }
        }
        scoped_unlock unlock(mTracksMutex);
        waitForEvents(kEvtTrackLoaded);
    }
}
const char* TrackInfo::Loader::stateToStr(TrackInfo::Loader::State state) {
    switch(state) {
        case kStateComplete: return "complete";
        case kStateLoading: return "loading";
        case kStateFailed: return "failed";
        case kStatePending: return "pending";
        default: return "(invalid)";
    }
}
bool TrackInfo::Loader::load(int idx)
{
    // assumes trackMutex is locked!
    mState = kStateLoading;
    for (;;) {
        if (mQueue.mTaskStopReq) {
            return false;
        }
        TRKLDR_LOGD("load track %d: metaData %s, audioKey: %s, cdnUrl: %s",
            idx, stateToStr(mStateMetaData), stateToStr(mStateAudioKey), stateToStr(mStateCdnUrl));
        switch (mStateMetaData) {
            case kStateComplete:
                break;
            case kStatePending:
                mLoadRetryCtr = 0;
                stepLoadMetadata();
                goto waitEvent;
            case kStateFailed:
                TRKLDR_LOGE("Metadata load failed for track %d", idx);
                if (resetForLoadRetry()) {
                    continue;
                }
                mQueue.mEvents.setBits(TrackQueue::kEvtTrackLoaded);
                return false;
            case kStateLoading: goto waitEvent;
            default: myassert(false);
        }
        switch(mStateAudioKey) {
            case kStateComplete:
                if (mStateCdnUrl == kStateComplete && mState == kStateLoading) {
                    mState = kStateComplete;
                    goto done;
                }
                break;
            case kStateFailed:
                mTrack.audioKey.clear();
                if (++mLoadRetryCtr > kMaxLoadRetries) {
                    goto failed;
                }
                [[fallthrough]];
            case kStatePending:
                mLoadRetryCtr = 0; // shared with metadata load: they are always sequential
                stepLoadAudioKey();
                break;
            case kStateLoading: break;
            default: myassert(false);
        }
        switch(mStateCdnUrl) {
            case kStateComplete:
                if (mStateAudioKey == kStateComplete && mState == kStateLoading) {
                    mState = kStateComplete;
                    goto done;
                }
                break;
            case kStateFailed:
                if (++mCdnUrlRetryCtr > kMaxLoadRetries) {
                    goto failed;
                }
                [[fallthrough]];
            case kStatePending:
                mCdnUrlRetryCtr = 0;
                stepLoadCDNUrl();
                break;
            case kStateLoading: break;
            default: myassert(false);
        }
        waitEvent: {
            scoped_unlock unlock(mQueue.mTracksMutex);
            mQueue.waitForEvents(TrackQueue::kEvtTrackLoadStep);
            continue;
        }
    }
failed:
        mState = kStateFailed;
        mQueue.mEvents.setBits(TrackQueue::kEvtTrackLoaded);
        TRKLDR_LOGE("Failed to load track %d", idx);
        return false;
done:
        mState = kStateComplete;
        TRKLDR_LOGI("Info loaded for track %d: '%s'", idx, mTrack.name.c_str());
        mQueue.mEvents.setBits(TrackQueue::kEvtTrackLoaded);
        return true;
}

bool TrackQueue::nextTrack(bool nextPrev)
{
  std::scoped_lock lock(mTracksMutex);
  auto& pbState = mSpirc.mPlaybackState;
  if (nextPrev == false) { // prev
      uint64_t position = !mSpirc.mPlaybackState.innerFrame.state.has_position_ms
          ? 0
          : pbState.innerFrame.state.position_ms +
              mSpirc.mCtx.mTimeProvider.getSyncedTimestamp() -
              pbState.innerFrame.state.position_measured_at;

      if (mCurrentTrackIdx < 1 || position > 3000) {
          return false;
      }
      mCurrentTrackIdx--;
  }
  else { // skip next
      if (mCurrentTrackIdx + 1 >= mTracks.size()) {
          return false;
      }
      mCurrentTrackIdx++;
  }
  mEvents.setBits(kEvtTracksUpdated);
  return true;
}
void TrackQueue::notifyCurrentTrackPlaying(uint32_t pos)
{
    // Update frame data
    printf("===============Notifying current track playing\n");
    mSpirc.mPlaybackState.updatePositionMs(pos);
    mSpirc.notifyCurrTrackUpdate(mCurrentTrackIdx);
}
bool TrackQueue::hasTracks() {
    std::scoped_lock lock(mTracksMutex);
    return mTracks.size() > 0;
}

bool TrackQueue::isFinished() {
    std::scoped_lock lock(mTracksMutex);
    return mCurrentTrackIdx >= mTracks.size();
}

void TrackQueue::updateTracks(bool replace)
{
    mTaskStopReq = true;
    mEvents.setBits(kEvtTracksUpdated);
    if (mEvents.waitForOneNoReset(kStateStopped|kEvtTerminateReq, -1) & kEvtTerminateReq) {
        mTaskStopReq = false;
        return; // terminating
    }
    std::scoped_lock lock(mTracksMutex);
    // Copy requested track list
    mTracks.clear();
    mTracks.swap(mSpirc.mPlaybackState.remoteTracks);
    mCurrentTrackIdx = mSpirc.mPlaybackState.innerFrame.state.playing_track_index;
    ESP_LOGI(TAGQ, "Updated playlist");
    mTaskStopReq = false;
    mEvents.setBits(kEvtTracksUpdated);
}
