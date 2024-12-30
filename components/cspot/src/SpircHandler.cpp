#include "SpircHandler.h"

#include <cstdint>      // for uint8_t
#include <memory>       // for shared_ptr, make_unique, unique_ptr
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move

#include "BellLogger.h"      // for AbstractLogger
#include "CSpotContext.h"    // for Context::ConfigState, Context (ptr only)
#include "Logger.h"          // for CSPOT_LOG
#include "MercurySession.h"  // for MercurySession, MercurySession::Response
#include "NanoPBHelper.h"    // for pbDecode
#include "Packet.h"          // for cspot
#include "PlaybackState.h"   // for PlaybackState, PlaybackState::State
#include "TrackQueue.h"
#include "TrackReference.h"     // for TrackReference
#include "Utils.h"              // for stringHexToBytes
#include "pb_decode.h"          // for pb_release
#include "protobuf/spirc.pb.h"  // for Frame, State, Frame_fields, MessageTy...

using namespace cspot;
static const char* TAG = "spirc";

SpircHandler::SpircHandler(const LoginBlob& loginBlob, ITrackPlayer& trackPlayer)
    : mCtx(loginBlob), mPlaybackState(mCtx), mTrackQueue(*this), mPlayer(trackPlayer)
{
  // Subscribe to mercury on session ready
  mCtx.mSession.setConnectedHandler([this]() { this->subscribeToMercury(); });
}
void SpircHandler::start()
{
    mCtx.mSession.connectWithRandomAp();
    mCtx.mConfig.authData = mCtx.mSession.authenticate();
    mTrackQueue.startTask();
    mCtx.mSession.startTask();

}
void SpircHandler::subscribeToMercury() {
  auto responseLambda = [this](MercurySession::Response& res) {
    if (res.fail)
      return;

    sendCmd(MessageType_kMessageTypeHello);
    SPIRC_LOGP("Sent kMessageTypeHello!");

    // Assign country code
    mCtx.mConfig.countryCode = mCtx.mSession.getCountryCode();
  };
  auto subscriptionLambda = [this](MercurySession::Response& res) {
      if (res.fail) {
          return;
      }
      CSPOT_LOG(debug, "Received subscription response");
      this->handleFrame(res.parts[0]);
  };

  mCtx.mSession.executeSubscription(
      MercurySession::RequestType::SUB,
      "hm://remote/user/" + mCtx.mConfig.username + "/", responseLambda, subscriptionLambda);
}

void SpircHandler::loadTrackFromURI(const std::string& uri) {}

void SpircHandler::notifyPositionMs(uint32_t position)
{
    mPlaybackState.updatePositionMs(position);
    notify();
}
void SpircHandler::notifyPausedState(bool paused)
{
    mPlaybackState.setPlaybackState(paused ? PlaybackState::State::Paused: PlaybackState::State::Playing);
    notify();
}
void SpircHandler::notifyCurrTrackUpdate(int trackIdx)
{
    mPlaybackState.innerFrame.state.playing_track_index = trackIdx;
    notify();
}
void SpircHandler::disconnect() {
    mTrackQueue.stopTask();
    mPlayer.stopPlayback();
    mCtx.mSession.disconnect();
}
void SpircHandler::handleFrame(std::vector<uint8_t>& data)
{
  // Decode received spirc frame
  mPlaybackState.decodeRemoteFrame(data);
  auto typ = mPlaybackState.remoteFrame.typ;
  SPIRC_LOGP("Received message %s (%d)", messageTypeToStr(typ), typ);

  switch (typ) {
    case MessageType_kMessageTypeNotify: {
      // Pause the playback if another player took control
      if (mPlaybackState.isActive() && mPlaybackState.remoteFrame.device_state.is_active) {
        SPIRC_LOGW("Another player took control, pausing playback");
        mPlayer.stopPlayback();
        mPlaybackState.setActive(false);
      }
      break;
    }
    case MessageType_kMessageTypeSeek: {
        auto pos = mPlaybackState.remoteFrame.position;
        mPlayer.seekMs(pos);
        notify();
        break;
    }
    case MessageType_kMessageTypeVolume: {
        auto vol = mPlaybackState.remoteFrame.volume;
        // volume is encoded in 64 steps of 1024 each - max value is 65536
        SPIRC_LOGI("Requested volume: %lu", vol);
        mPlayer.setVolume(vol);
        break;
    }
    case MessageType_kMessageTypePause:
        setPause(true);
        break;
    case MessageType_kMessageTypePlay:
        setPause(false);
        break;
    case MessageType_kMessageTypeNext:
        mPlayer.nextTrack(true);
        break;
    case MessageType_kMessageTypePrev:
        mPlayer.nextTrack(false);
        break;
    case MessageType_kMessageTypeLoad: {
        SPIRC_LOGI("New play queue of %d tracks", mPlaybackState.remoteTracks.size());
        if (mPlaybackState.remoteTracks.empty()) {
            SPIRC_LOGI("No tracks in frame, stopping playback");
            break;
        }
        mPlaybackState.setActive(true);
        mPlaybackState.updatePositionMs(mPlaybackState.remoteFrame.position);
        mPlaybackState.setPlaybackState(PlaybackState::State::Playing);
        mPlaybackState.syncWithRemote();
        // Update track list in case we have a new one
        mTrackQueue.updateTracks();
        //    this->notify();
        // Stop the current track, if any
        mPlayer.play(mPlaybackState.remoteFrame.state.position_ms);
        break;
      }
    case MessageType_kMessageTypeReplace: {
        SPIRC_LOGI("Play queue replace with %d tracks", mPlaybackState.remoteTracks.size());
        mPlaybackState.syncWithRemote();
        // 1st track is the current one, but update the position
        mTrackQueue.updateTracks(true);
        notify();
        // need to re-load all if streaming track is completed
        mPlayer.play(mPlaybackState.remoteFrame.state.position_ms +
          mCtx.mTimeProvider.getSyncedTimestamp() - mPlaybackState.innerFrame.state.position_measured_at);
        break;
    }
    case MessageType_kMessageTypeShuffle: {
        notify();
        break;
    }
    case MessageType_kMessageTypeRepeat: {
        notify();
        break;
    }
    default:
        break;
    }
}
void SpircHandler::notifyVolumeSet(uint32_t volume)
{
    mPlaybackState.setVolume(volume);
    notify();
}
void SpircHandler::notify()
{
    sendCmd(MessageType_kMessageTypeNotify);
}
void SpircHandler::sendCmd(MessageType typ) {
    // Serialize current player state
    auto encodedFrame = mPlaybackState.encodeCurrentFrame(typ);
    auto responseLambda = [=](MercurySession::Response& res) {};
    auto parts = MercurySession::DataParts({encodedFrame});

    mCtx.mSession.execute(MercurySession::RequestType::SEND,
      "hm://remote/user/" + mCtx.mConfig.username + "/", responseLambda, parts);
    SPIRC_LOGP("Sent message %s", messageTypeToStr(typ));
}

void SpircHandler::setPause(bool isPaused) {
    if (isPaused) {
        SPIRC_LOGI("External pause command");
        mPlaybackState.setPlaybackState(PlaybackState::State::Paused);
    }
    else {
        SPIRC_LOGI("External play command");
        mPlaybackState.setPlaybackState(PlaybackState::State::Playing);
    }
    notify();
    mPlayer.pause(isPaused);
}
#define ENUM_CASE(type) case MessageType_kMessageType##type: return #type;

const char* SpircHandler::messageTypeToStr(MessageType type)
{
    switch(type) {
        ENUM_CASE(Hello);
        ENUM_CASE(Goodbye);
        ENUM_CASE(Probe);
        ENUM_CASE(Notify);
        ENUM_CASE(Load);
        ENUM_CASE(Play);
        ENUM_CASE(Pause);
        ENUM_CASE(PlayPause);
        ENUM_CASE(Seek);
        ENUM_CASE(Prev);
        ENUM_CASE(Next);
        ENUM_CASE(Volume);
        ENUM_CASE(Shuffle);
        ENUM_CASE(Repeat);
        ENUM_CASE(VolumeDown);
        ENUM_CASE(VolumeUp);
        ENUM_CASE(Replace);
        ENUM_CASE(Logout);
        ENUM_CASE(Action);
        default: return "(unknown)";
    }
}
