#pragma once

#include <stdint.h>  // for uint8_t, uint32_t
#include <memory>    // for shared_ptr
#include <string>    // for string
#include <vector>    // for vector

#include "TrackReference.h"
#include "protobuf/spirc.pb.h"  // for Frame, TrackRef, CapabilityType, Mess...
#include "TrackQueue.h"

namespace cspot {
struct Context;

class PlaybackState {
 private:
  cspot::Context& mCtx;
  uint32_t seqNum = 0;
  uint8_t capabilityIndex = 0;
  std::vector<uint8_t> frameData;
  void addCapability(
      CapabilityType typ, int intValue = -1,
      std::vector<std::string> stringsValue = std::vector<std::string>());
  // Encodes list of track references into a pb structure, used by nanopb
  static bool pbEncodeTrackList(pb_ostream_t* stream, const pb_field_t* field, void* const* arg);
  static bool pbDecodeTrackRef(pb_istream_t* stream, const pb_field_t* field, void** arg);
  struct Buf {
      Buf* next;
      int16_t size;
      uint8_t data[];
  };
  struct BufList {
      Buf* head = nullptr;
      Buf* tail = nullptr;
      Buf& add(int16_t size) {
          auto buf = (Buf*)malloc(sizeof(Buf) + size);
          buf->size = size;
          buf->next = nullptr;
          if (tail) {
              tail->next = buf;
          }
          else { // empty
              myassert(!head);
              head = buf;
          }
          tail = buf;
          return *buf;
      }
      void clear() {
          for(Buf* item = head; item;) {
              auto toDel = item;
              item = item->next;
              free(toDel);
          }
          head = tail = nullptr;
      }
      ~BufList() { clear(); }
 };
 public:
  Frame innerFrame;
  Frame remoteFrame;
  std::vector<TrackQueueItem> remoteTracks;
  BufList rawRemoteTracks;
  bool hasTrackDecoded = false; // used to determine when to clear remoteTracks and rawRemoteTracks while parsing
  enum class State { Playing, Stopped, Loading, Paused };

  /**
     * @brief Player state represents the current state of player.
     *
     * Responsible for keeping track of player's state. Doesn't control the playback itself.
     *
     * @param timeProvider synced time provider
     */
  PlaybackState(Context& ctx);

  ~PlaybackState();

  /**
     * @brief Updates state according to current playback state.
     *
     * @param state playback state
     */
  void setPlaybackState(const PlaybackState::State state);

  /**
     * @brief Sets player activity
     *
     * @param isActive activity status
     */
  void setActive(bool isActive);

  /**
     * @brief Simple getter
     *
     * @return true player is active
     * @return false player is inactive
     */
  bool isActive();

  /**
     * @brief Updates local track position.
     *
     * @param position position in milliseconds
     */
  void updatePositionMs(uint32_t position);

  /**
     * @brief Sets local volume on internal state.
     *
     * @param volume volume between 0 and UINT16 max
     */
  void setVolume(uint16_t volume);

  /**
   * @brief Updates local track queue from remote data.
     */
  void syncWithRemote();

  /**
     * @brief Encodes current frame into binary data via protobuf.
     *
     * @param typ message type to include in frame type
     * @return std::vector<uint8_t> binary frame data
     */
  std::vector<uint8_t> encodeCurrentFrame(MessageType typ);

  bool decodeRemoteFrame(std::vector<uint8_t>& data);
};
}  // namespace cspot
