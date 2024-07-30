#ifndef SPOTIFY_NODE_HPP
#define SPOTIFY_NODE_HPP

#define BELL_ONLY_CJSON 1

#include "audioNode.hpp"
#include <httpClient.hpp>
#include "streamRingQueue.hpp"
#include <SpircHandler.h>
#include <Crypto.h>

namespace cspot {
    class LoginBlob;
}
namespace http { class Server; }
class MDns;
class AudioPlayer;

class SpotifyNode: public AudioNodeWithTask, cspot::ITrackPlayer {
protected:
    enum { kRecvSize = 4096 };
    enum: uint8_t {
        kCmdRestartPlayback = AudioNodeWithTask::kCommandLast + 1, /* int pos */
        kCmdRestartPlaybackPaused,
        kCmdStopPlayback,
        kCmdPause, /* bool paused */
        kCmdNextTrack, /* bool prev */
        kCmdStop,
        kCmdSeek, /* size_t pos */
        kCmdSetVolume /* int vol */
    };
    static std::unique_ptr<cspot::LoginBlob> sLoginBlob;
    static AudioPlayer* sAudioPlayer;
    // AES IV for decrypting the audio stream
    static const std::vector<uint8_t> sAudioAesIV;
    cspot::SpircHandler mSpirc;
    StreamRingQueue<200> mRingBuf;
    HttpClient mHttp;
    std::shared_ptr<cspot::TrackItem> mCurrentTrack;
    Crypto mCrypto;
    int32_t mFileSize = 0;
    int32_t mRecvPos = 0;
    int32_t mTsSeek = 0;
    uint8_t mStreamId = 0;

    void onStopRequest() override;
    void nodeThreadFunc();
    virtual bool dispatchCommand(Command &cmd) override;
    void connect();
    bool recv();
    bool startCurrentTrack(uint32_t tsSeek = 0);
    bool startNextTrack(cspot::TrackQueue::SkipDirection dir = cspot::TrackQueue::SkipDirection::NEXT);
    // ITrackPlayer interface
    virtual void restart(uint32_t pos, bool paused = false) override;
    virtual void pause(bool paused) override;
    virtual void nextTrack(cspot::TrackQueue::SkipDirection dir) override;
    virtual void stopPlayback() override;
    virtual void seekMs(size_t pos) override;
    virtual void setVolume(int vol) override;
public:
    static void registerService(AudioPlayer& audioPlayer, MDns& mdns);
    SpotifyNode(IAudioPipeline& parent);
    virtual Type type() const override { return kTypeSpotify; }
    virtual StreamEvent pullData(PacketResult &pr) override { return kErrStreamStopped; }
};
#endif
