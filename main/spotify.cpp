#include "spotify.hpp"
#include <httpServer.hpp>
#include <LoginBlob.h>
#include "audioPlayer.hpp"
#include <utils.hpp>
#include <mdns.hpp>
#include <SpircHandler.h>
#include <Utils.h> // for bigNumAdd
#include <BellLogger.h>

static const char* TAG = "spotify";
std::unique_ptr<cspot::LoginBlob> SpotifyNode::sLoginBlob;
AudioPlayer* SpotifyNode::sAudioPlayer = nullptr;
const std::vector<uint8_t> SpotifyNode::sAudioAesIV = {
    0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77, 0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93
};
#define LOCK_PLAYER() MutexLocker locker(sAudioPlayer->mutex)

void SpotifyNode::registerService(AudioPlayer& audioPlayer, MDns& mdns)
{
    sAudioPlayer = &audioPlayer;
    auto& httpSvr = audioPlayer.httpServer();
    bell::setDefaultLogger();
    bell::enableTimestampLogging();
    httpSvr.on("/spotify_info", HTTP_GET, [](httpd_req_t* req) {
        if (!sLoginBlob.get()) {
            sLoginBlob = std::make_unique<cspot::LoginBlob>(AudioPlayer::mdnsName());
        }
        ESP_ERROR_CHECK(httpd_resp_set_type(req, "text/json"));
        std::string blob = sLoginBlob->buildZeroconfInfo();
      //ESP_LOGI(TAG, "Sending zeroconf data:\n%s", blob.c_str());
        return httpd_resp_send(req, blob.c_str(), blob.size());
    }, nullptr);

    httpSvr.on("/close", HTTP_GET, [](httpd_req_t* req) {
        //sLoginBlob.reset();
        return ESP_OK;
    }, nullptr);

    httpSvr.on("/spotify_info", HTTP_POST, [](httpd_req_t* req) {
        if (!sLoginBlob) {
            ESP_LOGW(TAG, "No prior GET /spotify_info request - Login BLOB doesn't exist");
            return ESP_FAIL;
        }
        DynBuffer body;
        int recvd = 0;
        int clen = req->content_len;
        body.reserve(clen + 1);
        while (recvd < clen) {
            int recvLen = httpd_req_recv(req, body.data() + recvd, clen - recvd);
            if (recvLen <= 0) {
                ESP_LOGW(TAG, "Network error receiving zeroauth postdata");
                return ESP_FAIL;
            }
            recvd += recvLen;
        }
        body.setDataSize(clen);
        body.nullTerminate();
        KeyValParser params(body.data(), body.dataSize(), false);
        if (!params.parse('&', '=', KeyValParser::kUrlUnescape)) {
            ESP_LOGW(TAG, "Error parsing zeroauth postdata");
            return ESP_FAIL;
        }
        std::map<std::string, std::string> queryMap;

        //ESP_LOGI(TAG, "zeroauth POST data map:");
        for (auto& item: params.keyVals()) {
            queryMap[item.key.str] = item.val.str;
        //    printf("\t'%s':'%s'\n", item.key.str, item.val.str);
        }
        // Pass user's credentials to the blob
        sLoginBlob->loadZeroconfQuery(queryMap);

        // We have the blob, proceed to login
        if (sAudioPlayer->mode() != AudioPlayer::kModeSpotify) {
            ESP_LOGI(TAG, "Spotify auth successful, starting client");
            LOCK_PLAYER();
            sAudioPlayer->switchMode(AudioPlayer::kModeSpotify, false);
            sAudioPlayer->play();
        }
        else {
            ESP_LOGI(TAG, "Spotify node already running");
        }
        return httpd_resp_sendstr(req,
            "{\"status\"=101,\"spotifyError\"=0,\"statusString\"=\"ERROR-OK\"}");
    }, nullptr);

    mdns.registerService(
        AudioPlayer::mdnsName(), "_spotify-connect", "_tcp", 80,
        {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});
}
SpotifyNode::SpotifyNode(IAudioPipeline& parent)
    : AudioNodeWithTask(parent, "node-spotify", true, 4096, 10, -1), mSpirc(*sLoginBlob, *this)
{
    mRingBuf.clearStopSignal();
}

void SpotifyNode::onStopRequest()
{
    mRingBuf.setStopSignal();
}
bool SpotifyNode::dispatchCommand(Command &cmd)
{
    ESP_LOGI(TAG, "Received command %d(0x%x)", cmd.opcode, cmd.arg);
    if (AudioNodeWithTask::dispatchCommand(cmd)) {
        return true;
    }
    switch(cmd.opcode) {
        case kCmdPlay:
            startCurrentTrack(true, cmd.arg);
            break;
        case kCmdNextTrack:
            startNextTrack(true, (bool)cmd.arg);
            break;
        case kCmdPause:
            stop(false);
            break;
        case kCmdStop:
        case kCmdStopPlayback:
            mCurrentTrack.reset();
            mHttp.close();
            mFileSize = 0;
            mRecvPos = 0;
            break;
        default:
            return false;
    }
    return true;
}
bool SpotifyNode::startCurrentTrack(bool flush, uint32_t tsSeek)
{
    mHttp.close();
    mCurrentTrack.reset();
    mFileSize = 0;
    mRecvPos = 0;
    mTsSeek = tsSeek;
    mCurrentTrack = mSpirc.mTrackQueue.currentTrack();
    if (!mCurrentTrack) {
        return false;
    }
    if (flush) {
        clearRingQueue();
        prefillStart();
    }
    mInStreamId = mPipeline.getNewStreamId();
    return true;
}
void SpotifyNode::clearRingQueue()
{
    ESP_LOGI(mTag, "Clearing ring buffer");
    mRingBuf.clear();
    if (mOutStreamId) { // properly terminate current stream, if its end packet was not read yet
        mRingBuf.pushBack(new StreamEndEvent(mOutStreamId));
    }
}
void SpotifyNode::prefillStart()
{
    auto fmt = StreamFormat(Codec::kCodecVorbis, 44100, 16, 2);
    mWaitingPrefill = fmt.prefillAmount();
}
void SpotifyNode::prefillComplete()
{
    mWaitingPrefill = 0;
    plSendEvent(kEventPrefillComplete, PrefillEvent::lastPrefillId());
}
bool SpotifyNode::startNextTrack(bool flush, bool nextPrev)
{
    ESP_LOGI(TAG, "Starting %s track...", nextPrev ? "next" : "prev");
    if (!mSpirc.mTrackQueue.nextTrack(nextPrev)) {
        ESP_LOGI(TAG, "nextTrack: No more tracks");
        return false;
    }
    return startCurrentTrack(flush);
}
void SpotifyNode::onTrackPlaying(StreamId id, uint32_t pos)
{
    if (id == mInStreamId) {
        mSpirc.mTrackQueue.notifyCurrentTrackPlaying(pos);
    }
    else {
        ESP_LOGW(TAG, "onTrackPlaying: streamId %d is not the one of the currently sent stream %d", id, mInStreamId);
    }
}
void SpotifyNode::play(uint32_t pos)
{
    ESP_LOGW(TAG, "Restart command: pos = %lu", pos);
    mCmdQueue.post(kCmdPlay, pos);
}
void SpotifyNode::pause(bool paused)
{
    ESP_LOGI(TAG, "Pause command");
    LOCK_PLAYER();
    if (paused) {
        sAudioPlayer->pause();
    }
    else {
        sAudioPlayer->resume();
    }
}
void SpotifyNode::nextTrack(bool nextPrev)
{
    printf("================cb nextTrack\n");
    mCmdQueue.post(kCmdNextTrack, nextPrev);
}
void SpotifyNode::stopPlayback()
{
    ESP_LOGI(TAG, "Stop command");
    LOCK_PLAYER();
    sAudioPlayer->stop();
}
void SpotifyNode::seekMs(uint32_t pos)
{
    mCmdQueue.post(kCmdSeek, pos);
}
void SpotifyNode::setVolume(uint32_t vol)
{
    // volume is 0-64
    printf("=========== volume command: %lu\n", vol);
    LOCK_PLAYER();
    sAudioPlayer->volumeSet(vol >> 10); // FIXME: volume range is 0-65536, changes in steps of 1024
}
void SpotifyNode::nodeThreadFunc()
{
    ESP_LOGI(TAG, "Task started");
    try {
        mSpirc.start();
        mSpirc.notifyVolumeSet(sAudioPlayer->volumeGet() << 10);
    }
    catch(std::exception& ex) {
        ESP_LOGE(TAG, "Error starting SPIRC: %s", ex.what());
        return;
    }
    for (;;) {
        try {
            while (mCmdQueue.numMessages() || !mCurrentTrack || (mState == kStateStopped)) {
                processMessages();
            }
            if (mState == kStateTerminated) {
                return;
            }
            myassert(mCurrentTrack);
            myassert(mState == kStateRunning);
            mRingBuf.clearStopSignal();
            if (!mHttp.connected()) {
                ESP_LOGI(TAG, "Connecting...");
                connect();
                ESP_LOGI(TAG, "Starting download...");
            }
            while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
                if (!recv()) {
                    startNextTrack(false, true);
                    break;
                }
            }
        }
        catch(std::exception& ex) {
            stop(false);
            ESP_LOGW(TAG, "Exception: %s", ex.what());
        }
    }
}
void SpotifyNode::connect()
{
    myassert(mCurrentTrack && !mCurrentTrack->cdnUrl.empty());
    std::unique_ptr<HttpClient::Headers> hdrs;
    if (mRecvPos) {
        hdrs.reset(new HttpClient::Headers({{"Range", (std::string("bytes=") + std::to_string(mRecvPos) + '-').c_str()}}));
    }
    for (int i = 0; i < 10; i++) {
        auto ret = mHttp.request(mCurrentTrack->cdnUrl.c_str(), HTTP_METHOD_GET, hdrs.get());
        if (ret >= 0) {
            mFileSize = mHttp.contentLen();
            return;
        }
        int msDelay = std::min(i * 500, 6000);
        if (mCmdQueue.waitForMessage(msDelay)) {
            throw std::runtime_error("connect: Command received while delaying retry, aborting");
        }
    }
    throw std::runtime_error("Could not connect after many retries");
}
bool SpotifyNode::recv()
{
    bool isStart = (mRecvPos == 0);
    auto toRecv = std::min(mFileSize - mRecvPos, (int32_t)kRecvSize);
    if (toRecv <= 0) {
        if (toRecv < 0) {
            ESP_LOGE(TAG, "toRecv < 0: toRecv=%ld", toRecv);
            assert(toRecv == 0);
        }
        ESP_LOGI(TAG, "recv: Track download finished");
        mRingBuf.pushBack(new StreamEndEvent(mInStreamId));
        mHttp.close();
        mCurrentTrack.reset();
        return false;
    }
    if (mRecvPos & 0x0f) {
        throw std::runtime_error("Receive offset not aligned to 16 bytes - a non-final recv got less "
                                 "than the chunk amount");
    }
    DataPacket::unique_ptr pkt(DataPacket::create(toRecv));
    auto nRecv = mHttp.recv(pkt->data, pkt->dataLen);
    if (nRecv <= 0) { // premature file end
        ESP_LOGW(TAG, "Connection dropped, will resume...");
        mHttp.close();
        return true;
    }
    pkt->dataLen = nRecv;
    {
        MutexLocker locker(mMutex);
        mSpeedProbe.onTraffic(nRecv);
    }
    auto calculatedIV = bigNumAdd(sAudioAesIV, mRecvPos >> 4);
    mRecvPos += nRecv;
    mCrypto.aesCTRXcrypt(mCurrentTrack->audioKey, calculatedIV, (uint8_t*)pkt->data, pkt->dataLen);
    if (isStart) {
        memmove(pkt->data, pkt->data + 167, pkt->dataLen - 167);
        pkt->dataLen -= 167;
        mRingBuf.pushBack(
            new NewStreamEvent(mInStreamId, Codec(Codec::kCodecVorbis, Codec::kTransportOgg), mTsSeek)
        );
        mRingBuf.pushBack(new TitleChangeEvent(strdup(mCurrentTrack->name.c_str()),
            strdup(mCurrentTrack->artist.c_str())));
        if (mWaitingPrefill) {
            mRingBuf.pushBack(new PrefillEvent(mInStreamId));
        }
    }
    mRingBuf.pushBack(pkt.release());
    if (mWaitingPrefill && mRingBuf.dataSize() >= mWaitingPrefill) {
        mWaitingPrefill = 0;
        ESP_LOGI(TAG, "Prefill complete, sending event");
        plSendEvent(kEventPrefillComplete, PrefillEvent::lastPrefillId());
    }
    return true;
}
StreamEvent SpotifyNode::pullData(PacketResult &pr)
{
    StreamPacket::unique_ptr pkt;
    pkt.reset(mRingBuf.popFront());
    if (!pkt) {
        return kErrStreamStopped;
    }
    auto type = pkt->type;
    if (type == kEvtData) {
    }
    else if(type == kEvtStreamChanged) {
        mOutStreamId = pkt.as<NewStreamEvent>().streamId;
    }
    else if (type == kEvtStreamEnd) {
        mOutStreamId = 0;
    }
    return pr.set(pkt);
}
uint32_t SpotifyNode::pollSpeed()
{
    MutexLocker locker(mMutex);
    return mSpeedProbe.poll();
}
