#pragma once

#include <stdint.h>    // for uint32_t, uint8_t
#include <functional>  // for function
#include <memory>      // for shared_ptr, unique_ptr
#include <string>      // for string
#include <variant>     // for variant
#include <vector>      // for vector
#include "TrackQueue.h"
#include "protobuf/spirc.pb.h"  // for MessageType
#include "CSpotContext.h"
#include "PlaybackState.h"

namespace cspot {
class LoginBlob;
struct ITrackPlayer {
    virtual void play(uint32_t pos) = 0;
    virtual void pause(bool paused);
    virtual void nextTrack(bool nextPrev) = 0;
    virtual void stopPlayback() = 0;
    virtual void seekMs(uint32_t pos) = 0;
    virtual void setVolume(uint32_t vol) = 0;
};

class SpircHandler {
 public:
  SpircHandler(const cspot::LoginBlob& loginBlob, ITrackPlayer& player);

  enum class EventType {
    PLAY_PAUSE,
    VOLUME,
    TRACK_INFO,
    DISC,
    NEXT,
    PREV,
    SEEK,
    DEPLETED,
    FLUSH,
    PLAYBACK_START
  };
  static const char* messageTypeToStr(MessageType type);
  void subscribeToMercury();
  ITrackPlayer& getTrackPlayer() { return mPlayer; }
  void start();
  void setPause(bool pause);
  bool previousSong();
  bool nextSong();
  void notifyAudioEnded();
  void notifyCurrTrackUpdate(int trackIdx);
  void notifyPositionMs(uint32_t position);
  void notifyPausedState(bool paused);
  void notifyVolumeSet(uint32_t volume);
  void loadTrackFromURI(const std::string& uri);
  TrackQueue& getTrackQueue() { return mTrackQueue; }
  void disconnect();

  Context mCtx;
  PlaybackState mPlaybackState;
  TrackQueue mTrackQueue;
  ITrackPlayer& mPlayer;
private:
  void sendCmd(MessageType typ);
  void handleFrame(std::vector<uint8_t>& data);
  void notify();
};
}  // namespace cspot
