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

void TrackInfo::loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

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
      auto imageId =
          pbArrayToVector(pbTrack->album.cover_group.image[0].file_id);
      imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
    }
  }

  number = pbTrack->has_number ? pbTrack->number : 0;
  discNumber = pbTrack->has_disc_number ? pbTrack->disc_number : 0;
  duration = pbTrack->duration;
}

void TrackInfo::loadPbEpisode(Episode* pbEpisode,
                              const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

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

TrackItem::TrackItem(TrackQueue& queue, TrackReference& aRef)
  : mRef(&aRef), mQueue(queue)
{
    mQueue.mLoadedCnt++;
}

void TrackItem::signalLoadStep() {
    mQueue.mEvents.setBits(TrackQueue::kEvtTrackLoadStep);
}
bool TrackItem::resetForLoadRetry(bool force)
{
    if (!force && mLoadRetryCtr >= kMaxLoadRetries) {
        return false;
    }
    mLoadRetryCtr++;
    audioKey.clear();
    cdnUrl.clear();
    trackId.clear();
    fileId.clear();
    mState = State::QUEUED;
    delayBeforeRetry();
    return true;
}
TrackItem::~TrackItem() {
    mState = State::FAILED;
    auto& sess = mQueue.mSpirc.mCtx.mSession;
    if (mPendingMercuryRequest != 0) {
        sess.unregister(mPendingMercuryRequest);
    }
    if (mPendingAudioKeyRequest != 0) {
        sess.unregisterAudioKey(mPendingAudioKeyRequest);
    }
    mQueue.mLoadedCnt--;
}

bool TrackItem::parseMetadata(Track* pbTrack, Episode* pbEpisode) {
  int filesCount = 0;
  AudioFile* selectedFiles = nullptr;
  const char* countryCode = mQueue.mSpirc.mCtx.mConfig.countryCode.c_str();

  if (mRef->type == TrackReference::Type::TRACK) {
    CSPOT_LOG(info, "Track name: %s", pbTrack->name);
    CSPOT_LOG(info, "Track duration: %d", pbTrack->duration);

    CSPOT_LOG(debug, "trackInfo.restriction.size() = %d",
              pbTrack->restriction_count);

    // Check if we can play the track, if not, try alternatives
    if (TrackDataUtils::doRestrictionsApply(
            pbTrack->restriction, pbTrack->restriction_count, countryCode)) {
      // Go through alternatives
      for (int x = 0; x < pbTrack->alternative_count; x++) {
        if (!TrackDataUtils::doRestrictionsApply(
                pbTrack->alternative[x].restriction,
                pbTrack->alternative[x].restriction_count, countryCode)) {
          selectedFiles = pbTrack->alternative[x].file;
          filesCount = pbTrack->alternative[x].file_count;
          trackId = pbArrayToVector(pbTrack->alternative[x].gid);
          break;
        }
      }
    } else {
      // We can play the track
      selectedFiles = pbTrack->file;
      filesCount = pbTrack->file_count;
      trackId = pbArrayToVector(pbTrack->gid);
    }

    if (trackId.size() > 0) {
      // Load track information
      loadPbTrack(pbTrack, trackId);
    }
  } else {
    // Handle episodes
    CSPOT_LOG(info, "Episode name: %s", pbEpisode->name);
    CSPOT_LOG(info, "Episode duration: %d", pbEpisode->duration);

    CSPOT_LOG(debug, "episodeInfo.restriction.size() = %d",
              pbEpisode->restriction_count);

    // Check if we can play the episode
    if (!TrackDataUtils::doRestrictionsApply(pbEpisode->restriction,
                                             pbEpisode->restriction_count,
                                             countryCode)) {
      selectedFiles = pbEpisode->file;
      filesCount = pbEpisode->file_count;
      trackId = pbArrayToVector(pbEpisode->gid);

      // Load track information
      loadPbEpisode(pbEpisode, trackId);
    }
  }

  // Find playable file
  for (int x = 0; x < filesCount; x++) {
    CSPOT_LOG(debug, "File format: %d", selectedFiles[x].format);
    if (selectedFiles[x].format == mQueue.mSpirc.mCtx.mConfig.audioFormat) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      break;  // If file found stop searching
    }

    // Fallback to OGG Vorbis 96kbps
    if (fileId.size() == 0 &&
        selectedFiles[x].format == AudioFormat_OGG_VORBIS_96) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
    }
  }

  // No viable files found for playback
  if (fileId.size() == 0) {
      CSPOT_LOG(info, "File not available for playback");
      // no alternatives for song
      return false;
  }
  else {
      return true;
  }
}

void TrackItem::stepLoadAudioKey() {
  // Request audio key
  mPendingAudioKeyRequest = mQueue.mSpirc.mCtx.mSession.requestAudioKey(trackId, fileId,
      [this, wptr = weak_from_this()](bool success, const std::vector<uint8_t>& audioKey) {
        if (wptr.expired()) {
            return;
        }
        std::scoped_lock lock(mQueue.mTracksMutex);
        if (success) {
          CSPOT_LOG(info, "Got audio key");
          this->audioKey = std::vector<uint8_t>(audioKey.begin() + 4, audioKey.end());
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

void TrackItem::stepLoadCDNUrl(const std::string& accessKey) {
  if (accessKey.size() == 0) {
    // Wait for access key
    return;
  }
  // Request CDN URL
  CSPOT_LOG(info, "Received access key, fetching CDN URL...");
  try {
    std::string requestUrl = string_format(
        "https://api.spotify.com/v1/storage-resolve/files/audio/interactive/"
        "%s?alt=json&product=9",
        bytesToHexString(fileId).c_str());

    auto result = httpGet<std::string>(
        requestUrl.c_str(), {{"Authorization", ("Bearer " + accessKey).c_str()}});

#ifdef BELL_ONLY_CJSON
    cJSON* jsonResult = cJSON_Parse(result.data());
    cdnUrl = cJSON_GetArrayItem(cJSON_GetObjectItem(jsonResult, "cdnurl"), 0)
                 ->valuestring;
    cJSON_Delete(jsonResult);
#else
    auto jsonResult = nlohmann::json::parse(result);
    cdnUrl = jsonResult["cdnurl"][0];
#endif

    CSPOT_LOG(info, "Received CDN URL, %s", cdnUrl.c_str());
    mState = State::READY;
    signalLoadStep();
  }
  catch (...) {
    CSPOT_LOG(error, "Cannot fetch CDN URL");
    mState = State::FAILED;
    signalLoadStep();
  }
}

void TrackItem::stepLoadMetadata()
{
  // Prepare request ID
  std::string requestUrl = string_format(
      "hm://metadata/3/%s/%s",
      mRef->type == TrackReference::Type::TRACK ? "track" : "episode",
      bytesToHexString(mRef->gid).c_str());

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

    Track pbTrack = Track_init_zero;
    Episode pbEpisode = Episode_init_zero;

    // Parse the metadata
    if (mRef->type == TrackReference::Type::TRACK) {
      pbDecode(pbTrack, Track_fields, res.parts[0]);
    }
    else {
      pbDecode(pbEpisode, Episode_fields, res.parts[0]);
    }

    // Parse received metadata
    if (parseMetadata(&pbTrack, &pbEpisode)) {
        mState = State::KEY_REQUIRED;
    }
    else {
        mState = State::FAILED;
    }
    pb_release(Track_fields, &pbTrack);
    pb_release(Episode_fields, &pbEpisode);
    signalLoadStep();
  };
  // Execute the request
  mPendingMercuryRequest = mQueue.mSpirc.mCtx.mSession.execute(
      MercurySession::RequestType::GET, requestUrl, responseHandler);

  // Set the state to pending
  mState = State::PENDING_META;
}
void TrackItem::delayBeforeRetry() const
{
    int delay = (mLoadRetryCtr < 2) ? 500 : std::min(mLoadRetryCtr * 2000, 10000);
    mQueue.mEvents.waitForOneNoReset(TrackQueue::kEvtTerminateReq, delay);
}

TrackQueue::TrackQueue(SpircHandler& spirc)
    : mEvents(kEvtTerminateReq|kStateStopped|kStateTerminated),
      mSpirc(spirc), mAccessKeyFetcher(spirc.mCtx)
{
    // Assign encode callback to track list
    mSpirc.mPlaybackState.innerFrame.state.track.funcs.encode = &TrackReference::pbEncodeTrackList;
    mSpirc.mPlaybackState.innerFrame.state.track.arg = &mTracks;
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
    if (track.mDetails.get()) {
        if (track.mDetails->state() == TrackItem::State::FAILED) { // retry a previous fail
            if (!track.mDetails->resetForLoadRetry()) {
                return false;
            }
            CSPOT_LOG(info, "Re-trying failed loading of track %d", idx);
        }
        else { // in progress or done
            printf("loadTrack[%d]: already loading or loaded: state: %d\n", idx, (int)track.mDetails->state());
            return true;
        }
    }
    else {
        if (mLoadedCnt >= kMaxTracksPreload) {
            freeMostDistantTrack();
        }
        track.mDetails = std::make_shared<TrackItem>(*this, track);
    }
    return track.mDetails->load(true, idx);
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
void TrackQueue::freeMostDistantTrack()
{
    // free the info of the track that is more distant than mCurrentTrackIdx
    int firstBefore = -1;
    int lastAfter = -1;
    for (int i = 0; i < mCurrentTrackIdx; i++) {
        if (mTracks[i].isLoaded()) {
            firstBefore = i;
            break;
        }
    }
    for (int i = mTracks.size() - 1; i > mCurrentTrackIdx; i--) {
        if (mTracks[i].isLoaded()) {
            lastAfter = i;
            break;
        }
    }
    int chosen = -1;
    if (firstBefore < 0) {
        chosen = lastAfter;
    }
    else if (lastAfter < 0) {
        chosen = firstBefore;
    }
    // if distance to left >= distance to right
    else if (mCurrentTrackIdx - firstBefore >= lastAfter - mCurrentTrackIdx) {
        chosen = firstBefore;
    }
    else {
        chosen = lastAfter;
    }
    if (chosen > -1) {
        mTracks[chosen].mDetails.reset();
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

std::shared_ptr<TrackItem> TrackQueue::currentTrack()
{
    std::scoped_lock lock(mTracksMutex);
    for(;;) {
        if (mCurrentTrackIdx >= 0 && mCurrentTrackIdx < mTracks.size()) {
            auto& track = mTracks[mCurrentTrackIdx];
            if (track.mDetails.get()) {
                auto state = track.mDetails->state();
                if (state == TrackItem::State::READY) {
                    return track.mDetails;
                }
            }
        }
        scoped_unlock unlock(mTracksMutex);
        waitForEvents(kEvtTrackLoaded);
    }
}

bool TrackItem::load(bool retry, int idx)
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
                CSPOT_LOG(info, "Details loaded for track %d: name: '%s'", idx, name.c_str());
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

void TrackQueue::updateTracks(bool initial)
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
    mTracks.reserve(mSpirc.mPlaybackState.remoteTracks.size());
    for (const auto& trk: mSpirc.mPlaybackState.remoteTracks) {
        mTracks.emplace_back(trk);
    }
    mCurrentTrackIdx = mSpirc.mPlaybackState.innerFrame.state.playing_track_index;
    CSPOT_LOG(info, "Updated playlist");
    mTaskStopReq = false;
    mEvents.setBits(kEvtTracksUpdated);
}
