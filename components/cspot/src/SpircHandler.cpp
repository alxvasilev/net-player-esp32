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
    mCtx.mSession.startTask();
}
void SpircHandler::subscribeToMercury() {
  auto responseLambda = [this](MercurySession::Response& res) {
    if (res.fail)
      return;

    sendCmd(MessageType_kMessageTypeHello);
    CSPOT_LOG(debug, "Sent kMessageTypeHello!");

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

void SpircHandler::notifyPositionMs(uint32_t position) {
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

void SpircHandler::handleFrame(std::vector<uint8_t>& data) {
  // Decode received spirc frame
  mPlaybackState.decodeRemoteFrame(data);

  switch (mPlaybackState.remoteFrame.typ) {
    case MessageType_kMessageTypeNotify: {
      CSPOT_LOG(debug, "Notify frame");

      // Pause the playback if another player took control
      if (mPlaybackState.isActive() && mPlaybackState.remoteFrame.device_state.is_active) {
        CSPOT_LOG(debug, "Another player took control, pausing playback");
        mPlaybackState.setActive(false);
        mPlayer.stopPlayback();
      }
      break;
    }
    case MessageType_kMessageTypeSeek: {
          auto pos = mPlaybackState.remoteFrame.position;
      mPlayer.seekMs(pos);
      notify();
      break;
    }
    case MessageType_kMessageTypeVolume:
      mPlaybackState.setVolume(mPlaybackState.remoteFrame.volume);
      notify();
      mPlayer.setVolume((int)mPlaybackState.remoteFrame.volume);
      break;
    case MessageType_kMessageTypePause:
      setPause(true);
      break;
    case MessageType_kMessageTypePlay:
      setPause(false);
      break;
    case MessageType_kMessageTypeNext:
      mPlayer.nextTrack(TrackQueue::SkipDirection::NEXT);
      break;
    case MessageType_kMessageTypePrev:
      mPlayer.nextTrack(TrackQueue::SkipDirection::PREV);
      break;
    case MessageType_kMessageTypeLoad: {
      CSPOT_LOG(info, "New play queue of %d tracks", mPlaybackState.remoteTracks.size());
      if (mPlaybackState.remoteTracks.size() == 0) {
        CSPOT_LOG(debug, "No tracks in frame, stopping playback");
        break;
      }
      mPlaybackState.setActive(true);
      mPlaybackState.updatePositionMs(mPlaybackState.remoteFrame.position);
      mPlaybackState.setPlaybackState(PlaybackState::State::Playing);
      mPlaybackState.syncWithRemote();
      // Update track list in case we have a new one
      mTrackQueue.updateTracks();
      this->notify();
      // Stop the current track, if any
      mPlayer.restart(mPlaybackState.remoteFrame.state.position_ms);
      break;
    }
    case MessageType_kMessageTypeReplace: {
      CSPOT_LOG(debug, "Play queue replace with %d tracks", mPlaybackState.remoteTracks.size());
      mPlaybackState.syncWithRemote();
      // 1st track is the current one, but update the position
      mTrackQueue.updateTracks(false);
      notify();
      // need to re-load all if streaming track is completed
      mPlayer.restart(mPlaybackState.remoteFrame.state.position_ms +
          mCtx.mTimeProvider.getSyncedTimestamp() - mPlaybackState.innerFrame.state.position_measured_at);
      break;
    }
    case MessageType_kMessageTypeShuffle: {
      CSPOT_LOG(debug, "Got shuffle frame");
      notify();
      break;
    }
    case MessageType_kMessageTypeRepeat: {
      CSPOT_LOG(debug, "Got repeat frame");
      notify();
      break;
    }
    default:
      break;
  }
}

void SpircHandler::setRemoteVolume(int volume) {
  mPlaybackState.setVolume(volume);
  notify();
}

void SpircHandler::notify() {
  sendCmd(MessageType_kMessageTypeNotify);
}

void SpircHandler::sendCmd(MessageType typ) {
  // Serialize current player state
  auto encodedFrame = mPlaybackState.encodeCurrentFrame(typ);

  auto responseLambda = [=](MercurySession::Response& res) {
  };
  auto parts = MercurySession::DataParts({encodedFrame});
  mCtx.mSession.execute(MercurySession::RequestType::SEND,
                        "hm://remote/user/" + mCtx.mConfig.username + "/",
                        responseLambda, parts);
}

void SpircHandler::setPause(bool isPaused) {
  if (isPaused) {
    CSPOT_LOG(debug, "External pause command");
    mPlaybackState.setPlaybackState(PlaybackState::State::Paused);
  }
  else {
    CSPOT_LOG(debug, "External play command");
    mPlaybackState.setPlaybackState(PlaybackState::State::Playing);
  }
  notify();
  mPlayer.pause(isPaused);
}
