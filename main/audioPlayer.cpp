#include <string.h>
#include "freertos/FreeRTOS.h"
#include <esp_log.h>
#include "utils.hpp"
#include "audioPlayer.hpp"
#include "httpNode.hpp"
#include "i2sSinkNode.hpp"
#include "decoderNode.hpp"
#include "equalizerNode.hpp"
#include "a2dpInputNode.hpp"
#include "spotify.hpp"
#include <stdfonts.hpp>
#include <string>
#include <httpServer.hpp>
#include "tostring.hpp"
#include <st7735.hpp>
#include <esp_heap_caps.h>
#include "wifi.hpp" // need reference to the global Wifi instance, for createDlnaHandler
#include "asyncCall.hpp"

#define kStreamInfoFont font_Camingo22
#define kTrackTitleFont font_CamingoBold43
const LcdColor kLcdColorBackground(0, 0, 128);
const LcdColor kLcdColorCaption(255, 255, 128);
const LcdColor kLcdColorGrid(0, 128, 128);
const LcdColor kLcdColorStreamInfo(128, 255, 255);
const LcdColor kLcdColorPlayState(0, 200, 0);
const LcdColor kLcdColorNetSpeed_Normal = kLcdColorCaption;
const LcdColor kLcdColorNetSpeed_Underrun(LcdColor::RED);

#define LOCK_PLAYER() MutexLocker locker(mutex)

static constexpr const char* const TAG = "AudioPlayer";
extern std::unique_ptr<WifiBase> gWiFi;

void AudioPlayer::setLogLevel(esp_log_level_t level)
{
    esp_log_level_set(TAG, level);
}
const char* AudioPlayer::mdnsName()
{
    const char* name = nvsSimple.getString("mdnsName").get();
    return name ? name : kDefaultMdnsName;
}
void AudioPlayer::createOutputA2dp()
{
    /*
    assert(!mStreamOut);
    ESP_LOGI(TAG, "Creating a2dp output source");
    ESP_LOGI(TAG, "\tCreating Bluetooth service");
    bluetooth_service_cfg_t cfg;
    cfg.device_name = "ESP-ADF-SOURCE";
    cfg.mode = BLUETOOTH_A2DP_SOURCE;
    cfg.remote_name = "DL-LINK";
    ESP_ERROR_CHECK(bluetooth_service_start(&cfg));
    ESP_LOGI(TAG, "\tCreating bluetooth sink element");
    mStreamOut = bluetooth_service_create_stream();
    assert(mStreamOut);
    const uint8_t* addr = esp_bt_dev_get_address();
    char strAddr[13];
    binToHex(addr, 6, strAddr);
    ESP_LOGW("BT", "Own BT MAC: '%s'", strAddr);
//  Move this to execute only once
    */
}
AudioNode::Type AudioPlayer::playerModeToInNodeType(PlayerMode mode)
{
    return (uint8_t)mode & (uint8_t)AudioNode::kTypeHttpIn ? AudioNode::kTypeHttpIn : (AudioNode::Type)mode;
}

AudioPlayer::AudioPlayer(ST7735Display& lcd, http::Server& httpServer, PlayerMode mode, AudioNode::Type outType)
: mNvsHandle("aplayer", NVS_READWRITE), mLcd(lcd),
  mDmaFrameBuf(320, kTrackTitleFont.height + kTrackTitleFont.lineSpacing, MALLOC_CAP_DMA),
  mTitleTextFrameBuf(kMaxTrackTitleLen * (kTrackTitleFont.width + kTrackTitleFont.charSpacing),
                     kTrackTitleFont.height + kTrackTitleFont.lineSpacing, MALLOC_CAP_SPIRAM),
  mHttpServer(httpServer), mVuDisplay(mDmaFrameBuf)
{
    lcdInit();
    mNvsHandle.enableAutoCommit(20000);
    if (mode == kModeInvalid)
    {
        mode = initFromNvs();
    }
    else
    {
        if (!playerModeIsValid(mode)) {
            ESP_LOGW(TAG, "init: Specfied player mode %d is invalid, defaulting to %s",
                mode, playerModeToStr(kModeDefault));
            mode = kModeDefault;
        }
        createPipeline(playerModeToInNodeType(mode), outType);
    }
    auto inType = inputType();
    if (inType == AudioNode::kTypeHttpIn) {
        stationList.reset(new StationList(mutex));
    }
    lcdDrawGui();
    setPlayerMode(mode);
    initTimedDrawTask();
    registerUrlHanlers();
    if (inType == AudioNode::kTypeHttpIn) {
        createDlnaHandler();
        mDlna->start();
    }
    SpotifyNode::registerService(*this, mdns);
}
AudioPlayer::PlayerMode AudioPlayer::initFromNvs()
{
    auto playerMode = (PlayerMode)mNvsHandle.readDefault<uint8_t>("playerMode", kModeRadio);
    if (!playerModeIsValid(playerMode)) {
        playerMode = kModeRadio;
    }
    bool ok = createPipeline(playerModeToInNodeType(playerMode), AudioNode::kTypeI2sOut);
    if (!ok) {
        ESP_LOGE(TAG, "Failed to create audio pipeline");
        myassert(false);
    }
    return playerMode;
}
bool AudioPlayer::playerModeIsValid(PlayerMode mode)
{
    return mode == kModeRadio || mode == kModeDlna || mode == kModeUrl ||
           mode == kModeBluetoothSink || mode == kModeSpdifIn;
}

void AudioPlayer::createDlnaHandler()
{
    assert(gWiFi);
    char url[48];
    snprintf(url, sizeof(url), "http%s://" IPSTR ":%d", mHttpServer.isSsl() ? "s":"",
             IP2STR(&gWiFi->localIp()), 80);
    mDlna.reset(new DlnaHandler(mHttpServer, url, *this));
}
void AudioPlayer::lcdInit()
{
    mDmaFrameBuf.setBgColor(kLcdColorBackground);
    mDmaFrameBuf.clear();
    mLcd.dmaMountFrameBuffer(mDmaFrameBuf);
    mTitleTextFrameBuf.setBgColor(kLcdColorBackground);
    mTitleTextFrameBuf.setFgColor(kLcdColorCaption);
    mTitleTextFrameBuf.setFont(kTrackTitleFont);
    mVuDisplay.init(mNvsHandle);
}

void AudioPlayer::initTimedDrawTask()
{
//    xTaskCreatePinnedToCore(&lcdTimedDrawTaskFunc, "lcdTask", kLcdTaskStackSize, this, kLcdTaskPrio, nullptr, kLcdTaskCore);
    mLcdTask.createTask("lcdTask", true, kLcdTaskStackSize, kLcdTaskCore, kLcdTaskPrio, this, &AudioPlayer::lcdTimedDrawTask);
}

bool AudioPlayer::createPipeline(AudioNode::Type inType, AudioNode::Type outType)
{
    ESP_LOGI(TAG, "Creating audio pipeline");
    AudioNode* pcmSource = nullptr;
    switch(inType) {
    case AudioNode::kTypeHttpIn:
    case AudioNode::kTypeSpotify: {
        mStreamIn.reset(inType == AudioNode::kTypeHttpIn
                        ? (AudioNodeWithState*)new HttpNode(*this)
                        : (AudioNodeWithState*)new SpotifyNode(*this));
        mDecoder.reset(new DecoderNode(*this));
        mDecoder->linkToPrev(mStreamIn.get());
        pcmSource = mDecoder.get();
        break;
    }
    case AudioNode::kTypeA2dpIn:
        mStreamIn.reset(new A2dpInputNode(*this));
        mDecoder.reset();
        pcmSource = mStreamIn.get();
        break;
    default:
        ESP_LOGE(TAG, "Unknown pipeline input node type %d", inType);
        return false;
    }
    mEqualizer.reset(new EqualizerNode(*this, mNvsHandle));
    mEqualizer->linkToPrev(pcmSource);
    pcmSource = mEqualizer.get();

    switch(outType) {
    case AudioNode::kTypeI2sOut: {
        uint8_t dmaBufTimeMs = mNvsHandle.readDefault<uint8_t>("i2s.dmaBufMs", kI2sDmaBufMs);
        if (dmaBufTimeMs < 30 || dmaBufTimeMs > 200) {
            ESP_LOGE(TAG, "Bad i2s.dmaBufMs config value %u, defaulting to %u", dmaBufTimeMs, kI2sDmaBufMs);
            dmaBufTimeMs = kI2sDmaBufMs;
        };
        I2sOutputNode::Config cfg = {
            .port = 0, .pin_dout = 27, .pin_ws = 25, .pin_bclk = 26,
            .dmaBufSizeMs = dmaBufTimeMs, .dmaBufSizeMax = kDmaBufSizeMax
        };
        mStreamOut.reset(new I2sOutputNode(*this, cfg, kI2sStackSize, kI2sCpuCore));
        break;
    }
    /*
    case kOutputA2dp:
        createOutputA2dp();
        break;
    */
    default:
        ESP_LOGE(TAG, "Unknown pipeline output node type %d", outType);
        return false;
    }
    mStreamOut->linkToPrev(pcmSource);
    // VU stuff
    mVolumeInterface = mVuLevelInterface = mEqualizer.get();
    mVolumeInterface->volEnableProcessing(true);
    // setup VU level probe point
    auto vuAtInput = mNvsHandle.readDefault<uint8_t>("vuAtEqInput", 0);
    mVuLevelInterface->volEnableLevel(audioLevelCb, this, vuAtInput ? 0 : 1);
    ESP_LOGI(TAG, "VU source set %s volume and EQ processing", vuAtInput ? "before" : "after");
    // ====
    ESP_LOGI(TAG, "Audio pipeline:\n%s", printPipeline().c_str());
    loadSettings();
    return true;
}

void AudioPlayer::lcdDrawGui()
{
    mLcd.setBgColor(0, 0, 128);
    mLcd.clear();
    mLcd.setFgColor(kLcdColorGrid);
    mLcd.setFont(font_Camingo22);
    mLcd.hLine(0, mLcd.width()-1, mLcd.fontHeight() + 3);
    auto vuTop = mLcd.height() - mVuDisplay.height();
    mLcd.hLine(0, mLcd.width()-1, vuTop - kStreamInfoFont.height - 4);
    mLcd.hLine(0, mLcd.width()-1, vuTop - 2);
}
void AudioPlayer::setPlayerMode(PlayerMode mode)
{
    if (mPlayerMode == mode) {
        return;
    }
    mPlayerMode = mode;
    mTrackInfo.reset();
    lcdUpdateTrackDisplay();
    mLcd.setFont(font_Camingo22);
    mLcd.setBgColor(0, 0, 128);
    mLcd.clear(0, 0, mLcd.width(), mLcd.fontHeight() + 2);
    mLcd.setFgColor(kLcdColorCaption);
    mLcd.gotoXY(1, 1);
    mLcd.puts(playerModeToStr(mPlayerMode));
}
void AudioPlayer::lcdUpdateTrackDisplay()
{
    if (mPlayerMode == kModeRadio) {
        if (stationList) {
            lcdUpdateStationInfo();
        }
    } else if (mTrackInfo) {
        lcdUpdateArtistName(mTrackInfo->artistName);
        lcdUpdateTrackTitle(mTrackInfo->trackName);
    }
    else if (mStreamIn && mStreamIn->type() == AudioNode::kTypeHttpIn) {
        lcdUpdateArtistName("Playing URL");
        auto& http = *static_cast<HttpNode*>(mStreamIn.get());
        MutexLocker locker(http.mMutex);
        lcdUpdateTrackTitle(http.url());
    }
    else {
        lcdUpdateArtistName(nullptr);
        lcdUpdateTrackTitle(nullptr);
    }
}
void AudioPlayer::lcdUpdateStationInfo()
{
    assert(mPlayerMode == kModeRadio);
    auto& station = stationList->currStation;
    if (!station.isValid()) {
        return;
    }
    lcdUpdateArtistName(station.name());
    lcdUpdateTrackTitle(nullptr);
// station flags
    mLcd.setFont(kPictoFont);
    mLcd.cursorY = 0;
    mLcd.cursorX = (mLcd.width() - mLcd.charWidth(kSymFavorite)) / 2;
    if (station.flags() & Station::kFlagFavorite) {
        mLcd.setFgColor(255, 0, 0);
        mLcd.putc(kSymFavorite);
    } else {
        mLcd.putc(kSymBlank);
    }
}
void AudioPlayer::lcdUpdateArtistName(const char* name)
{
    mLcd.setFont(font_Camingo32);
    mLcd.clear(0, kLcdArtistNameLineY, mLcd.width(), mLcd.fontHeight());
    if (name) {
        mLcd.gotoXY(0, kLcdArtistNameLineY);
        mLcd.setFgColor(kLcdColorCaption);
        mLcd.putsCentered(name);
    }
}

void AudioPlayer::lcdUpdatePlayState(const char* text, bool isRecording)
{
    mLcd.setFont(font_Camingo22);
    mLcd.clear(0, kLcdPlayStateLineY, mLcd.width(), mLcd.fontHeight());
    if (text) {
        mLcd.setFgColor(kLcdColorPlayState);
        mLcd.gotoXY(0, kLcdPlayStateLineY);
        mLcd.putsCentered(text);
        return;
    }
    // playing, maybe display REC indicator
    if (isRecording) {
        mLcd.setFgColor(255, 0, 0);
    }
    else { // maybe display a REC-enabled-but-not-active indicator
        if ((mPlayerMode != kModeRadio) || !mStreamIn.get() || !stationList) {
            return;
        }
        auto& station = stationList->currStation;
        if (!station.isValid() || !(station.flags() & Station::kFlagRecord)) {
            return;
        }
        mLcd.setFgColor(0xF68E); // orange
    }
    mLcd.gotoXY(0, kLcdPlayStateLineY);
    mLcd.putsCentered("rec");
}

std::string AudioPlayer::printPipeline()
{
    std::string result;
    result.reserve(128);
    std::vector<AudioNode*> nodes;
    nodes.reserve(6);
    for (AudioNode* node = mStreamOut.get(); node; node = node->prev()) {
        nodes.push_back(node);
    }
    for (int i = nodes.size()-1; i >= 0; i--) {
        auto node = nodes[i];
        result.append(node->tag());
        if (node->volumeInterface() == mVolumeInterface) {
            result.append("(v)");
        }
        if (i > 0) {
            result.append(" --> ");
        }
    }
    return result;
}

void AudioPlayer::switchMode(PlayerMode playerMode, bool persist)
{
    if (mPlayerMode == playerMode) {
        return;
    }
    if (persist) {
        mNvsHandle.write("playerMode", (uint8_t)playerMode);
    }
    auto oldType = mStreamIn ? mStreamIn->type() : 0;
    auto newType = playerModeToInNodeType(playerMode);
    if (newType != oldType) {
        destroyPipeline();
        if (persist) {
            initFromNvs();
        }
        else {
            createPipeline(newType, AudioNode::kTypeI2sOut);
        }
    }
    setPlayerMode(playerMode); // must be after the pipeline update, because it gets info from input nodes
}

void AudioPlayer::loadSettings()
{
    mVolumeCap = mNvsHandle.readDefault<uint8_t>("maxVolume", 255);
    if (mVolumeInterface) {
        auto vol = mNvsHandle.readDefault<uint8_t>("volume", kDefaultVolume);
        ESP_LOGI(TAG, "Setting volume to %u", vol);
        mVolumeInterface->setVolume(vol);
    }
}

void AudioPlayer::destroyPipeline()
{
    if (!mStreamIn) {
        return;
    }
    stop();
    mStreamIn.reset();
    mDecoder.reset();
    mEqualizer.reset();
    mStreamOut.reset();
}

bool AudioPlayer::doPlayUrl(TrackInfo* trackInfo, PlayerMode playerMode, const char* record)
{
    myassert(playerMode & (int)AudioNode::kTypeHttpIn);
    switchMode(playerMode, false);
    mTrackInfo.reset(trackInfo);
    auto& http = *static_cast<HttpNode*>(mStreamIn.get());
    // setUrl will start the http node, if it's stopped. However, this may take a while.
    // If we meanwhile start the i2s out node, it will start to pull data from the not-yet-started http node,
    // whose state may not be set up correctly for the new stream (i.e. waitingPrefill not set)
    stop(nullptr);
    lcdUpdateTrackDisplay();
    lcdResetNetSpeedIndication();
    http.setUrlAndStart(HttpNode::UrlInfo::Create(trackInfo->url, getNewStreamId(), record));
    if (http.waitForState(AudioNode::kStateRunning, 10000) != AudioNode::kStateRunning) {
        return false;
    }
    play();
    return true;
}
bool AudioPlayer::playUrl(const char* url, PlayerMode playerMode, const char* record)
{
    return doPlayUrl(TrackInfo::Create(url, nullptr, nullptr, 0), playerMode, record);
}
bool AudioPlayer::playUrl(TrackInfo* trackInfo, PlayerMode playerMode, const char* record)
{
    return doPlayUrl(trackInfo, playerMode, record);
}
const char* AudioPlayer::url() const // needed by DLNA
{
    if (!mStreamIn || mStreamIn->type() != AudioNode::kTypeHttpIn) {
        return nullptr;
    }
    if (mTrackInfo) {
        return mTrackInfo->url;
    }
    auto& http = *static_cast<HttpNode*>(mStreamIn.get());
    return http.url();
}
esp_err_t AudioPlayer::playStation(const char* id)
{
    myassert(id);
    switchMode(kModeRadio, false);
    if (!this->stationList) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(id, ".") == 0) { // "." = current station
        if (!this->stationList->currStation.isValid() && !this->stationList->next()) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    else if (strcmp(id, "+") == 0) { // "+" = next station
        if (!this->stationList->next()) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    else if (!this->stationList->setCurrent(id)) {
        return ESP_ERR_NOT_FOUND;
    }

    auto& station = this->stationList->currStation;
    assert(station.isValid());
    if (!playUrl(station.url(), kModeRadio, (station.flags() & station.kFlagRecord) ? station.id() : nullptr)) {
        return ESP_ERR_NOT_SUPPORTED; // stream source node is not http client
    }
    return ESP_OK;
}

bool AudioPlayer::isStopped() const
{
    if (!mStreamIn || !mStreamOut) {
        return true;
    }
    return mStreamIn->state() == AudioNode::kStateStopped ||
           mStreamOut->state() == AudioNode::kStateStopped;
}
bool AudioPlayer::isPlaying() const
{
    if (!mStreamIn || !mStreamOut) {
        return false;
    }
    return mStreamIn->state() == AudioNode::kStateRunning ||
           mStreamOut->state() == AudioNode::kStateRunning;

}

void AudioPlayer::play()
{
    mStreamIn->run();
    if (mDecoder) {
        mDecoder->run();
        mDecoder->waitForState(AudioNode::kStateRunning);
    }
    mStreamOut->run();
    if (mDlna) {
        mDlna->notifyPlayStart();
    }
}

void AudioPlayer::resume()
{
    play();
}
void AudioPlayer::pipelineStop()
{
    mStopping = true;
    mStreamIn->stop(false);
    mStreamOut->stop(false);
    if (mDecoder) {
        mDecoder->stop(false);
        mDecoder->waitForStop();
    }
    mStreamIn->waitForStop();
    mStreamOut->waitForStop();
    mStopping = false;
}
void AudioPlayer::pause()
{
    stop("Paused");
}
void AudioPlayer::stop()
{
    stop("Stopped");
}
void AudioPlayer::stop(const char* caption)
{
    pipelineStop();
    if (mVuLevelInterface) {
        mVuLevelInterface->clearAudioLevels();
    }
    lcdUpdatePlayState(caption);
    if (mDlna) {
        mDlna->notifyPlayStop();
    }
}
uint32_t AudioPlayer::positionTenthSec() const
{
    if (!isPlaying() || !mStreamOut || (mStreamOut->type() != AudioNode::kTypeI2sOut)) {
        return 0;
    }
    auto& i2sOut = *static_cast<I2sOutputNode*>(mStreamOut.get());
    MutexLocker locker(i2sOut.mutex);
    if (i2sOut.mStreamId != mCurrentStreamId) {
        return 0;
    }
    return i2sOut.positionTenthSec();
}
uint8_t AudioPlayer::volumeSet(uint8_t vol)
{
    if (vol > mVolumeCap) {
        ESP_LOGW(TAG, "Artificially limiting volume %u to %u", vol, mVolumeCap);
        vol = mVolumeCap;
    }
    if (isMuted() && vol <= mMuteVolume) {
        mMuteVolume = vol;
    }
    else {
        mMuteVolume = -1;
        if (mVolumeInterface) {
            mVolumeInterface->setVolume(vol);
            vol = mVolumeInterface->getVolume();
        }
    }
    // avoid writing directly to flash from this thread, as it is called by PSRAM-based threads
    asyncCall([this, vol]() {
        mNvsHandle.write("volume", vol);
    });
    if (mDlna.get()) {
        mDlna->notifyVolumeChange(vol);
    }
    return vol;
}

int AudioPlayer::volumeGet()
{
    if (mMuteVolume >= 0) {
        return mMuteVolume;
    }
    else if (mVolumeInterface) {
        return mVolumeInterface->getVolume();
    }
    return -1;
}
void AudioPlayer::mute()
{
    if (!mVolumeInterface) {
        return;
    }
    if (mMuteVolume > -1) {
        ESP_LOGI(TAG, "mute: Already muted");
    } else {
        mMuteVolume = mVolumeInterface->getVolume();
        mVolumeInterface->setVolume(0);
    }
    if (mDlna) {
        mDlna->notifyMute(true, mMuteVolume);
    }
}
void AudioPlayer::unmute()
{
    if (!mVolumeInterface) {
        return;
    }
    if (mMuteVolume < 0) {
        ESP_LOGI(TAG, "unmute: Not muted");
    }
    else {
        mVolumeInterface->setVolume(mMuteVolume);
        mMuteVolume = -1;
    }
    if (mDlna) {
        mDlna->notifyMute(false, mVolumeInterface->getVolume());
    }
}
int AudioPlayer::volumeChange(int step)
{
    int currVol = volumeGet();
    if (currVol < 0) {
        return -1;
    }
    int newVol = currVol + step;
    if (newVol < 0) {
        newVol = 0;
    } else if (newVol > 100) {
        newVol = 100;
    }
    if (newVol != currVol) {
        volumeSet(newVol);
    }
    return newVol;
}

bool AudioPlayer::equalizerSetGainsFromString(char* str, size_t len)
{
    if (!str) {
        mEqualizer->setAllGains(nullptr, 0);
        mEqualizer->saveGains();
        return true;
    }
    KeyValParser vals(str, len + 1);
    vals.parse(';', '=', KeyValParser::kTrimSpaces);
    bool ok = true;
    for (const auto& kv: vals.keyVals()) {
        int band = kv.key.toInt(-1);
        if (band < 0 || band > 9) {
            ESP_LOGW(TAG, "Invalid band %d", band);
            ok = false;
            continue;
        }
        int gain = kv.val.toInt(-127);
        if (gain == -127) {
            ESP_LOGW(TAG, "Invalid gain '%s'", kv.val.str);
            ok = false;
            continue;
        }
        ESP_LOGI(TAG, "Setting band %d gain %d", band, gain);
        mEqualizer->setBandGain(band, gain);
    }
    mEqualizer->saveGains();
    return ok;
}

AudioPlayer::~AudioPlayer()
{
    destroyPipeline();
}

esp_err_t AudioPlayer::playUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    UrlParams params(req);
    DynBuffer buf(128);
    auto param = params.strVal("url");
    if (param) {
        self->playUrl(param.str, kModeUrl, nullptr);
        buf.printf("Playing url '%s'", param.str);
    }
    else if ((param = params.strVal("sta"))) {
        auto err = self->playStation(param.str);
        if (err != ESP_OK) {
            switch(err) {
                case ESP_ERR_INVALID_STATE:
                    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No station list present");
                    break;
                case ESP_ERR_NOT_FOUND:
                    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Station not found");
                    break;
                case ESP_ERR_NOT_SUPPORTED:
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Radio stations not supported in this mode");
                    break;
                default:
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown error");
                    break;
            }
            return ESP_FAIL;
        }
        buf.printf("Playing station '%s'", self->stationList->currStation.name());
    }
    httpd_resp_send(req, buf.buf(), buf.dataSize());
    return ESP_OK;
}

esp_err_t AudioPlayer::pauseUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    if (self->isPlaying()) {
        self->pause();
        httpd_resp_sendstr(req, "Pause");
    } else {
        self->play();
        httpd_resp_sendstr(req, "Play");
    }
    return ESP_OK;
}

void AudioPlayer::registerHttpGetHandler(const char* path, esp_err_t(*handler)(httpd_req_t*))
{
    httpd_uri_t desc = {
        .uri       = path,
        .method    = HTTP_GET,
        .handler   = handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(mHttpServer.handle(), &desc);
}

esp_err_t AudioPlayer::volumeUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    UrlParams params(req);
    int newVol;
    auto step = params.intVal("step", 0);
    if (step) {
        newVol = self->volumeChange(step);
        if (newVol < 0) {
            httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "Error setting volume");
            return ESP_OK;
        }
    } else {
        auto vol = params.intVal("vol", -1);
        if (vol < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Neither 'step' nor 'vol' parameter provided");
            return ESP_OK;
        }
        self->volumeSet(vol);
        newVol = self->volumeGet();
    }
    DynBuffer buf(24);
    buf.printf("Volume set to %d", newVol);
    httpd_resp_send(req, buf.buf(), buf.dataSize());
    return ESP_OK;
}
esp_err_t respondOkOrFail(bool ok, httpd_req_t* req)
{
    if (ok) {
        httpd_resp_sendstr(req, "ok");
        return ESP_OK;
    } else {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
}
esp_err_t AudioPlayer::equalizerSetUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    if (!self->mEqualizer) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No equalizer node");
        return ESP_FAIL;
    }
    auto& eq = *self->mEqualizer;
    UrlParams params(req);
    auto nbands = params.intVal("nbands", -1);
    if (nbands != -1) {
        return respondOkOrFail(eq.setMyEqNumBands(nbands), req);
    }
    auto preset = params.strVal("preset");
    if (preset.str) {
        return respondOkOrFail(eq.switchPreset(preset.str), req);
    }
    int useEsp = params.intVal("esp", -1);
    if (useEsp != -1) {
        eq.useEspEqualizer(useEsp);
        httpd_resp_sendstr(req, "ok");
        return ESP_OK;
    }
    auto cfgBand = params.intVal("cfgband", -1);
    if (cfgBand > -1) {
        auto freq = params.intVal("freq", 0);
        auto bw = params.intVal("bw", 0);
        return respondOkOrFail(eq.reconfigEqBand(cfgBand, freq, bw), req);
    }

    auto data = params.strVal("vals");
    if (data.str) {
        return respondOkOrFail(self->equalizerSetGainsFromString(data.str, data.len), req);
    }
    if (params.strVal("reset").str) {
        return respondOkOrFail(self->equalizerSetGainsFromString(nullptr, 0), req);
    }
    auto enable = params.intVal("enable", -1);
    if (enable != -1) {
        eq.disable(!enable);
        httpd_resp_sendstr(req, "ok");
        return ESP_OK;
    }
    auto band = params.intVal("band", -1);
    if (band < 0 || band >= eq.numBands()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'band' parameter");
        return ESP_FAIL;
    }
    auto level = params.intVal("level", -127);
    if (level == -127) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'level' parameter");
        return ESP_OK;
    }
    eq.setBandGain(band, level);
    return respondOkOrFail(eq.saveGains(), req);
}

esp_err_t AudioPlayer::equalizerDumpUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    if (!self->mEqualizer) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No equalizer node");
        return ESP_FAIL;
    }
    auto& eq = *self->mEqualizer;
    MutexLocker eqLocker(eq.mMutex);
    auto levels = eq.gains();
    DynBuffer buf(240);
    buf.printf("{\"t\":%d,\"n\":\"%s\",\"b\":[", eq.eqType(), eq.presetName());
    for (int i = 0; i < eq.numBands(); i++) {
        auto cfg = eq.bandCfg(i);
        buf.printf("[%d,%d,%d],", cfg.freq, cfg.width, levels[i]);
    }
    buf.setDataSize(buf.dataSize() - 2); // remove terminating null
    buf.appendStr("]}", true);
    httpd_resp_send(req, buf.buf(), buf.dataSize()-1);
    return ESP_OK;
}

esp_err_t AudioPlayer::getStatusUrlHandler(httpd_req_t* req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    DynBuffer buf;
    auto in = self->mStreamIn.get();
    if (!in) {
        buf.reserve(32);
        buf.printf("{\"state\":0,\"fmem\":%d}", xPortGetFreeHeapSize());
        httpd_resp_sendstr(req, buf.buf());
        return ESP_OK;
    }
    buf.printf("{\"state\":%d,\"src\":\"%s\"", in->state(), in->tag());
    if (in->state() == AudioNode::kStateRunning &&
        in->type() == AudioNode::kTypeHttpIn) {
            auto http = static_cast<HttpNode*>(in);
            MutexLocker locker(http->mMutex);
            auto& icy = http->icyInfo();
            if (icy.staName()) {
                buf.printf(",\"sname\":\"%s\"", icy.staName());
            }
            if (icy.staDesc()) {
                buf.printf(",\"sdesc\":\"%s\"", icy.staDesc());
            }
            if (icy.staGenre()) {
                buf.printf(",\"sgenre\":\"%s\"", icy.staGenre());
            }
            if (icy.staUrl()) {
                buf.printf(",\"surl\":\"%s\"", icy.staUrl());
            }
            if (icy.trackName()) {
                buf.printf(",\"track\":\"%s\"", icy.trackName());
            }
    }
    buf.printf("}");
    httpd_resp_sendstr(req, buf.buf());
    return ESP_OK;
}
esp_err_t AudioPlayer::nvsSetParamUrlHandler(httpd_req_t* req)
{
    UrlParams params(req);
    auto key = params.strVal("key");
    if (!key.str || !key.str[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No 'key' param specified");
        return ESP_FAIL;
    }
    auto strVal = params.strVal("val");
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    if (!strVal.str) {
        auto err = self->mNvsHandle.eraseKey(key.str);
        if (err == ESP_OK) {
            httpd_resp_sendstr(req, "Key deleted");
            return ESP_OK;
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
            return ESP_FAIL;
        }
    }
    auto type = params.strVal("type");
    if (!type.str) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No 'type' param specified");
        return ESP_FAIL;
    }
    auto err = self->mNvsHandle.writeValueFromString(key.str, type.str, strVal.str);
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "ok");
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }
}
esp_err_t AudioPlayer::nvsGetParamUrlHandler(httpd_req_t* req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    UrlParams params(req);
    auto key = params.strVal("key");
    auto nsParam = params.strVal("ns");
    const char* ns;
    std::unique_ptr<NvsHandle> nvsHolder;
    NvsHandle* nvs;
    if (nsParam.str) {
        ns = nsParam.str;
        nvsHolder.reset(new NvsHandle(ns));
        nvs = nvsHolder.get();
    } else {
        ns = "aplayer";
        nvs = &self->mNvsHandle;
    }
    httpd_resp_set_type(req, "text/json");
    DynBuffer buf(128);
    MutexLocker locker(self->mutex);
    httpd_resp_send_chunk(req, "{", 1);
    bool isFirst = true;
    nvs_iterator_t it;
    for(it = nvsEntryFind("nvs", ns, NVS_TYPE_ANY); it; it = nvsEntryNext(it)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (key.str && strcmp(info.key, key.str) != 0) {
            continue;
        }
        buf.clear();
        if (!isFirst) {
            buf.appendChar(',');
        }
        isFirst = false;
        buf.appendChar('"').appendStr(info.key).appendChar('"').appendChar(':');
        auto err = nvs->valToString(info.key, info.type, buf, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NvsHandle::valToString error %s, key: '%s'", esp_err_to_name(err), info.key);
        }
        httpd_resp_send_chunk(req, buf.buf(), buf.dataSize()); // don't send null terminator
    }
    nvs_release_iterator(it);
    httpd_resp_send_chunk(req, "}", 1);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}
esp_err_t AudioPlayer::resetSubsystemUrlHandler(httpd_req_t* req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    UrlParams params(req);
    auto key = params.strVal("what");
    if (!key.str) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No 'what' parameter");
        return ESP_FAIL;
    }
    MutexLocker locker(self->mutex);
    if (strcmp(key.str, "vu") == 0) {
        self->mVuDisplay.reset(self->mNvsHandle);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown subsystem");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

esp_err_t AudioPlayer::changeInputUrlHandler(httpd_req_t *req)
{
    UrlParams params(req);
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    auto type = params.strVal("mode");
    if (!type) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No mode param");
        return ESP_OK;
    }
    switch(type.str[0]) {
        case 'b':
            self->switchMode(kModeBluetoothSink);
            httpd_resp_sendstr(req, "Switched to Bluetooth A2DP sink");
            break;
        case 'h':
            self->switchMode(kModeRadio);
            httpd_resp_sendstr(req, "Switched to HTTP client (net radio)");
            break;
        default:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode param");
            return ESP_OK;
    }
    ESP_LOGI(TAG, "Changed input node to type %d", self->nvs().readDefault<uint8_t>("playerMode", kModeDefault));
    return ESP_OK;
}

void AudioPlayer::registerUrlHanlers()
{
    mHttpServer.on("/play", HTTP_GET, &playUrlHandler, this);
    mHttpServer.on("/pause", HTTP_GET, &pauseUrlHandler, this);
    mHttpServer.on("/vol", HTTP_GET, &volumeUrlHandler, this);
    mHttpServer.on("/eqget", HTTP_GET, &equalizerDumpUrlHandler, this);
    mHttpServer.on("/eqset", HTTP_GET, &equalizerSetUrlHandler, this);
    mHttpServer.on("/status", HTTP_GET, &getStatusUrlHandler, this);
    mHttpServer.on("/nvget", HTTP_GET, &nvsGetParamUrlHandler, this);
    mHttpServer.on("/nvset", HTTP_GET, &nvsSetParamUrlHandler, this);
    mHttpServer.on("/inmode", HTTP_GET, &changeInputUrlHandler, this);
    mHttpServer.on("/reset", HTTP_GET, &resetSubsystemUrlHandler, this);
    if (stationList) {
        stationList->registerHttpHandlers(mHttpServer.handle());
    }
}

void AudioPlayer::onNodeError(AudioNode& node, int error, uintptr_t arg)
{
    if (mStopping) {
        const char* errName = streamEventToStr((StreamEvent)error);
        ESP_LOGW(TAG, "Discarding error %s from node '%s' while stopping", errName, node.tag());
        return;
    }
    mStopping = true;
    asyncCall([this, error, nodeName = std::string(node.tag())]() {
        const char* errName = streamEventToStr((StreamEvent)error);
        ESP_LOGW(TAG, "Error %s from node '%s', stopping pipeline", errName, nodeName.c_str());
        LOCK_PLAYER();
        stop(errName);
    });
}

bool AudioPlayer::onNodeEvent(AudioNode& node, uint32_t event, size_t numArg, uintptr_t arg)
{
    // For most of the events, we do an asyncCall to access the player, to avoid deadlocks
    // Must not do any locking from here, so we call into the player via async messages
    if (event == AudioNode::kEventTrackInfo) {
        const std::string title((const char*)arg);
        ESP_LOGI(TAG, "Received title event: '%s'", title.c_str());
        asyncCall([this, title]() {
            LOCK_PLAYER();
            ElapsedTimer t;
            lcdUpdateTrackTitle(title.c_str());
            printf("set title: %d\n", t.msElapsed());
        });
    }
    else if (event == AudioNode::kEventNewStream) {
        auto& evt = *(NewStreamEvent*)arg;
        asyncCall([this, streamId = evt.streamId, fmt = evt.fmt, sourceBps = evt.sourceBps]() {
            LOCK_PLAYER();
            onNewStream(fmt, sourceBps);
            if (mStreamIn) {
                printf("input intf: %p\n", mStreamIn->inputNodeIntf());
                mStreamIn->inputNodeIntf()->onTrackPlaying(streamId, 0);
            }
        });
    }
    else if (event == AudioNode::kEventPrefillComplete) {
        LOCK_PLAYER();
        if (mStreamOut) {
            static_cast<I2sOutputNode*>(mStreamOut.get())->notifyPrefillComplete(numArg);
        }
    }
    else {
        asyncCall([this, event, arg, numArg]() {
            LOCK_PLAYER();
            switch(event) {
                case AudioNode::kEventConnected:
                    return lcdUpdatePlayState(numArg ? nullptr : "Buffering..."); break;
                case AudioNode::kEventConnecting:
                    return lcdUpdatePlayState(numArg ? "Reconnecting..." : "Connecting..."); break;
                case AudioNode::kEventPlaying:
                    return lcdUpdatePlayState(nullptr); break;
                case HttpNode::kEventRecording:
                    return lcdUpdatePlayState(nullptr, numArg); break;
                case AudioNode::kEventBufUnderrun:
                    return lcdShowBufUnderrunImmediate(); break;
                case AudioNode::kEventStreamEnd:
                    if ((uint8_t)mPlayerMode & (uint8_t)AudioNode::kTypeFlagPlayerCtrl) {
                        break;
                    }
                    else if (numArg == mCurrentStreamId) {
                        this->stop();
                    }
                    else {
                        ESP_LOGI(TAG, "Discarding stream end event for a previous streamId %d", numArg);
                    }
                    break;
                default: break;
            }
        });
    }
    return true;
}
void AudioPlayer::lcdTimedDrawTask()
{
    int16_t fps;
    {
        LOCK_PLAYER();
        fps = mNvsHandle.readDefault<uint8_t>("tscrollFps", kDefTitleScrollFps);
    }
    int64_t now = esp_timer_get_time();
    int64_t tsLastSpeedUpdate = now - kLcdNetSpeedUpdateIntervalUs;
    int8_t waitTicks = ((1000 / (2 * fps)) + portTICK_PERIOD_MS / 2) / portTICK_PERIOD_MS;
    bool vuOrScroll = false;
    for (;;) {
        vTaskDelay(waitTicks);
        LOCK_PLAYER();
        now = esp_timer_get_time();
        if (now - tsLastSpeedUpdate > kLcdNetSpeedUpdateIntervalUs) {
            tsLastSpeedUpdate = now;
            lcdUpdateNetSpeed();
        }
        mLcd.waitDone(); // wait while player locked, so that someone else doesn't start another operation
        if ((vuOrScroll = !vuOrScroll)) {
            if (now - mTsLastVuLevel > 100000 && mVolumeInterface->audioLevels().data) { // 100ms no volume event, set level to zero
                ESP_LOGI(TAG, "No sound output, clearing VU levels");
                mVolumeInterface->clearAudioLevelsNoEvent();
            }
            mVuDisplay.update(mVuLevels);
            mLcd.dmaBlit(0, mLcd.height() - mVuDisplay.height(), mLcd.width(), mVuDisplay.height());
        }
        else {
            if (mTitleScrollEnabled == 1) {
                lcdScrollTrackTitle();
                mLcd.dmaBlit(0, kLcdTrackTitleY, mDmaFrameBuf.width(), mDmaFrameBuf.height());
            }
        }
    }
}

void AudioPlayer::lcdUpdateTrackTitle(const char* buf)
{
    if (!buf || !buf[0]) {
        mTitleScrollEnabled = false;
        if (!mLcdTrackTitle.isEmpty()) {
            mLcdTrackTitle.clear();
            mLcd.gotoXY(0, kLcdTrackTitleY);
            mLcd.clear(mLcd.cursorX, mLcd.cursorY, mLcd.width(), kTrackTitleFont.height);
        }
        return;
    }

    size_t len = strlen(buf);
    if (len > kMaxTrackTitleLen - 3) {
        mLcdTrackTitle.reserve(kMaxTrackTitleLen + 1);
        mLcdTrackTitle.assign(buf, kMaxTrackTitleLen - 3);
        mLcdTrackTitle.append("...", 4);
        mTitleTextWidth = mTitleTextFrameBuf.width();
    }
    else {
        mLcdTrackTitle.reserve(len + 4);
        mLcdTrackTitle.assign(buf, len);
        mLcdTrackTitle.append(" * ", 4);
        mTitleTextWidth = (len + 3) * (kTrackTitleFont.width + kTrackTitleFont.charSpacing);
    }
    mTitleScrollPixOffset = 0;
    mTitleScrollStep = 2;
    mTitleTextFrameBuf.clear();
    mTitleTextFrameBuf.gotoXY(0, 0);
    mTitleTextFrameBuf.puts(mLcdTrackTitle.buf(), mLcd.kFlagNoAutoNewline | mLcd.kFlagAllowPartial);
    mTitleScrollEnabled = true; //!streamIsCpuHeavy();
    lcdBlitTrackTitle();
}
void AudioPlayer::lcdBlitTrackTitle()
{
#ifdef PERF_TITLESCROLL
    ElapsedTimer t;
#endif
    auto wptr = mDmaFrameBuf.data();
    for (int line = 0; line < mTitleTextFrameBuf.height(); line++) {
        auto rLineStart = mTitleTextFrameBuf.data() + line * mTitleTextFrameBuf.width();
        auto rptr =  rLineStart + mTitleScrollPixOffset;
        auto rend = rLineStart + mTitleTextWidth;
        auto wend = wptr + mDmaFrameBuf.width();
//      printf("rptr=%d, rend=%d, wptr=%d, wend=%d\n", rptr - mTitleTextFrameBuf.data(), rend - mTitleTextFrameBuf.data(),
//             wptr - mDmaFrameBuf.data(), wend - mDmaFrameBuf.data());
        while (wptr < wend) {
            int toCopy = std::min(wend - wptr, rend - rptr);
            int cnt = toCopy >> 1; // whole number of 4-byte words
            if (cnt) {
                cnt <<= 2; // to bytes
                memcpy(wptr, rptr, cnt);
                cnt >>= 1; // back to number of colors
                wptr += cnt;
                rptr += cnt;
            }
            if (toCopy & 1) { // odd one
                *(wptr++) = *(rptr++);
            }
            if (rptr >= rend) {
                rptr = rLineStart;
            }
        }
        myassert(wptr == wend);
    }
#ifdef PERF_TITLESCROW
    printf("title draw: %lld us\n", t.usElapsed());
#endif
}
void AudioPlayer::lcdScrollTrackTitle()
{
    if (mLcdTrackTitle.isEmpty()) {
        return;
    }
    mTitleScrollPixOffset += mTitleScrollStep;
    if (mTitleScrollPixOffset >= mTitleTextWidth) {
        mTitleScrollPixOffset -= mTitleTextWidth;
    }
    lcdBlitTrackTitle();
}
void AudioPlayer::lcdWriteStreamInfo(int8_t charOfs, const char* str)
{
    mLcd.setFont(kStreamInfoFont);
    mLcd.setFgColor(kLcdColorStreamInfo);
    uint16_t x = (charOfs >= 0) ? mLcd.textWidth(charOfs) : mLcd.width() - mLcd.textWidth(-charOfs);
    mLcd.gotoXY(x, mLcd.height() - mVuDisplay.height() - kStreamInfoFont.height - 2);
    mLcd.puts(str);
}
void AudioPlayer::onNewStream(StreamFormat fmt, int sourceBps)
{
    mStreamFormat = fmt;
    if (sourceBps < mStreamFormat.bitsPerSample()) {
        mStreamFormat.setBitsPerSample(sourceBps);
    }
    lcdUpdateCodec();
    lcdUpdateAudioFormat();
    mTitleScrollEnabled = mLcdTrackTitle.dataSize() && !streamIsCpuHeavy();
    mBufLowThreshold = fmt.prefillAmount() / 4;
    enum { kDiv = 63 - kBufLowMinGreen };
    mBufLowDisplayGradient = (mBufLowThreshold + kBufLowMinGreen - 1) / kBufLowMinGreen;
}
bool AudioPlayer::streamIsCpuHeavy() const
{
    return mStreamFormat.sampleRate() > 90000 && mStreamFormat.bitsPerSample() >= 24;
}
void AudioPlayer::lcdUpdateCodec()
{
    enum { kMaxLen = 8 };
    char buf[kMaxLen + 1];
    strncpy(buf, mStreamFormat.codec().toString(), kMaxLen);
    buf[kMaxLen] = 0;
    lcdWriteStreamInfo(0, buf);
}
void AudioPlayer::lcdUpdateAudioFormat()
{
    char buf[32];
    auto end = vtsnprintf(buf, sizeof(buf), fmtInt(mStreamFormat.sampleRate() / 1000, 0, 3),
        '.', (mStreamFormat.sampleRate() % 1000 + 50) / 100, "kHz/", mStreamFormat.bitsPerSample(), "-bit");
    lcdWriteStreamInfo(-(end-buf), buf);
}
void AudioPlayer::lcdUpdateNetSpeed()
{
    if (!mStreamIn) {
        return;
    }
    auto input = mStreamIn->inputNodeIntf();
    auto speed = input->pollSpeed();
    uint32_t bufDataSize;

    if (!mDisplayedBufUnderrunTimer) {
        if (speed == mLastShownNetSpeed) {
            return;
        }
        bufDataSize = input->bufferedDataSize();
    }
    else {
        bufDataSize = (--mDisplayedBufUnderrunTimer) ? 0 : input->bufferedDataSize();
    }
    mLastShownNetSpeed = speed;
    lcdRenderNetSpeed(speed, bufDataSize);
}
void AudioPlayer::lcdRenderNetSpeed(uint32_t speed, uint32_t bufDataSize)
{
    char buf[16];
    int whole = speed / 1024;
    int dec = ((speed % 1024) * 10 + 512) / 1024;
    if (dec >= 10) {
        dec = 0;
        whole++;
    }
    auto end = vtsnprintf(buf, sizeof(buf), fmtInt(whole, 0, 4), '.', dec, "K/s");
    LcdColor color;
    //printf("===========buf: '%s', val: %lu\n", buf, speed);
    if (bufDataSize >= mBufLowThreshold) {
        color = kLcdColorNetSpeed_Normal;
    }
    else if (bufDataSize == 0) {
        color = kLcdColorNetSpeed_Underrun;
    }
    else {
        uint8_t green = kBufLowMinGreen + bufDataSize / mBufLowDisplayGradient;
        assert(green < 64);
        //printf("buf: %u, green=%d\n", bufDataSize, green);
        color.rgb(255, green << 2, 128);
    }
    mLcd.setFont(kStreamInfoFont);
    mLcd.setFgColor(color);
    mLcd.gotoXY(mLcd.width() - mLcd.textWidth(end - buf), 0);
    mLcd.puts(buf);
}
void AudioPlayer::lcdShowBufUnderrunImmediate()
{
    mDisplayedBufUnderrunTimer = 1;
    lcdRenderNetSpeed(mLastShownNetSpeed < 0 ? 0 : mLastShownNetSpeed, 0);
}
void AudioPlayer::lcdResetNetSpeedIndication()
{
    mDisplayedBufUnderrunTimer = 0;
    lcdRenderNetSpeed(0, 0xff);
}
void AudioPlayer::audioLevelCb(void* ctx)
{
    // If we are called by the i2s output node, it calls us just before getting the new levels,
    // thus implementing one audio frame delay, which compensates a bit for the actual audio output
    // delay due to DMA buffering
    auto& self = *static_cast<AudioPlayer*>(ctx);
    self.mVuLevels = self.mVuLevelInterface->audioLevels();
    self.mTsLastVuLevel = esp_timer_get_time();
}

const char* AudioPlayer::playerModeToStr(PlayerMode mode) {
    switch(mode) {
        case kModeRadio: return "Radio";
        case kModeDlna: return "DLNA";
        case kModeSpotify: return "Spotify";
        case kModeUrl: return "User URL";
        case kModeBluetoothSink: return "Bluetooth";
        case kModeSpdifIn: return "SPDIF";
//      case kModeAuxIn: return "AUX";
        default: return "";
    }
}
