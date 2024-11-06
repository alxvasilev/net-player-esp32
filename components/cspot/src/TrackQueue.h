#pragma once

#include <stddef.h>  // for size_t
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <eventGroup.hpp>
#include "task.hpp"
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
    scoped_unlock(M& aMutex): mMutex(aMutex){ aMutex.unlock(); }
    ~scoped_unlock() { mMutex.lock(); }
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
    friend class TrackReference; // needs access to mRef to set it to null when destroying
    enum { kMaxLoadRetries = 4 };
    TrackReference* mRef;
    TrackQueue& mQueue;
    uint64_t mPendingMercuryRequest = 0;
    uint32_t mPendingAudioKeyRequest = 0;
    int8_t mLoadRetryCtr = 0;
    State mState = State::QUEUED;  // Current state of the track
public:
    bool load(bool retry, int idx); // idx is only needed for debug print
    void delayBeforeRetry() const;
    State state() const { return mState; }
    TrackItem(TrackQueue& queue, TrackReference& ref);
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

class TrackQueue : public Task {
protected:
  enum { kMaxTracksPreload = 4 };
  friend class TrackItem; // for ctx
  EventGroup mEvents;
  SpircHandler& mSpirc; // as received from Spotify
  AccessKeyFetcher mAccessKeyFetcher;
  std::string accessKey;
  std::vector<TrackReference> mTracks; // as received from Spotify
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
  bool loadTrack(int idx);
public:
  enum Event: uint8_t {
         kEvtTracksUpdated = 1,
         kEvtTrackLoadStep = 2, kEvtTrackLoaded = 4,
         kEvtTerminateReq = 8,
         kStateStopped = 64, kStateTerminated = 128
  };
  struct TerminatingException: public std::runtime_error {
      TerminatingException(): runtime_error("Terminating") {}
  };
  TrackQueue(SpircHandler& spirc);
  ~TrackQueue();
  Event waitForEvents(EventBits_t events, bool reset=true) {
      auto ret = reset
                 ? mEvents.waitForOneAndReset(events | kEvtTerminateReq, -1)
                 : mEvents.waitForOneNoReset(events | kEvtTerminateReq, -1);
      if (ret & kEvtTerminateReq) {
          throw TerminatingException();
      }
      return (Event)ret;
  }
  void waitForState(Event state) {
      if (mEvents.waitForOneNoReset(state | kEvtTerminateReq, -1) & kEvtTerminateReq) {
          throw TerminatingException();
      }
  }
  void msDelay(int ms) {
      if (mEvents.waitForOneNoReset(kEvtTerminateReq, ms) & kEvtTerminateReq) {
          throw TerminatingException();
      }
  }
  void taskFunc();
  void startTask();
  void stopTask();
  void terminateTask();
  bool hasTracks();
  bool isFinished();
  bool nextTrack(bool nextPrev = true);
  void updateTracks(bool initial = false);
  std::shared_ptr<TrackItem> currentTrack();
  bool queueNextTrack(int offset = 0, uint32_t positionMs = 0);
  void notifyCurrentTrackPlaying(uint32_t pos);
};
}  // namespace cspot
