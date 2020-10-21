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
#include <stdfonts.hpp>

#define LOCK_PLAYER() MutexLocker locker(mutex)

const float AudioPlayer::sDefaultEqGains[EqualizerNode::kBandCount] = {
    8, 8, 4, 0, -2, -4, -4, -2, 4, 6
};

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

AudioPlayer::AudioPlayer(AudioNode::Type inType, AudioNode::Type outType, ST7735Display& lcd, bool useEq)
:mFlags(useEq ? kFlagUseEqualizer : (Flags)0),
 mNvsHandle("aplayer", NVS_READWRITE), mLcd(lcd), mEvents(kEventTerminating)
{
    lcdInit();
    mNvsHandle.enableAutoCommit(20000);
    createPipeline(inType, outType);
    initTimedDrawTask();
}

AudioPlayer::AudioPlayer(ST7735Display& lcd)
:mFlags((Flags)0), mNvsHandle("aplayer", NVS_READWRITE), mLcd(lcd)
{
    lcdInit();
    mNvsHandle.enableAutoCommit(20000);
    initFromNvs();
    initTimedDrawTask();
}

void AudioPlayer::initFromNvs()
{
    uint8_t useEq = mNvsHandle.readDefault("useEq", 1);
    if (useEq) {
        mFlags = (Flags)(mFlags | kFlagUseEqualizer);
    }
    AudioNode::Type inType = (AudioNode::Type)mNvsHandle.readDefault<uint8_t>("inType", AudioNode::kTypeHttpIn);
    bool ok = createPipeline(inType, AudioNode::kTypeI2sOut);
    if (!ok) {
        ESP_LOGE(TAG, "Failed to create audio pipeline");
        myassert(false);
    }
}

void AudioPlayer::lcdInit()
{
    auto numLeds = mLcd.width() / kVuLedWidth;
    mLevelPerVuLed = (std::numeric_limits<int16_t>::max() + numLeds - 1) / numLeds;
    mVuRedStartX = mLcd.width() - kVuLedWidth - 2;
    mVuYellowStartX = mVuRedStartX - kVuLedWidth * 2;

    mLcd.setBgColor(0, 0, 128);
    mLcd.clear();
    mLcd.setFgColor(0, 128, 128);
    mLcd.hLine(0, 127, 15);
}

void AudioPlayer::initTimedDrawTask()
{
    xTaskCreate(&lcdTimedDrawTask, "lcdTask", 1200, this, 20, nullptr);
    mVolumeInterface->setLevelCallback(audioLevelCb, this);
}

bool AudioPlayer::createPipeline(AudioNode::Type inType, AudioNode::Type outType)
{
    ESP_LOGI(TAG, "Creating audio pipeline");
    AudioNode* pcmSource = nullptr;
    switch(inType) {
    case AudioNode::kTypeHttpIn:
        mStreamIn.reset(new HttpNode(kHttpBufSize));
        mStreamIn->subscribeToEvents(HttpNode::kEventTrackInfo | HttpNode::kEventConnecting | HttpNode::kEventConnected);
        mStreamIn->setEventHandler(this);

        mDecoder.reset(new DecoderNode);
        mDecoder->linkToPrev(mStreamIn.get());
        pcmSource = mDecoder.get();
        break;
    case AudioNode::kTypeA2dpIn:
        mStreamIn.reset(new A2dpInputNode("NetPlayer"));
        mDecoder.reset();
        pcmSource = mStreamIn.get();
        break;
    default:
        ESP_LOGE(TAG, "Unknown pipeline input node type %d", inType);
        return false;
    }
    if (mFlags & kFlagUseEqualizer) {
        mEqualizer.reset(new EqualizerNode(sDefaultEqGains));
        mEqualizer->linkToPrev(pcmSource);
        pcmSource = mEqualizer.get();
    }
    switch(outType) {
    case AudioNode::kTypeI2sOut:
        i2s_pin_config_t cfg;
        cfg.ws_io_num = 25;
        cfg.bck_io_num = 26;
        cfg.data_out_num = 27;
        cfg.data_in_num = -1;

        mStreamOut.reset(new I2sOutputNode(0, &cfg));
        break;
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
    detectVolumeNode();
    ESP_LOGI(TAG, "Audio pipeline:\n%s", printPipeline().c_str());
    loadSettings();
    lcdUpdateModeInfo();
    return true;
}

void AudioPlayer::lcdUpdateModeInfo()
{
    mLcd.setFont(Font_5x7, 2);
    mLcd.gotoXY(0, 0);
    mLcd.setFgColor(255, 255, 128);
    auto type = mStreamIn->type();
    if (type == AudioNode::kTypeHttpIn) {
        mLcd.putc('N');
    } else if (type == AudioNode::kTypeA2dpIn) {
        mLcd.putc('B');
    } else {
        mLcd.putc('?');
    }
    lcdUpdatePlayState();
}

void AudioPlayer::lcdUpdatePlayState()
{
    mLcd.setFont(Font_5x7, 2);
    mLcd.setFgColor(255, 255, 128);
    mLcd.gotoXY(mLcd.charWidth() + 1, 0);
    if (isPlaying()) {
        mLcd.putc('>');
    } else if (isStopped()) {
        mLcd.putc('O');
    } else {
        mLcd.putc('\"');
    }
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

void AudioPlayer::changeInput(AudioNode::Type inType)
{
    mNvsHandle.write("inType", (uint8_t)inType);
    if (mStreamIn && inType == mStreamIn->type()) {
        ESP_LOGW(TAG, "AudioPlayer::changeInput: Input type is already %d", inType);
        return;
    }
    destroyPipeline();
    mNvsHandle.write("inType", (uint8_t)inType);
    initFromNvs();
}

void AudioPlayer::loadSettings()
{
    if (mVolumeInterface) {
        mVolumeInterface->setVolume(
            mNvsHandle.readDefault<uint16_t>("volume", 50));
    }
    if (mEqualizer) {
        int8_t gains[10];
        size_t len = sizeof(gains);
        if (mNvsHandle.readBlob("eqGains", gains, len) == ESP_OK
          && len == sizeof(gains)) {
            ESP_LOGI(TAG, "Loaded equalizer gains from NVS:");
            for (int i = 0; i < 10; i++) {
                ESP_LOGI("band", "%d Hz -> %.1f", mEqualizer->bandFreqs[i], (float)gains[i] / kEqGainPrecisionDiv);
                mEqualizer->setBandGain(i, (float)gains[i] / kEqGainPrecisionDiv);
            }
        }
    }
}

void AudioPlayer::detectVolumeNode() {
    std::vector<AudioNode*> nodes(10);
    for (AudioNode* node = mStreamOut.get(); node; node = node->prev()) {
        nodes.push_back(node);
    }
    for (auto it = nodes.rbegin(); it != nodes.rend(); it++) {
        mVolumeInterface = (*it)->volumeInterface();
        if (mVolumeInterface) {
            ESP_LOGW(TAG, "Volume node found: '%s'", (*it)->tag());
            return;
        }
    }
    ESP_LOGE(TAG, "No node with volume interface found, volume control will be unavailable");
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

void AudioPlayer::playUrl(const char* url, const char* record)
{
    LOCK_PLAYER();
    assert(mStreamIn && mStreamIn->type() == AudioNode::kTypeHttpIn);
    auto& http = *static_cast<HttpNode*>(mStreamIn.get());
    http.setUrl(url);
    if (record) {
        http.startRecording(record);
    }
    if (isStopped()) {
        play();
    }
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
    LOCK_PLAYER();
    mStreamIn->run();
    mStreamOut->run();
    lcdUpdatePlayState();
}

void AudioPlayer::pause()
{
    LOCK_PLAYER();
    mStreamIn->pause();
    mStreamOut->pause();
    mStreamIn->waitForState(AudioNodeWithTask::kStatePaused);
    mStreamOut->waitForState(AudioNodeWithTask::kStatePaused);
    lcdUpdatePlayState();
}

void AudioPlayer::resume()
{
    play();
}

void AudioPlayer::stop()
{
   LOCK_PLAYER();
   mStreamIn->stop(false);
   mStreamOut->stop(false);
   mStreamIn->waitForStop();
   mStreamOut->waitForStop();
   mTitleScrollTimer.cancel();
   if (mVolumeInterface) {
       mVolumeInterface->clearAudioLevels();
   }
   lcdUpdatePlayState();
}

bool AudioPlayer::volumeSet(uint16_t vol)
{
    LOCK_PLAYER();
    if (!mVolumeInterface) {
        return false;
    }
    mVolumeInterface->setVolume(vol);
    mNvsHandle.write("volume", vol);
    return true;
}

int AudioPlayer::volumeGet()
{
    LOCK_PLAYER();
    if (mVolumeInterface) {
        return mVolumeInterface->getVolume();
    }
    return -1;
}

uint16_t AudioPlayer::volumeChange(int step)
{
    LOCK_PLAYER();
    auto currVol = volumeGet();
    if (currVol < 0) {
        return currVol;
    }
    double newVol = currVol + step;
    if (newVol < 0) {
        newVol = 0;
    } else if (newVol > 255) {
        newVol = 255;
    }
    if (fabs(newVol - currVol) > 0.01) {
        if (!volumeSet(newVol)) {
            return -1;
        }
    }
    return newVol;
}

bool AudioPlayer::equalizerSetBand(int band, float dbGain)
{
    LOCK_PLAYER();
    if (!mEqualizer) {
        return false;
    }
    equalizerDoSetBandGain(band, dbGain);
    equalizerSaveGains();
    return true;
}

float AudioPlayer::equalizerDoSetBandGain(int band, float dbGain)
{
    if (dbGain < -40) {
        dbGain = -40;
    } else if (dbGain > 40) {
        dbGain = 40;
    }
    dbGain = roundf(dbGain * kEqGainPrecisionDiv) / kEqGainPrecisionDiv; // encode to int and decode back
    mEqualizer->setBandGain(band, dbGain);
    return dbGain;
}

bool AudioPlayer::equalizerSetGainsBulk(char* str, size_t len)
{
    LOCK_PLAYER();
    if (!str) {
        mEqualizer->zeroAllGains();
        equalizerSaveGains();
        return true;
    }
    KeyValParser vals(str, len);
    vals.parse(';', '=', KeyValParser::kTrimSpaces);
    bool ok = true;
    for (const auto& kv: vals.keyVals()) {
        int band = kv.key.toInt(-1);
        if (band < 0 || band > 9) {
            ESP_LOGW(TAG, "Invalid band %d", band);
            ok = false;
            continue;
        }
        auto gain = kv.val.toFloat(INFINITY);
        if (gain == INFINITY) {
            ESP_LOGW(TAG, "Invalid gain '%s'", kv.val.str);
            ok = false;
            continue;
        }
        ESP_LOGI(TAG, "Setting band %d gain %.1f", band, gain);
        equalizerDoSetBandGain(band, gain);
    }
    equalizerSaveGains();
    return ok;
}

const float* AudioPlayer::equalizerGains()
{
    if (!mEqualizer) {
        return nullptr;
    }
    return mEqualizer->allGains();
}

void AudioPlayer::equalizerSaveGains()
{
    if (!mEqualizer) {
        return;
    }
    auto fGains = mEqualizer->allGains();
    int8_t gains[10];
    for (int i = 0; i < 10; i++) {
        gains[i] = roundf(fGains[i] * kEqGainPrecisionDiv);
    }
    mNvsHandle.writeBlob("eqGains", gains, sizeof(gains));
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
    auto urlParam = params.strVal("url");
    const char* url;
    if (!urlParam) {
        url = self->playlist.getNextTrack();
        if (!url) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Playlist is empty");
            return ESP_OK;
        }
    } else {
        url = urlParam.str;
    }
    self->playUrl(url);
    DynBuffer buf(128);
    buf.printf("Changing stream url to '%s'", url);
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

void AudioPlayer::registerHttpGetHandler(httpd_handle_t server,
    const char* path, esp_err_t(*handler)(httpd_req_t*))
{
    httpd_uri_t desc = {
        .uri       = path,
        .method    = HTTP_GET,
        .handler   = handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server, &desc);
}

esp_err_t AudioPlayer::volumeUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
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

esp_err_t AudioPlayer::equalizerSetUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    UrlParams params(req);
    auto data = params.strVal("vals");
    if (data.str) {
        self->equalizerSetGainsBulk(data.str, data.len);
        httpd_resp_sendstr(req, "ok");
        return ESP_OK;
    }
    if (params.strVal("reset").str) {
        self->equalizerSetGainsBulk(nullptr, 0);
        httpd_resp_sendstr(req, "ok");
        return ESP_OK;
    }
    auto band = params.intVal("band", -1);
    if (band < 0 || band > 9) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'band' parameter");
        return ESP_OK;
    }
    auto level = params.floatVal("level", INFINITY);
    if (level == INFINITY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'level' parameter");
        return ESP_OK;
    }
    if (self->equalizerSetBand(band, level)) {
        httpd_resp_sendstr(req, "ok");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed setting equalizer band");
    }
    return ESP_OK;
}

esp_err_t AudioPlayer::equalizerDumpUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
    auto levels = self->equalizerGains();
    DynBuffer buf(240);
    buf.printf("[");
    for (int i = 0; i < 10; i++) {
        buf.printf("[%d,%.1f],", self->mEqualizer->bandFreqs[i], levels[i]);
    }
    buf[buf.dataSize()-2] = ']';
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
            auto& icy = http->icyInfo;
            MutexLocker locker(icy.mutex);
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

void AudioPlayer::registerUrlHanlers(httpd_handle_t server)
{
    registerHttpGetHandler(server, "/play", &playUrlHandler);
    registerHttpGetHandler(server, "/pause", &pauseUrlHandler);
    registerHttpGetHandler(server, "/vol", &volumeUrlHandler);
    registerHttpGetHandler(server, "/eqget", &equalizerDumpUrlHandler);
    registerHttpGetHandler(server, "/eqset", &equalizerSetUrlHandler);
    registerHttpGetHandler(server, "/status", &getStatusUrlHandler);
}

bool AudioPlayer::onEvent(AudioNode *self, uint32_t event, void *buf, size_t bufSize)
{
    if (self->type() == AudioNode::kTypeHttpIn) {
        if (event == HttpNode::kEventTrackInfo) {
            lcdUpdateTrackTitle((const char*)buf, bufSize);
        } else if (event == HttpNode::kEventConnected) {
            lcdUpdateStationInfo();
        }
    }
    return true;
}

void AudioPlayer::lcdTimedDrawTask(void* ctx)
{
    auto& self = *static_cast<AudioPlayer*>(ctx);
    for (;;) {
        auto events = self.mEvents.waitForOneAndReset(kEventTerminating|kEventScroll|kEventVolLevel, -1);
        if (events & kEventTerminating) {
            break;
        }
        {
            MutexLocker locker(self.mutex);
            if (events & kEventVolLevel) {
                self.lcdUpdateVolLevel();
            }
            if (events & kEventScroll) {
                self.lcdScrollTrackTitle();
            }
        }
    }
    self.mEvents.setBits(kEventTerminated);
    vTaskDelete(nullptr);
}

void AudioPlayer::lcdUpdateTrackTitle(const char* buf, int size)
{
    LOCK_PLAYER();
    if (size <= 1) {
        mTitleScrollTimer.cancel();
        lcdSetupForTrackTitle();
        mLcd.puts("----------------", mLcd.kFlagNoAutoNewline|mLcd.kFlagAllowPartial);
        return;
    }
    mTrackTitle.reserve(size + 3);
    mTrackTitle.assign(buf, size - 1);
    mTrackTitle.append(" * ", 4);
    mTitleScrollCharOffset = mTitleScrollPixOffset = 0;
    titleSrollTickCb(this);
    if (!mTitleScrollTimer.running()) {
        mTitleScrollTimer.start(kTitleScrollTickPeriodMs, false, titleSrollTickCb, this);
    }
}

void AudioPlayer::lcdSetupForTrackTitle()
{
    mLcd.setFont(Font_5x7, 2);
    mLcd.setFgColor(255, 255, 128);
    mLcd.gotoXY(0, (mLcd.height() - mLcd.charHeight()) / 2);
}

void AudioPlayer::titleSrollTickCb(void* ctx)
{
    auto& self = *static_cast<AudioPlayer*>(ctx);
    self.mEvents.setBits(kEventScroll);
}

void AudioPlayer::lcdScrollTrackTitle()
{
    if (mTrackTitle.dataSize() <= 1) {
        return;
    }
    lcdSetupForTrackTitle();

    auto title = mTrackTitle.buf() + mTitleScrollCharOffset;
    auto titleEnd = mTrackTitle.buf() + mTrackTitle.dataSize() - 1; // without terminating null
    if (title >= titleEnd) {
        title = mTrackTitle.buf();
        mTitleScrollCharOffset = mTitleScrollPixOffset = 0;
    } else {
        if (mTitleScrollPixOffset) {
            mLcd.putc(*(title++), mLcd.kFlagNoAutoNewline, mTitleScrollPixOffset); // can advance to the terminating NULL
        }
        if (++mTitleScrollPixOffset >= mLcd.font()->width + mLcd.font()->charSpacing) {
            mTitleScrollPixOffset = 0;
            mTitleScrollCharOffset++;
        }
    }
    for(;;) {
        char ch = *title;
        if (!ch) {
            title = mTrackTitle.buf();
            ch = *title;
        }
        title++;
        if (!mLcd.putc(ch, mLcd.kFlagNoAutoNewline | mLcd.kFlagAllowPartial)) {
            break;
        }
    }
}

void AudioPlayer::audioLevelCb(void* ctx)
{
    auto& self = *static_cast<AudioPlayer*>(ctx);
    self.mEvents.setBits(kEventVolLevel);
}
inline uint16_t AudioPlayer::vuLedColor(int16_t ledX, int16_t level)
{
    if (ledX >= mVuRedStartX) {
        return (level == std::numeric_limits<decltype(level)>::max())
            ? ST77XX_RED
            : ST77XX_YELLOW;
    } else if (ledX >= mVuYellowStartX) {
        return ST77XX_YELLOW;
    } else {
        return ST77XX_GREEN;
    }
}
void AudioPlayer::lcdUpdateVolLevel()
{
    // Called from the display refresh worker
    const auto& levels = mVolumeInterface->audioLevels();
    auto yLeft = mLcd.height() - 2 * kVuLedHeight - 2;
    auto yRight = yLeft + kVuLedHeight + 2;

    if (levels.left > mVuLeftAvg) {
        mVuLeftAvg = levels.left;
    } else {
        mVuLeftAvg = ((int)mVuLeftAvg * (kVuLevelSmoothFactor-1) + levels.left) / kVuLevelSmoothFactor;
    }
    if (levels.left > mVuPeakLeft) {
        mVuPeakLeft = levels.left;
        mVuPeakTimerLeft = kVuPeakHoldTime;
    } else {
        if (--mVuPeakTimerLeft <= 0) {
            mVuPeakTimerLeft = kVuPeakDropTime;
            if (mVuPeakLeft > 0) {
                mVuPeakLeft -= mLevelPerVuLed;
            }
        }
    }
    if (levels.right > mVuRightAvg) {
        mVuRightAvg = levels.right;
    } else {
        mVuRightAvg = ((int)mVuRightAvg * (kVuLevelSmoothFactor-1) + levels.right) / kVuLevelSmoothFactor;
    }
    if (levels.right > mVuPeakRight) {
        mVuPeakRight = levels.right;
        mVuPeakTimerRight = kVuPeakHoldTime;
    } else {
        if (--mVuPeakTimerRight <= 0) {
            mVuPeakTimerRight = kVuPeakDropTime;
            if (mVuPeakRight > 0) {
                mVuPeakRight -= mLevelPerVuLed;
            }
        }
    }

    int16_t endXLeft = kVuLedWidth * (((int)mVuLeftAvg + mLevelPerVuLed - 1) / mLevelPerVuLed);
    int16_t endXRight = kVuLedWidth * (((int)mVuRightAvg + mLevelPerVuLed - 1) / mLevelPerVuLed);
    int16_t endX = std::max(endXLeft, endXRight);
    for (int16_t x = 0; x < endX; x += kVuLedWidth) {
        if (x < endXLeft) {
            mLcd.setFgColor(vuLedColor(x, mVuPeakLeft));
            mLcd.fillRect(x, yLeft, kVuLedWidth-1, kVuLedHeight);
        } else {
            mLcd.clear(x, yLeft, kVuLedWidth-1, kVuLedHeight);
        }
        if (x < endXRight) {
            mLcd.setFgColor(vuLedColor(x, mVuPeakRight));
            mLcd.fillRect(x, yRight, kVuLedWidth-1, kVuLedHeight);
        } else {
            mLcd.clear(x, yRight, kVuLedWidth-1, kVuLedHeight);
        }
    }
    int16_t afterEndX = mLcd.width() - endX;
    int16_t leftPeakX = kVuLedWidth * (((int)mVuPeakLeft + mLevelPerVuLed - 1) / mLevelPerVuLed - 1);
    if (leftPeakX >= endXLeft) {
        if (leftPeakX > endXLeft) {
            mLcd.clear(endXLeft, yLeft, leftPeakX - endXLeft, kVuLedHeight);
        }
        mLcd.setFgColor(vuLedColor(leftPeakX, mVuPeakLeft));
        mLcd.fillRect(leftPeakX, yLeft, kVuLedWidth-1, kVuLedHeight);
        int afterBg = mLcd.width() - leftPeakX - kVuLedWidth;
        if (afterBg > 0) {
            mLcd.clear(leftPeakX + kVuLedWidth, yLeft, afterBg, kVuLedHeight);
        }
    } else if (afterEndX) {
        mLcd.clear(endX, yLeft, afterEndX, kVuLedHeight);
    }

    int16_t rightPeakX = kVuLedWidth * (((int)mVuPeakRight + mLevelPerVuLed - 1) / mLevelPerVuLed - 1);
    if (rightPeakX >= endXRight) {
        if (rightPeakX > endXRight) {
            mLcd.clear(endXRight, yRight, rightPeakX - endXRight, kVuLedHeight);
        }
        mLcd.setFgColor(vuLedColor(rightPeakX, mVuPeakRight));
        mLcd.fillRect(rightPeakX, yRight, kVuLedWidth-1, kVuLedHeight);
        int afterBg = mLcd.width() - rightPeakX - kVuLedWidth;
        if (afterBg > 0) {
            mLcd.clear(rightPeakX + kVuLedWidth, yRight, afterBg, kVuLedHeight);
        }
    } else if (afterEndX) {
        mLcd.clear(endX, yRight, afterEndX, kVuLedHeight);
    }
}

void AudioPlayer::lcdUpdateStationInfo()
{
    LOCK_PLAYER();
    myassert(mStreamIn->type() == AudioNode::kTypeHttpIn);
    auto& icy = static_cast<HttpNode*>(mStreamIn.get())->icyInfo;
    MutexLocker locker1(icy.mutex);
    const char* name = icy.staName();
    if (!name) {
        name = icy.staUrl();
    }
    mLcd.clear(0, 20, mLcd.width()-1, mLcd.charHeight() * 3);
    mLcd.gotoXY(0, 20);
    mLcd.setFont(Font_5x7, 1);
    mLcd.setFgColor(255, 255, 128);
    mLcd.nputs(name, 3 * mLcd.charsPerLine());
}
