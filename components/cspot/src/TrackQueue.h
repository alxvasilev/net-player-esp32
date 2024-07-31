#pragma once

#include <stddef.h>  // for size_t
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <eventGroup.hpp>
#include "BellTask.h"
#include "AccessKeyFetcher.h"
#include "TrackReference.h"

#include "protobuf/metadata.pb.h"  // for Track, _Track, AudioFile, Episode

namespace cspot {
struct Context;
class TrackQueue;
class AccessKeyFetcher;
class CDNAudioFile;
class SpircHandler;

template <class M>
class scoped_unlock {
    M& mMutex;
public:
    scoped_unlock(M& aMutex): mMutex(aMutex){ aMutex.lock(); }
    ~scoped_unlock() { mMutex.unlock(); }
};
struct TrackInfo {
    std::string name, album, artist, imageUrl, trackId;
    uint32_t duration, number, discNumber;
    void loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid);
    void loadPbEpisode(Episode* pbEpisode, const std::vector<uint8_t>& gid);
};

class TrackItem: public std::enable_shared_from_this<TrackItem>, public TrackInfo {
public:
    enum class State: uint8_t {
        QUEUED, PENDING_META, KEY_REQUIRED, PENDING_KEY, CDN_REQUIRED, READY, FAILED
    };
    std::vector<uint8_t> trackId, fileId, audioKey;
    std::string cdnUrl;
protected:
    friend class TrackQueue;
    enum { kMaxLoadRetries = 4 };
    TrackReference& mRef;
    TrackQueue& mQueue;
    uint64_t mPendingMercuryRequest = 0;
    uint32_t mPendingAudioKeyRequest = 0;
    int8_t mLoadRetryCtr = 0;
    State mState = State::QUEUED;  // Current state of the track
public:
    bool load(bool retry);
    void delayBeforeRetry() const;
    State state() const { return mState; }
    TrackItem(TrackQueue& queue, TrackReference& ref, uint32_t aRequestedPos = 0);
    ~TrackItem();
    bool resetForLoadRetry(bool force=false); // clear metadata and reset loading state to QUEUED to retry loading
    bool parseMetadata(Track* pbTrack, Episode* pbEpisode);
    // --- Steps ---
    void stepLoadMetadata();
    void stepLoadAudioKey();
    void stepLoadCDNUrl(const std::string& accessKey);
    //===
    void signalLoadStep();
};

// Item type of TrackQueue
struct PlaylistTrack: public TrackReference {
    std::shared_ptr<TrackItem> mTrackItem;
    PlaylistTrack(const TrackReference& other): TrackReference(other){}
    bool isLoaded() const {
        return mTrackItem.get() && mTrackItem->state() == TrackItem::State::READY;
    }
    bool isLoadedOrFailed() const {
        return mTrackItem.get() && (mTrackItem->state() == TrackItem::State::READY ||
               mTrackItem->state() == TrackItem::State::FAILED);
    }
};

class TrackQueue : public bell::Task {
protected:
  enum { kMaxTracksPreload = 4 };
  friend class TrackItem; // for ctx
  EventGroup mEvents;
  SpircHandler& mSpirc; // as received from Spotify
  AccessKeyFetcher mAccessKeyFetcher;
  std::string accessKey;
  std::vector<PlaylistTrack> mTracks; // as received from Spotify
  int16_t mCurrentTrackIdx = -1;
  bool mTaskStopReq = false;
  bool mTaskTermReq = false;
  int mRequestedPos = -1;
  int mLoadedCnt = 0;
  std::recursive_mutex mTracksMutex;
  void freeMostDistantTrack();
  /** @param immediateRetry - retry failure before returning. This is unline the normal retry where the function
   *  returns after a failed load attempt, and the next retry would be done after the round-robin completes
   */
  bool loadTrack(int idx, bool immediateRetry, int requestedPos = 0);
public:
  enum class SkipDirection { NEXT, PREV };
  enum {
         kEvtTracksUpdated = 1,
         kEvtTrackLoadStep = 2, kEvtTrackLoaded = 4,
         kEvtTerminateReq = 8,
         kStateRunning = 32, kStateStopped = 64, kStateTerminated = 128
  };
  TrackQueue(SpircHandler& spirc);
  ~TrackQueue();
  int waitEvent(EventBits_t events, bool reset=true) {
      auto ret = reset
                 ? mEvents.waitForOneAndReset(events | kEvtTerminateReq, -1)
                 : mEvents.waitForOneNoReset(events | kEvtTerminateReq, -1);
      return (ret & kEvtTerminateReq) == 0;
  }
  void runTask() override;
  void stopTask();
  void terminateTask();
  bool hasTracks();
  bool isFinished();
  bool nextTrack(SkipDirection dir = SkipDirection::NEXT);
  void updateTracks(bool initial = false);
  std::shared_ptr<TrackItem> currentTrack();
  bool queueNextTrack(int offset = 0, uint32_t positionMs = 0);
};
}  // namespace cspot
