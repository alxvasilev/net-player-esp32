#ifndef SPOTIFY_NODE_HPP
#define SPOTIFY_NODE_HPP

#define BELL_ONLY_CJSON 1

#include "audioNode.hpp"
#include <httpClient.hpp>
#include "streamRingQueue.hpp"
#include "speedProbe.hpp"
#include <SpircHandler.h>
#include <Crypto.h>

namespace cspot {
    class LoginBlob;
}
namespace http { class Server; }
class MDns;
class AudioPlayer;

class SpotifyNode: public AudioNodeWithTask, public cspot::ITrackPlayer, public IInputAudioNode {
protected:
    enum { kRecvSize = 4096 };
    enum: uint8_t {
        kCmdPlay = AudioNodeWithTask::kCommandLast + 1, /* int pos */
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
    StreamRingQueue<100> mRingBuf;
    HttpClient mHttp;
    cspot::TrackInfo::SharedPtr mCurrentTrack;
    Crypto mCrypto;
    int32_t mFileSize = 0;
    int32_t mRecvPos = 0;
    int32_t mTsSeek = 0;
    int32_t mWaitingPrefill = 0;
    mutable LinkSpeedProbe mSpeedProbe;
    StreamId mInStreamId = 0;
    StreamId mOutStreamId = 0;
    void onStopRequest() override;
    void nodeThreadFunc();
    virtual bool dispatchCommand(Command &cmd) override;
    void connect();
    bool recv();
    void prefillStart();
    void prefillComplete();
    void clearRingQueue();
    bool startCurrentTrack(bool flush, uint32_t tsSeek = 0);
    bool startNextTrack(bool flush, bool nextPrev = true);
    // cspot::TrackPlayer interface
    virtual void play(uint32_t pos) override;
    virtual void pause(bool paused) override;
    virtual void nextTrack(bool nextPrev) override;
    virtual void stopPlayback() override;
    virtual void seekMs(uint32_t pos) override;
    virtual void setVolume(uint32_t vol) override;
    // IPlayerCtrl interface
    virtual void onTrackPlaying(StreamId id, uint32_t pos) override;
    virtual IInputAudioNode* inputNodeIntf() override { return static_cast<IInputAudioNode*>(this); }
    virtual uint32_t pollSpeed() override;
    virtual uint32_t bufferedDataSize() const override { return mRingBuf.dataSize(); }
public:
    static void registerService(AudioPlayer& audioPlayer, MDns& mdns);
    SpotifyNode(IAudioPipeline& parent);
    virtual Type type() const override { return kTypeSpotify; }
    virtual StreamEvent pullData(PacketResult &pr) override;
};
#endif
