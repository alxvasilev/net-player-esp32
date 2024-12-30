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
bool TrackInfo::Loader::resetForLoadRetry(bool force)
{
    if (!force && mLoadRetryCtr >= kMaxLoadRetries) {
        return false;
    }
    mLoadRetryCtr++;
    mTrack.clear();
    mActualGid.clear();
    mFileId.clear();
    mState = State::QUEUED;
    delayBeforeRetry();
    return true;
}
TrackInfo::Loader::~Loader()
{
    printf("=============Loader destroying\n");
    mState = State::FAILED;
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
        CSPOT_LOG(info, "Track name: %s", pbTrack->name);
        CSPOT_LOG(info, "Track duration: %d", pbTrack->duration);
        CSPOT_LOG(debug, "trackInfo.restriction.size() = %d", pbTrack->restriction_count);

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
        CSPOT_LOG(info, "Episode name: %s", pbEpisode->name);
        CSPOT_LOG(info, "Episode duration: %d", pbEpisode->duration);
        CSPOT_LOG(debug, "episodeInfo.restriction.size() = %d", pbEpisode->restriction_count);

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
        CSPOT_LOG(debug, "File format: %d", selectedFiles[x].format);
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
        CSPOT_LOG(info, "File not available for playback");
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
          CSPOT_LOG(info, "Got audio key");
          this->mTrack.audioKey = std::vector<uint8_t>(audioKey.begin() + 4, audioKey.end());
          mState = State::CDN_REQUIRED;
        }
        else {
          CSPOT_LOG(error, "Failed to get audio key");
          mState = State::FAILED;
        }
        signalLoadStep();
      });
  mState = State::PENDING_KEY;
}

void TrackInfo::Loader::stepLoadCDNUrl(const std::string& accessKey)
{
    if (accessKey.size() == 0) {
        // Wait for access key
        return;
    }
    // Request CDN URL
    CSPOT_LOG(info, "Received access key, fetching CDN URL...");
    try {
        std::string requestUrl = string_format(
            "https://api.spotify.com/v1/storage-resolve/files/audio/interactive/%s?alt=json&product=9",
            bytesToHexString(mFileId).c_str());
        auto result = httpGet<std::string>(requestUrl.c_str(), {{"Authorization", ("Bearer " + accessKey).c_str()}});

#ifdef BELL_ONLY_CJSON
        cJSON* jsonResult = cJSON_Parse(result.data());
        mTrack.cdnUrl = cJSON_GetArrayItem(cJSON_GetObjectItem(jsonResult, "cdnurl"), 0)->valuestring;
        cJSON_Delete(jsonResult);
#else
        auto jsonResult = nlohmann::json::parse(result);
        mTrack.cdnUrl = jsonResult["cdnurl"][0];
#endif

        CSPOT_LOG(info, "Received CDN URL, %s", mTrack.cdnUrl.c_str());
        mState = State::READY;
        signalLoadStep();
  }
  catch (...) {
        CSPOT_LOG(error, "Cannot fetch CDN URL");
        mState = State::FAILED;
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
            printf("============wptr expired\n");
            return;
        }
        std::scoped_lock lock(mQueue.mTracksMutex);
        if (res.parts.size() == 0) {
            // Invalid metadata, cannot proceed
            mState = State::FAILED;
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
        mState = parseOk ? State::KEY_REQUIRED : State::FAILED;
        signalLoadStep();
    };
  // Execute the request
  mPendingMercuryRequest = mQueue.mSpirc.mCtx.mSession.execute(
      MercurySession::RequestType::GET, requestUrl, responseHandler);

  // Set the state to pending
  mState = State::PENDING_META;
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
        printf("Cache hit\n");
        return *it;
    }
    auto ret = emplace(new TrackInfo(ref, mQueue));
    printf("Cache miss, loading new TrackInfo\n");
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
        if (track.info->loadState() == TrackInfo::Loader::State::FAILED) { // retry a previous fail
            if (!track.info->mLoader->resetForLoadRetry()) { // loader is guaranteed to exist if state is FAILED
                CSPOT_LOG(error, "Gave up retrying failed load of track %d", idx);
                return false;
            }
            CSPOT_LOG(info, "Re-trying failed loading of track %d", idx);
        }
        else { // in progress or done
            printf("loadTrack[%d]: already loading or loaded: state: %d\n", idx, (int)track.info->loadState());
            return true;
        }
    }
    else { // no TrackInfo at all, create and start loading
        track.info = mCache.get(track);
    }
    return track.info->mLoader->load(true, idx);
}
void TrackQueue::taskFunc()
{
    CSPOT_LOG(info, "Track queue task started");
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
            CSPOT_LOG(info, "Monitoring playlist/curr track for changes(kEvtTracksUpdated)...");
            waitForEvents(kEvtTracksUpdated);
            CSPOT_LOG(info, "Playlist or current track changed, preloading track info...");
            if (mCurrentTrackIdx < 0) {
                continue;
            }
            // Make sure we have the newest access key
            accessKey = mAccessKeyFetcher.getAccessKey();
            CSPOT_LOG(info, "Loading tracks around current index %d", mCurrentTrackIdx);
            loadTrack(mCurrentTrackIdx);
            vTaskDelay(100 / portTICK_PERIOD_MS); // give a chance to currentTrack() to return here
            printf("=========loading track +1 details...\n");
            loadTrack(mCurrentTrackIdx + 1);
            printf("=========loading track +2 details...\n");
            loadTrack(mCurrentTrackIdx + 2);
            printf("=========loading track -1 details...\n");
            loadTrack(mCurrentTrackIdx - 1);
            printf("==========done loading all track details...\n");
        }
    }
    catch(std::exception& ex) {
        CSPOT_LOG(info, "TrackQueue terminating due to exception '%s'", ex.what());
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
            if (track.info && track.info->loadState() == TrackInfo::Loader::State::READY) {
                return track.info;
            }
        }
        scoped_unlock unlock(mTracksMutex);
        waitForEvents(kEvtTrackLoaded);
    }
}

bool TrackInfo::Loader::load(bool retry, int idx)
{
    // assumes trackMutex is locked!
    for (;;) {
        if (mQueue.mTaskStopReq) {
            return false;
        }
        printf("==========load[%d]: state %d\n", idx, (int)mState);
        switch (mState) {
            case State::QUEUED:
                stepLoadMetadata();
                break;
            case State::KEY_REQUIRED:
                stepLoadAudioKey();
                break;
            case State::CDN_REQUIRED:
                stepLoadCDNUrl(mQueue.accessKey);
                break;
            case State::READY:
                mLoadRetryCtr = 0;
                mQueue.mEvents.setBits(TrackQueue::kEvtTrackLoaded);
                CSPOT_LOG(info, "Details loaded for track %d: name: '%s'", idx, mTrack.name.c_str());
                return true;
            case State::FAILED:
                CSPOT_LOG(info, "Details load failed for track %d", idx);
                if (retry && resetForLoadRetry()) {
                    delayBeforeRetry();
                    continue;
                }
                mQueue.mEvents.setBits(TrackQueue::kEvtTrackLoaded);
                return false;
            default:
                break;
        }
        scoped_unlock unlock(mQueue.mTracksMutex);
        mQueue.waitForEvents(TrackQueue::kEvtTrackLoadStep);
    }
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
    CSPOT_LOG(info, "Updated playlist");
    mTaskStopReq = false;
    mEvents.setBits(kEvtTracksUpdated);
}
