#include "PlaybackState.h"

#include <string.h>  // for strdup, memcpy, strcpy, strlen
#include <cstdint>   // for uint8_t
#include <cstdlib>   // for free, NULL, realloc, rand
#include <cstring>
#include <memory>       // for shared_ptr
#include <type_traits>  // for remove_extent_t
#include <utility>      // for swap

#include "BellLogger.h"          // for AbstractLogger
#include "CSpotContext.h"        // for Context::ConfigState, Context (ptr o...
#include "ConstantParameters.h"  // for protocolVersion, swVersion
#include "Logger.h"              // for CSPOT_LOG
#include "NanoPBHelper.h"        // for pbEncode, pbPutString
#include "Packet.h"              // for cspot
#include "pb.h"                  // for pb_bytes_array_t, PB_BYTES_ARRAY_T_A...
#include "pb_decode.h"           // for pb_release
#include "protobuf/spirc.pb.h"
#include "TrackQueue.h"

using namespace cspot;

PlaybackState::PlaybackState(cspot::Context& ctx)
: mCtx(ctx) {
  innerFrame = {};
  remoteFrame = {};

  // Prepare callbacks for decoding of remote frame track data
  remoteFrame.state.track.funcs.decode = &PlaybackState::pbDecodeTrackRef;
  remoteFrame.state.track.arg = this;

  innerFrame.state.track.funcs.encode = &PlaybackState::pbEncodeTrackList;
  innerFrame.state.track.arg = this;

  innerFrame.ident = strdup(mCtx.mConfig.deviceId.c_str());
  innerFrame.protocol_version = strdup(protocolVersion);

  // Prepare default state
  innerFrame.state.has_position_ms = true;
  innerFrame.state.position_ms = 0;

  innerFrame.state.status = PlayStatus_kPlayStatusStop;
  innerFrame.state.has_status = true;

  innerFrame.state.position_measured_at = 0;
  innerFrame.state.has_position_measured_at = true;

  innerFrame.state.shuffle = false;
  innerFrame.state.has_shuffle = true;

  innerFrame.state.repeat = false;
  innerFrame.state.has_repeat = true;

  innerFrame.device_state.sw_version = strdup(swVersion);

  innerFrame.device_state.is_active = false;
  innerFrame.device_state.has_is_active = true;

  innerFrame.device_state.can_play = true;
  innerFrame.device_state.has_can_play = true;

  innerFrame.device_state.volume = mCtx.mConfig.volume;
  innerFrame.device_state.has_volume = true;

  innerFrame.device_state.name = strdup(mCtx.mConfig.deviceName.c_str());

  // Prepare player's capabilities
  addCapability(CapabilityType_kCanBePlayer, 1);
  addCapability(CapabilityType_kDeviceType, 4);
  addCapability(CapabilityType_kGaiaEqConnectId, 1);
  addCapability(CapabilityType_kSupportsLogout, 0);
  addCapability(CapabilityType_kSupportsPlaylistV2, 1);
  addCapability(CapabilityType_kIsObservable, 1);
  addCapability(CapabilityType_kVolumeSteps, 64);
  addCapability(CapabilityType_kSupportedContexts, -1,
                std::vector<std::string>({"album", "playlist", "search",
                                          "inbox", "toplist", "starred",
                                          "publishedstarred", "track"}));
  addCapability(CapabilityType_kSupportedTypes, -1,
                std::vector<std::string>(
                    {"audio/track", "audio/episode", "audio/episode+track"}));
  innerFrame.device_state.capabilities_count = 8;
}

PlaybackState::~PlaybackState() {
  pb_release(Frame_fields, &innerFrame);
  pb_release(Frame_fields, &remoteFrame);
}

void PlaybackState::setPlaybackState(const PlaybackState::State state) {
  switch (state) {
    case State::Loading:
      // Prepare the playback at position 0
      innerFrame.state.status = PlayStatus_kPlayStatusPause;
      innerFrame.state.position_ms = 0;
      innerFrame.state.position_measured_at =
          mCtx.mTimeProvider.getSyncedTimestamp();
      break;
    case State::Playing:
      innerFrame.state.status = PlayStatus_kPlayStatusPlay;
      innerFrame.state.position_measured_at =
          mCtx.mTimeProvider.getSyncedTimestamp();
      break;
    case State::Stopped:
      break;
    case State::Paused:
      // Update state and recalculate current song position
      innerFrame.state.status = PlayStatus_kPlayStatusPause;
      uint32_t diff = mCtx.mTimeProvider.getSyncedTimestamp() -
                      innerFrame.state.position_measured_at;
      this->updatePositionMs(innerFrame.state.position_ms + diff);
      break;
  }
}

void PlaybackState::syncWithRemote()
{
  innerFrame.state.context_uri = (char*)realloc(
      innerFrame.state.context_uri, strlen(remoteFrame.state.context_uri) + 1);
  strcpy(innerFrame.state.context_uri, remoteFrame.state.context_uri);

  innerFrame.state.has_playing_track_index = true;
  innerFrame.state.playing_track_index = remoteFrame.state.playing_track_index;
}

bool PlaybackState::isActive() {
  return innerFrame.device_state.is_active;
}

void PlaybackState::setActive(bool isActive) {
  innerFrame.device_state.is_active = isActive;
  if (isActive) {
    innerFrame.device_state.became_active_at =
        mCtx.mTimeProvider.getSyncedTimestamp();
    innerFrame.device_state.has_became_active_at = true;
  }
}

void PlaybackState::updatePositionMs(uint32_t position) {
  innerFrame.state.position_ms = position;
  innerFrame.state.position_measured_at =
      mCtx.mTimeProvider.getSyncedTimestamp();
}

void PlaybackState::setVolume(uint16_t volume)
{
    innerFrame.device_state.volume = volume;
    mCtx.mConfig.volume = volume;
}

bool PlaybackState::decodeRemoteFrame(std::vector<uint8_t>& data)
{
    pb_release(Frame_fields, &remoteFrame);
    hasTrackDecoded = false;
    pbDecode(remoteFrame, Frame_fields, data);
    // if message type is track update, but it didn't contain any tracks, clear them
    if (!hasTrackDecoded &&
       (remoteFrame.typ == MessageType_kMessageTypeLoad || remoteFrame.typ == MessageType_kMessageTypeReplace)) {
        remoteTracks.clear();
        rawRemoteTracks.clear();
    }
    return true;
}

std::vector<uint8_t> PlaybackState::encodeCurrentFrame(MessageType typ)
{
  // Prepare current frame info
  innerFrame.version = 1;
  innerFrame.seq_nr = this->seqNum;
  innerFrame.typ = typ;
  innerFrame.state_update_id = mCtx.mTimeProvider.getSyncedTimestamp();
  innerFrame.has_version = true;
  innerFrame.has_seq_nr = true;
  innerFrame.recipient_count = 0;
  innerFrame.has_state = true;
  innerFrame.has_device_state = true;
  innerFrame.has_typ = true;
  innerFrame.has_state_update_id = true;

  this->seqNum += 1;
  return pbEncode(Frame_fields, &innerFrame);
}

// Wraps messy nanopb setters. @TODO: find a better way to handle this
void PlaybackState::addCapability(CapabilityType typ, int intValue, std::vector<std::string> stringValue)
{
  auto& cap = innerFrame.device_state.capabilities[capabilityIndex];
  cap.has_typ = true;
  cap.typ = typ;

  if (intValue != -1) {
    cap.intValue[0] = intValue;
    cap.intValue_count = 1;
  }
  else {
    cap.intValue_count = 0;
  }

  for (int x = 0; x < stringValue.size(); x++) {
    pbPutString(stringValue[x], cap.stringValue[x]);
  }
  cap.stringValue_count = stringValue.size();
  this->capabilityIndex += 1;
}

bool PlaybackState::pbDecodeTrackRef(pb_istream_t* stream, const pb_field_t* field, void** arg)
{
    auto& self = *static_cast<PlaybackState*>(*arg);
    auto& trackQueue = self.remoteTracks;
    auto& rawTracks = self.rawRemoteTracks;
    if (!self.hasTrackDecoded) {
        trackQueue.clear();
        rawTracks.clear();
        self.hasTrackDecoded = true;
    }
    // Push a new reference
    trackQueue.emplace_back();
    auto& track = trackQueue.back();
    // save raw track data
    auto& raw = rawTracks.add(stream->bytes_left);
    memcpy(raw.data, stream->state, stream->bytes_left);
    pb_wire_type_t wire_type;
    pb_istream_t substream;
    uint32_t tag;
    std::string uri;

    for(;;) {
        bool eof;
        if (!pb_decode_tag(stream, &wire_type, &tag, &eof)) { // decoding failed
            if (eof) { // EOF
                break;
            }
            return false; // error
        }
        switch (tag) {
            case TrackRef_uri_tag:
            case TrackRef_gid_tag: {
            // Make substream
                if (!pb_make_string_substream(stream, &substream)) {
                    return false;
                }
                uint8_t* destBuffer = nullptr;
                // Handle GID
                if (tag == TrackRef_gid_tag) {
                    track.gid.resize(substream.bytes_left);
                    destBuffer = track.gid.data();
                }
                // uri
                else if (tag == TrackRef_uri_tag) {
                    uri.resize(substream.bytes_left);
                    destBuffer = reinterpret_cast<uint8_t*>(uri.data());
                }
                if (!pb_read(&substream, destBuffer, substream.bytes_left)) {
                    return false;
                }
                // Close substream
                if (!pb_close_string_substream(stream, &substream)) {
                    return false;
                }
                break;
            }
            default:
                // Field not known, skip
                pb_skip_field(stream, wire_type);
                break;
        }
    }
    // Fill in GID when only URI is provided
    track.decodeURI(uri);
    return true;
}
bool PlaybackState::pbEncodeTrackList(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    auto& self = *static_cast<PlaybackState*>(*arg);
    auto& rawTracks = self.rawRemoteTracks;
    int cnt = 0;
    for (auto rawTrack = rawTracks.head; rawTrack; rawTrack = rawTrack->next) {
        if (!pb_encode_tag_for_field(stream, field)) {
            SPIRC_LOGE("%s: pb_encode_tag_for_field failed", __FUNCTION__);
            return false;
        }
        bool ok;
        if (rawTrack->size < 128) {
            pb_byte_t size = rawTrack->size; // platform endian-ness agnostic
            ok = pb_write(stream, &size, 1);
        }
        else {
            ok = pb_encode_varint(stream, rawTrack->size);
        }
        if (!ok) {
            SPIRC_LOGE("%s: pb_write len failed", __FUNCTION__);
            return false;
        }
        if (!pb_write(stream, rawTrack->data, rawTrack->size)) {
            SPIRC_LOGE("%s: pb_write raw trackref failed", __FUNCTION__);
            return false;
        }
        cnt++;
    }
    return true;
}
