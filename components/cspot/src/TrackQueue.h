#pragma once

#include <stddef.h>  // for size_t
#include <atomic>
#include <set>
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
class TrackInfoSharedPtr;

template <class M>
class scoped_unlock {
    M& mMutex;
public:
    scoped_unlock(M& aMutex): mMutex(aMutex){ aMutex.unlock(); }
    ~scoped_unlock() { mMutex.lock(); }
};
struct TrackInfo: public TrackReference {
    class Loader;
    friend class TrackInfoSharedPtr;
    // loaded by parseMetaData
    std::string name, album, artist, imageUrl;
    uint32_t duration, number, discNumber;
    // loaded by other steps
    std::vector<uint8_t> audioKey;
    std::string cdnUrl;
    std::int64_t tsLastUse;
    std::shared_ptr<Loader> mLoader;
    TrackInfo(const TrackReference& ref, TrackQueue& queue)
    : TrackReference(ref), tsLastUse(esp_timer_get_time()), mLoader(new Loader(*this, queue))
    {}
    void clear();
    void addRef() { mRefCount++; }
    void unref() {
        int cnt = --mRefCount;
        if (cnt <= 0) {
            myassert(cnt == 0);
            printf("TrackInfo::unref: refcount reached 0, deleting\n");
            delete this;
            return; // explicit immediate return, just to make sure we don't continue accessing this
        }
        else if (cnt == 1) {
            tsLastUse = esp_timer_get_time();
        }
    }
protected:
    std::atomic<int> mRefCount = 0;
    void fromPbTrack(Track* pbTrack);
    void fromPbEpisode(Episode* pbEpisode);
public:
    class Loader: public std::enable_shared_from_this<Loader> { // need weak pointer for async requests
    public:
        enum State: uint8_t {
            kStateComplete = 0, kStatePending, kStateLoading, kStateFailed
        };
    protected:
        friend class TrackQueue;
        enum { kMaxLoadRetries = 4 };
        TrackInfo& mTrack;
        TrackQueue& mQueue;
        std::vector<uint8_t> mActualGid;
        std::vector<uint8_t> mFileId;
        uint64_t mPendingMercuryRequest = 0;
        uint32_t mPendingAudioKeyRequest = 0;
        int8_t mLoadRetryCtr = 0;
        int8_t mCdnUrlRetryCtr = 0;
        State mStateMetaData = kStatePending;  // Current state of the track
        State mStateAudioKey = kStatePending;
        State mStateCdnUrl = kStatePending;
        State mState = kStatePending;
    public:
        static const char* stateToStr(State state);
        bool load(int idx); // idx is only needed for debug print
        void delayBeforeRetry() const;
        State state() const { return mState; }
        Loader(TrackInfo& info, TrackQueue& queue);
        ~Loader();
        bool resetForLoadRetry(bool resetRetries=false); // clear metadata and reset loading state to QUEUED to retry loading
        bool parseMetadata(void* pbTrackOrEpisode);
    // --- Steps ---
        void stepLoadMetadata();
        void stepLoadAudioKey();
        void stepLoadCDNUrl();
        //===
        void signalLoadStep();
    };
    Loader::State loadState() const {
        return mLoader ? mLoader->state() : (cdnUrl.empty() ? Loader::kStatePending : Loader::kStateComplete);
    }
    struct SharedPtr {
        TrackInfo* mPtr;
        SharedPtr(TrackInfo* ptr=nullptr): mPtr(ptr) {
        if (mPtr) {
            mPtr->addRef();
        }
        }
        SharedPtr(const SharedPtr& other): SharedPtr(other.mPtr) {}
        ~SharedPtr() {
            release();
        }
        void reset(TrackInfo* ptr=nullptr)
        {
            if (mPtr) {
                mPtr->unref();
            }
            mPtr = ptr;
            if (mPtr) {
                mPtr->addRef();
            }
        }
        void release() {
            if (mPtr) {
                mPtr->unref();
                mPtr = nullptr;
            }
        }
        operator bool() const { return mPtr != nullptr; }
        TrackInfo* operator->() { return mPtr; }
        TrackInfo& operator*() { return *mPtr; }
        SharedPtr& operator=(const SharedPtr& other) { reset(other.mPtr); return *this; }
        const TrackInfo* operator->() const { return mPtr; }
        const TrackInfo& operator*() const { return *mPtr; }
    };
    struct CacheCompare {
        using is_transparent = std::true_type;
        bool operator()(const TrackReference& a, const TrackInfo::SharedPtr& b) const { return a < *b; }
        bool operator()(const TrackInfo::SharedPtr& a, const TrackReference& b) const { return *a < b; }
        bool operator()(const TrackInfo::SharedPtr& a, const TrackInfo::SharedPtr& b) const { return *a < *b; }
        bool operator()(const TrackReference& a, const TrackReference& b) const { return a < b; }
    };
    class Cache: public std::set<SharedPtr, CacheCompare> {
    protected:
        TrackQueue& mQueue;
    public:
        Cache(TrackQueue& queue): mQueue(queue){}
        SharedPtr get(const TrackReference& key);
    };
};
struct TrackQueueItem: public TrackReference {
    TrackInfo::SharedPtr info;
    using TrackReference::TrackReference;
    using TrackReference::operator==;
};

class TrackQueue : public Task {
protected:
  enum { kTrackLoadDistanceFromCurr = 2 };
  friend class TrackInfo::Loader; // for ctx
  EventGroup mEvents;
  SpircHandler& mSpirc; // as received from Spotify
  AccessKeyFetcher mAccessKeyFetcher;
  std::string accessKey;
  std::vector<TrackQueueItem> mTracks; // as received from Spotify
  TrackInfo::Cache mCache;
  int16_t mCurrentTrackIdx = -1;
  bool mTaskStopReq = false;
  bool mTaskTermReq = false;
  int mRequestedPos = -1;
  std::recursive_mutex mTracksMutex;
  void unrefDistantTracks();
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
  void updateTracks(bool replace = false);
  TrackInfo::SharedPtr currentTrack();
  bool queueNextTrack(int offset = 0, uint32_t positionMs = 0);
  void notifyCurrentTrackPlaying(uint32_t pos);
};
}  // namespace cspot
