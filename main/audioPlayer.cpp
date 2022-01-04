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
#include <string>

extern Font font_CamingoBold43;
extern Font font_Camingo22;
extern Font font_Camingo32;
extern Font font_Icons22;

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
    init(inType, outType);
}

AudioPlayer::AudioPlayer(ST7735Display& lcd)
:mFlags((Flags)0), mNvsHandle("aplayer", NVS_READWRITE), mLcd(lcd)
{
    init(AudioNode::kTypeUnknown, AudioNode::kTypeUnknown);
}

void AudioPlayer::init(AudioNode::Type inType, AudioNode::Type outType)
{
    lcdInit();
    mNvsHandle.enableAutoCommit(20000);
    if (inType == AudioNode::kTypeUnknown)
    {
        initFromNvs();
    }
    else
    {
        createPipeline(inType, outType);
    }
    if (inputType() == AudioNode::kTypeHttpIn) {
        stationList.reset(new StationList(mutex));
    }
    initTimedDrawTask();
}
void AudioPlayer::initFromNvs()
{
    uint8_t useEq = mNvsHandle.readDefault<uint8_t>("useEq", 1);
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
    mVuRedStartX = mLcd.width() - kVuLedWidth - 1;
    mVuYellowStartX = mVuRedStartX - kVuLedWidth * 2;
    mVuLeftCtx.barY = mLcd.height() - 2 * kVuLedHeight - kVuLedSpacing;
    mVuRightCtx.barY = mVuLeftCtx.barY + kVuLedHeight + kVuLedSpacing;
}

void AudioPlayer::initTimedDrawTask()
{
    xTaskCreate(&lcdTimedDrawTask, "lcdTask", kLcdTaskStackSize, this, kLcdTaskPrio, nullptr);
    mVolumeInterface->setLevelCallback(audioLevelCb, this);
}

bool AudioPlayer::createPipeline(AudioNode::Type inType, AudioNode::Type outType)
{
    ESP_LOGI(TAG, "Creating audio pipeline");
    AudioNode* pcmSource = nullptr;
    switch(inType) {
    case AudioNode::kTypeHttpIn:
        if (AudioNode::haveSpiRam()) {
            ESP_LOGI(TAG, "Allocating %d bytes in SPIRAM for http buffer", kHttpBufSizeSpiRam);
            mStreamIn.reset(new HttpNode(kHttpBufSizeSpiRam, 32768));
        } else {
            ESP_LOGI(TAG, "Allocating %d bytes internal RAM for http buffer", kHttpBufSizeSpiRam);
            mStreamIn.reset(new HttpNode(kHttpBufSizeInternal, kHttpBufSizeInternal * 3 / 4));
        }
        mStreamIn->setEventHandler(this);
        mStreamIn->subscribeToEvents(0xffff);

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
        auto vuAtInput = mNvsHandle.readDefault<uint8_t>("vuAtEqInput", 0);
        if (vuAtInput) {
            mEqualizer->monitorLevelAtInput(true);
        }
        ESP_LOGI(TAG, "VU source set %s volume and EQ processing", vuAtInput ? "before" : "after");
    }
    switch(outType) {
    case AudioNode::kTypeI2sOut:
        i2s_pin_config_t cfg;
        cfg.ws_io_num = 25;
        cfg.bck_io_num = 26;
        cfg.data_out_num = 27;
        cfg.data_in_num = -1;

        mStreamOut.reset(new I2sOutputNode(0, &cfg, AudioNode::haveSpiRam()));
        if ((mFlags & kFlagUseEqualizer) == 0) {
            static_cast<I2sOutputNode*>(mStreamOut.get())->useVolumeInterface(true);
        }
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
    lcdDrawGui();
    return true;
}

void AudioPlayer::lcdDrawGui()
{
    LOCK_PLAYER();
    mLcd.setBgColor(0, 0, 128);
    mLcd.clear();
    mLcd.setFont(font_Camingo22);
    mLcd.setFgColor(255, 255, 128);
    mLcd.gotoXY(0, 0);
    auto type = mStreamIn->type();
    if (type == AudioNode::kTypeHttpIn) {
        mLcd.puts("Radio");
        if (stationList) {
            lcdUpdateStationInfo();
        }
    } else if (type == AudioNode::kTypeA2dpIn) {
        mLcd.puts("Bluetooth");
    } else {
        mLcd.puts("?");
    }
    mLcd.setFgColor(0, 128, 128);
    mLcd.hLine(0, mLcd.width()-1, mLcd.fontHeight() + 3);
}
void AudioPlayer::lcdUpdateStationInfo()
{
    LOCK_PLAYER();
    assert(mStreamIn && mStreamIn->type() == AudioNode::kTypeHttpIn);
    auto& station = stationList->currStation;
    if (!station.isValid()) {
        return;
    }
// station name
    mLcd.setFont(font_Camingo32);
    mLcd.setFgColor(255, 255, 128);
    mLcd.clear(0, mLcd.fontHeight() + 6, mLcd.width(), mLcd.fontHeight());
    mLcd.gotoXY(0, mLcd.fontHeight() + 6);
    mLcd.putsCentered(station.name());
// station flags
    mLcd.setFont(font_Icons22); //TODO: Use pictogram font
    mLcd.cursorY = 0;
    mLcd.cursorX = (mLcd.width() - mLcd.charWidth(kSymFavorite)) / 2;
    if (station.flags() & Station::kFlagFavorite) {
        mLcd.setFgColor(255, 0, 0);
        mLcd.putc(kSymFavorite);
    } else {
        mLcd.putc(kSymBlank);
    }

    lcdUpdateRecIcon();
}
void AudioPlayer::lcdUpdateRecIcon()
{
    LOCK_PLAYER();
    mLcd.setFont(font_Icons22);
    mLcd.setFgColor(200, 0, 0);
    char sym;
    do {
        if (!stationList) {
            sym = kSymBlank;
            break;
        }
        auto& station = stationList->currStation;
        if (!station.isValid()) {
            sym = kSymBlank;
            break;
        }
        bool recEnabled = station.flags() & Station::kFlagRecord;
        if (recEnabled) {
            auto& httpNode = *static_cast<HttpNode*>(mStreamIn.get());
            sym = httpNode.recordingIsActive() ? kSymRecording : kSymRecEnabled;
        } else {
            sym = kSymBlank;
        }
    } while(false);
    mLcd.gotoXY(mLcd.width() - mLcd.charWidth(kSymPlaying) - mLcd.charWidth(sym) - 4, 0);
    mLcd.putc(sym);
}

void AudioPlayer::lcdUpdatePlayState(char sym)
{
    LOCK_PLAYER();
    mLcd.setFont(font_Icons22);
    mLcd.setFgColor(255, 255, 128);
    mLcd.gotoXY(mLcd.width() - mLcd.charWidth(sym) - 1, 0);
    mLcd.putc(sym);
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

bool AudioPlayer::playUrl(const char* url, const char* record)
{
    LOCK_PLAYER();
    if (!mStreamIn || mStreamIn->type() != AudioNode::kTypeHttpIn) {
        return false;
    }
    auto& http = *static_cast<HttpNode*>(mStreamIn.get());
    http.setUrl(url, record);
    if (isStopped()) {
        play();
    }
    return true;
}
esp_err_t AudioPlayer::playStation(const char* id)
{
    LOCK_PLAYER();
    if (!this->stationList) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!id) {
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
    if (!playUrl(station.url(), (station.flags() & station.kFlagRecord) ? station.id() : nullptr)) {
        return ESP_ERR_NOT_SUPPORTED; // stream source node is not http client
    }
    lcdUpdateStationInfo();
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
    LOCK_PLAYER();
    mStreamIn->run();
    mStreamOut->run();
    lcdUpdatePlayState(kSymPlaying);
}

void AudioPlayer::pause()
{
    LOCK_PLAYER();
    mStreamIn->pause();
    mStreamOut->pause();
    mStreamIn->waitForState(AudioNodeWithTask::kStatePaused);
    mStreamOut->waitForState(AudioNodeWithTask::kStatePaused);
    lcdUpdatePlayState(kSymPaused);
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
   mTitleScrollEnabled = false;
   if (mVolumeInterface) {
       mVolumeInterface->clearAudioLevels();
   }
   lcdUpdatePlayState(kSymStopped);
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
    UrlParams params(req);
    DynBuffer buf(128);
    auto param = params.strVal("url");
    if (param) {
        self->playUrl(param.str);
        buf.printf("Playing url '%s'", param.str);
    }
    else {
        auto err = self->playStation(params.strVal("sta").str);
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
    auto type = params.strVal("type");
    if (!type.str) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No 'type' param specified");
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
    DynBuffer buf(128);
    MutexLocker locker(self->mutex);
    httpd_resp_send_chunk(req, "{", 1);
    bool isFirst = true;
    nvs_iterator_t it;
    for(it = nvs_entry_find("nvs", ns, NVS_TYPE_ANY); it; it = nvs_entry_next(it)) {
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
        httpd_resp_send_chunk(req, buf.buf(), buf.dataSize());
    }
    nvs_release_iterator(it);
    httpd_resp_send_chunk(req, "}", 1);
    httpd_resp_send_chunk(req, nullptr, 0);
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
    registerHttpGetHandler(server, "/nvget", &nvsGetParamUrlHandler);
    registerHttpGetHandler(server, "/nvset", &nvsSetParamUrlHandler);
    if (stationList) {
        stationList->registerHttpHandlers(server);
    }
}

bool AudioPlayer::onEvent(AudioNode *self, uint32_t event, uintptr_t buf, size_t bufSize)
{
    if (self->type() != AudioNode::kTypeHttpIn) {
        return true;
    }
    // We are in the http node's thread, must not do any locking from here, so we call
    // into the player via async messages
    if (event == HttpNode::kEventTrackInfo) {
        asyncCall([this, title = std::string((const char*)buf)]() {
            lcdUpdateTrackTitle(title.c_str());
        });
    } else {
        asyncCall([this, event]() {
            if (event == HttpNode::kEventConnected) {
                lcdUpdatePlayState(kSymPlaying);
            } else if (event == HttpNode::kEventConnecting) {
                lcdUpdatePlayState(kSymConnecting);
            } else if (event == HttpNode::kEventRecording) {
                lcdUpdateRecIcon();
            }
        });
    }
    return true;
}

void AudioPlayer::lcdTimedDrawTask(void* ctx)
{
    auto& self = *static_cast<AudioPlayer*>(ctx);
    enum { kTitleScrollTickPeriodUs = kTitleScrollTickPeriodMs * 1000 };
    int64_t tsLastTitleScroll = esp_timer_get_time() - kTitleScrollTickPeriodUs - 1;
    for (;;) {
        EventBits_t events;
        if (self.mTitleScrollEnabled) {
            int timeout = kTitleScrollTickPeriodUs - (esp_timer_get_time() - tsLastTitleScroll);
            events = (timeout > 0)
                ? self.mEvents.waitForOneAndReset(kEventTerminating|kEventVolLevel, timeout)
                : (EventBits_t)0;
        } else {
            events = self.mEvents.waitForOneAndReset(kEventTerminating|kEventVolLevel, -1);
        }
        if (events & kEventTerminating) {
            break;
        }
        {
            MutexLocker locker(self.mutex);
            if (events & kEventVolLevel) {
                self.lcdUpdateVolLevel();
            } else if (events == 0) {
                tsLastTitleScroll = esp_timer_get_time();
                self.lcdScrollTrackTitle();
            }
        }
    }
    self.mEvents.setBits(kEventTerminated);
    vTaskDelete(nullptr);
}

void AudioPlayer::lcdUpdateTrackTitle(const char* buf)
{
    LOCK_PLAYER();
    auto len = strlen(buf);
    if (!len) {
        mTitleScrollEnabled = false;
        lcdSetupForTrackTitle();
        mLcd.clear(mLcd.cursorX, mLcd.cursorY, mLcd.width(), mLcd.fontHeight());
        return;
    }
    mTrackTitle.reserve(len + 4);
    mTrackTitle.assign(buf, len);
    mTrackTitle.append(" * ", 4);
    mTitleScrollCharOffset = mTitleScrollPixOffset = 0;
    mTitleScrollEnabled = true;
}

void AudioPlayer::lcdSetupForTrackTitle()
{
    mLcd.setFont(font_CamingoBold43);
    mLcd.setFgColor(255, 255, 128);
    mLcd.gotoXY(0, (mLcd.height() - mLcd.charHeight()) / 2);
}

void AudioPlayer::lcdScrollTrackTitle(int step)
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
        // display first partial char, if any
        if (mTitleScrollPixOffset) {
            mLcd.putc(*(title++), mLcd.kFlagNoAutoNewline, mTitleScrollPixOffset); // can advance to the terminating NULL
        }
        mTitleScrollPixOffset += step;
        if (mTitleScrollPixOffset >= mLcd.font()->width + mLcd.font()->charSpacing) {
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
        return (level >= std::numeric_limits<decltype(level)>::max()-1)
            ? ST77XX_RED
            : ST77XX_YELLOW;
    } else if (ledX >= mVuYellowStartX) {
        return ST77XX_YELLOW;
    } else {
        return ST77XX_GREEN;
    }
}

void AudioPlayer::vuCalculateLevels(VuLevelCtx& ctx, int16_t level)
{
    if (level > ctx.avgLevel) {
        ctx.avgLevel = level;
    } else {
        ctx.avgLevel = ((int)ctx.avgLevel * (kVuLevelSmoothFactor-1) + level) / kVuLevelSmoothFactor;
    }
    if (level > ctx.peakLevel) {
        ctx.peakLevel = level;
        ctx.peakTimer = kVuPeakHoldTime;
    } else {
        if (--ctx.peakTimer <= 0) {
            ctx.peakTimer = kVuPeakDropTime;
            if (ctx.peakLevel > 0) {
                ctx.peakLevel -= mLevelPerVuLed;
            }
        }
    }
}
void AudioPlayer::lcdUpdateVolLevel()
{
    // Called from the display refresh worker
    const auto& levels = mVolumeInterface->audioLevels();
    vuDrawChannel(mVuLeftCtx, levels.left);
    vuDrawChannel(mVuRightCtx, levels.right);
}

void AudioPlayer::vuDrawChannel(VuLevelCtx& ctx, int16_t level)
{
    vuCalculateLevels(ctx, level);
    //width in pixels of level bars
    int16_t levelBarWidth = kVuLedWidth * (((int)ctx.avgLevel + mLevelPerVuLed - 1) / mLevelPerVuLed);
    // draw bar
    for (int16_t x = 0; x < levelBarWidth; x += kVuLedWidth) {
        mLcd.setFgColor(vuLedColor(x, ctx.peakLevel));
        mLcd.fillRect(x, ctx.barY, kVuLedWidth-kVuLedSpacing, kVuLedHeight);
    }

    // draw peak indicators and background bafore and after them
    int16_t x = kVuLedWidth * ((ctx.peakLevel + mLevelPerVuLed - 1) / mLevelPerVuLed - 1);
    if (x < levelBarWidth) { // no peak led after level bar
        int16_t afterWidth = mLcd.width() - levelBarWidth;
        if (afterWidth > 0) {
            // draw background after level bar and return
            mLcd.clear(levelBarWidth, ctx.barY, afterWidth, kVuLedHeight);
        }
        return;
    }
    if (x > levelBarWidth) {
        // draw background between end of level bar and peak led
        mLcd.clear(levelBarWidth, ctx.barY, x - levelBarWidth, kVuLedHeight);
    }
    mLcd.setFgColor(vuLedColor(x, ctx.peakLevel));
    // draw peak led
    mLcd.fillRect(x, ctx.barY, kVuLedWidth - kVuLedSpacing, kVuLedHeight);
    // draw background after peak led
    int16_t xEnd = x + kVuLedWidth - kVuLedSpacing;
    int16_t afterBg = mLcd.width() - xEnd;
    if (afterBg > 0) {
        mLcd.clear(x + kVuLedWidth, ctx.barY, afterBg, kVuLedHeight);
    }
}
