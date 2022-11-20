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
#include <esp_netif.h> // for createDlnaHandler()

extern Font font_CamingoBold43;
extern Font font_Camingo22;
extern Font font_Camingo32;
extern Font font_Icons22;

#define LOCK_PLAYER() MutexLocker locker(mutex)

static constexpr const char* const TAG = "AudioPlayer";

const float AudioPlayer::sDefaultEqGains[EqualizerNode::kBandCount] = {
    8, 8, 4, 0, -2, -4, -4, -2, 4, 6
};

void AudioPlayer::setLogLevel(esp_log_level_t level)
{
    esp_log_level_set(TAG, level);
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

AudioPlayer::AudioPlayer(AudioNode::Type inType, AudioNode::Type outType, ST7735Display& lcd, HttpServerInfo& httpServer, bool useEq)
: mFlags(useEq ? kFlagUseEqualizer : (Flags)0), mNvsHandle("aplayer", NVS_READWRITE), mLcd(lcd),
  mEvents(kEventTerminating), mHttpServer(httpServer), mVuDisplay(mLcd)
{
    init(inType, outType);
}

AudioPlayer::AudioPlayer(ST7735Display& lcd, HttpServerInfo& httpServer)
: mFlags((Flags)0), mNvsHandle("aplayer", NVS_READWRITE), mLcd(lcd),
  mHttpServer(httpServer), mVuDisplay(mLcd)
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
    initTimedDrawTask();
    if (inputType() == AudioNode::kTypeHttpIn) {
        stationList.reset(new StationList(mutex));
        createDlnaHandler();
        mDlna->start();
    }
    registerUrlHanlers();
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
void AudioPlayer::createDlnaHandler()
{
    tcpip_adapter_ip_info_t ipInfo;
    char url[48];
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);
    snprintf(url, sizeof(url), "http%s://" IPSTR ":%d", mHttpServer.isSsl ? "s":"", IP2STR(&ipInfo.ip), 80);
    mDlna.reset(new DlnaHandler(mHttpServer.server, url, *this));
}
void AudioPlayer::lcdInit()
{
    mVuDisplay.init(mNvsHandle);
}

void AudioPlayer::initTimedDrawTask()
{
    xTaskCreatePinnedToCore(&lcdTimedDrawTask, "lcdTask", kLcdTaskStackSize, this, kLcdTaskPrio, nullptr, kLcdTaskCore);
    mVolumeInterface->setLevelCallback(audioLevelCb, this);
}

bool AudioPlayer::createPipeline(AudioNode::Type inType, AudioNode::Type outType)
{
    ESP_LOGI(TAG, "Creating audio pipeline");
    AudioNode* pcmSource = nullptr;
    switch(inType) {
    case AudioNode::kTypeHttpIn: {
        HttpNode* http;
        if (utils::haveSpiRam()) {
            ESP_LOGI(TAG, "Allocating %d bytes in SPIRAM for http buffer", kHttpBufSizeSpiRam);
            http = new HttpNode(*this, kHttpBufSizeSpiRam, 32768);
        } else {
            ESP_LOGI(TAG, "Allocating %d bytes internal RAM for http buffer", kHttpBufSizeSpiRam);
            http = new HttpNode(*this, kHttpBufSizeInternal, kHttpBufSizeInternal * 3 / 4);
        }
        mStreamIn.reset(http);

        mDecoder.reset(new DecoderNode(*this));
        mDecoder->linkToPrev(mStreamIn.get());
        pcmSource = mDecoder.get();
        break;
    }
    case AudioNode::kTypeA2dpIn:
        mStreamIn.reset(new A2dpInputNode(*this, "NetPlayer"));
        mDecoder.reset();
        pcmSource = mStreamIn.get();
        break;
    default:
        ESP_LOGE(TAG, "Unknown pipeline input node type %d", inType);
        return false;
    }
    if (mFlags & kFlagUseEqualizer) {
        mEqualizer.reset(new EqualizerNode(*this, sDefaultEqGains));
        mEqualizer->linkToPrev(pcmSource);
        pcmSource = mEqualizer.get();
        auto vuAtInput = mNvsHandle.readDefault<uint8_t>("vuAtEqInput", 0);
        if (vuAtInput) {
            mEqualizer->monitorLevelAtInput(true);
        }
        ESP_LOGI(TAG, "VU source set %s volume and EQ processing", vuAtInput ? "before" : "after");
    }
    switch(outType) {
    case AudioNode::kTypeI2sOut: {
        i2s_pin_config_t cfg = {
            .mck_io_num = I2S_PIN_NO_CHANGE,
            .bck_io_num = 26,
            .ws_io_num = 25,
            .data_out_num = 27,
            .data_in_num = -1,
        };
        mStreamOut.reset(new I2sOutputNode(*this, 0, &cfg, kI2sStackSize, kI2sCpuCore));
        if ((mFlags & kFlagUseEqualizer) == 0) {
            static_cast<I2sOutputNode*>(mStreamOut.get())->useVolumeInterface(true);
        }
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
    detectVolumeNode();
    ESP_LOGI(TAG, "Audio pipeline:\n%s", printPipeline().c_str());
    loadSettings();
    lcdDrawGui();
    return true;
}

void AudioPlayer::lcdDrawGui()
{
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
    assert(mStreamIn && mStreamIn->type() == AudioNode::kTypeHttpIn);
    auto& station = stationList->currStation;
    if (!station.isValid()) {
        return;
    }
    lcdDisplayStationName(station.name());
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
void AudioPlayer::lcdDisplayStationName(const char* name)
{
    mLcd.setFont(font_Camingo32);
    mLcd.setFgColor(255, 255, 128);
    mLcd.clear(0, mLcd.fontHeight() + 6, mLcd.width(), mLcd.fontHeight());
    mLcd.gotoXY(0, mLcd.fontHeight() + 6);
    mLcd.putsCentered(name);
}
void AudioPlayer::lcdUpdateRecIcon()
{
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
    if (!mStreamIn || mStreamIn->type() != AudioNode::kTypeHttpIn) {
        return false;
    }
    lcdUpdateTrackTitle(nullptr);
    auto& http = *static_cast<HttpNode*>(mStreamIn.get());
    auto urlInfo = HttpNode::UrlInfo::Create(url, ++mStreamSeqNo, record);
    http.setUrl(urlInfo);
    if (!isPlaying()) {
        play();
    }
    return true;
}
esp_err_t AudioPlayer::playStation(const char* id)
{
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
    mStreamIn->run();
    mStreamOut->run();
    lcdUpdatePlayState(kSymPlaying);
}

void AudioPlayer::pause()
{
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
uint32_t AudioPlayer::positionTenthSec() const
{
    if (!isPlaying() || !mStreamOut || (mStreamOut->type() != AudioNode::kTypeI2sOut)) {
        return 0;
    }
    auto& i2sOut = *static_cast<I2sOutputNode*>(mStreamOut.get());
    MutexLocker locker(i2sOut.mutex);
    if (i2sOut.mStreamId != mStreamSeqNo) {
        return 0;
    }
    return i2sOut.positionTenthSec();
}
bool AudioPlayer::volumeSet(uint16_t vol)
{
    if (!mVolumeInterface) {
        return false;
    }
    mVolumeInterface->setVolume(vol);
    mNvsHandle.write("volume", vol);
    return true;
}

int AudioPlayer::volumeGet()
{
    if (mVolumeInterface) {
        return mVolumeInterface->getVolume();
    }
    return -1;
}

uint16_t AudioPlayer::volumeChange(int step)
{
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
    if (!str) {
        mEqualizer->zeroAllGains();
        equalizerSaveGains();
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
    DynBuffer buf(128);
    auto param = params.strVal("url");
    if (param) {
        self->lcdDisplayStationName("User link");
        self->playUrl(param.str);
        buf.printf("Playing url '%s'", param.str);
        self->lcdUpdateTrackTitle(param.str);
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

void AudioPlayer::registerHttpGetHandler(const char* path, esp_err_t(*handler)(httpd_req_t*))
{
    httpd_uri_t desc = {
        .uri       = path,
        .method    = HTTP_GET,
        .handler   = handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(mHttpServer.server, &desc);
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

esp_err_t AudioPlayer::equalizerSetUrlHandler(httpd_req_t *req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    MutexLocker locker(self->mutex);
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
esp_err_t AudioPlayer::resetSubsystemUrlHandler(httpd_req_t* req)
{
    auto self = static_cast<AudioPlayer*>(req->user_ctx);
    UrlParams params(req);
    auto key = params.strVal("what");
    if (!key.str) {
        ESP_LOGW(TAG, "Restarting on user request...");
        esp_restart();
        return ESP_OK;
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
            self->changeInput(AudioNode::kTypeA2dpIn);
            httpd_resp_sendstr(req, "Switched to Bluetooth A2DP sink");
            break;
        case 'h':
            self->changeInput(AudioNode::kTypeHttpIn);
            httpd_resp_sendstr(req, "Switched to HTTP client");
            break;
        default:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode param");
            return ESP_OK;
    }
    ESP_LOGI(TAG, "Changed input node to type %d", self->nvs().readDefault<uint8_t>("inType", AudioNode::kTypeHttpIn));
    return ESP_OK;
}

void AudioPlayer::registerUrlHanlers()
{
    registerHttpGetHandler("/play", &playUrlHandler);
    registerHttpGetHandler("/pause", &pauseUrlHandler);
    registerHttpGetHandler("/vol", &volumeUrlHandler);
    registerHttpGetHandler("/eqget", &equalizerDumpUrlHandler);
    registerHttpGetHandler("/eqset", &equalizerSetUrlHandler);
    registerHttpGetHandler("/status", &getStatusUrlHandler);
    registerHttpGetHandler("/nvget", &nvsGetParamUrlHandler);
    registerHttpGetHandler("/nvset", &nvsSetParamUrlHandler);
    registerHttpGetHandler("/inmode", &changeInputUrlHandler);
    registerHttpGetHandler("/reset", &resetSubsystemUrlHandler);
    if (stationList) {
        stationList->registerHttpHandlers(mHttpServer.server);
    }
}

void AudioPlayer::onNodeError(AudioNode& node, int error)
{
    asyncCall([this, error, nodeName = std::string((const char*)node.tag())]() {
        ESP_LOGW(TAG, "Error %d from node '%s', pausing pipeline", error, nodeName.c_str());
        LOCK_PLAYER();
        pause();
    });
}

void AudioPlayer::onNodeEvent(AudioNode& node, uint32_t event, uintptr_t buf, size_t bufSize)
{
    if (node.type() != AudioNode::kTypeHttpIn) {
        return;
    }
    // We are in the http node's thread, must not do any locking from here, so we call
    // into the player via async messages
    if (event == HttpNode::kEventTrackInfo) {
        asyncCall([this, title = std::string((const char*)buf)]() {
            LOCK_PLAYER();
            lcdUpdateTrackTitle(title.c_str());
        });
    }
    else {
        asyncCall([this, event]() {
            LOCK_PLAYER();
            if (event == HttpNode::kEventConnected) {
                lcdUpdatePlayState(kSymPlaying);
            } else if (event == HttpNode::kEventConnecting) {
                lcdUpdatePlayState(kSymConnecting);
            } else if (event == HttpNode::kEventRecording) {
                lcdUpdateRecIcon();
            }
        });
    }
}

void AudioPlayer::lcdTimedDrawTask(void* ctx)
{
    auto& self = *static_cast<AudioPlayer*>(ctx);
    int16_t fps;
    {
        MutexLocker locker(self.mutex);
        fps = self.mNvsHandle.readDefault<uint8_t>("tscrollFps", kDefTitleScrollFps);
    }
    int64_t scrollTickPeriodUs = (1000000 + fps / 2) / fps;
    int64_t now = esp_timer_get_time();
    int64_t tsLastTitleScroll = now - scrollTickPeriodUs - 1;
    int64_t tsLastVolEvent = now;
    for (;;) {
        EventBits_t events;
        now = esp_timer_get_time();
        int64_t timeout = scrollTickPeriodUs - (now - tsLastTitleScroll);
        //ESP_LOGW(TAG, "timeout: %lld\n", timeout);
        events = (timeout > 0)
            ? self.mEvents.waitForOneAndReset(kEventTerminating|kEventVolLevel, (timeout + 500) / 1000)
            : (EventBits_t)0; // events = 0 -> scroll title
        if (events & kEventTerminating) {
            break;
        }
        {
            MutexLocker locker(self.mutex);
            if (events & kEventVolLevel) {
                tsLastVolEvent = now;
                self.mVuDisplay.update(self.mVolumeInterface->audioLevels());
            }
            else if (events == 0) { // timeout or due time to scroll title
                tsLastTitleScroll = now;
                if (self.mTitleScrollEnabled) {
                    self.lcdScrollTrackTitle();
                }
                if (now - tsLastVolEvent > 50000) { // 50ms no volume event, force update the VU display
                    ESP_LOGD(TAG, "No sound output, clearing VU levels");
                    self.mVolumeInterface->clearAudioLevelsNoEvent();
                    self.mVuDisplay.update(self.mVolumeInterface->audioLevels());
                }
            }
        }
    }
    self.mEvents.setBits(kEventTerminated);
    vTaskDelete(nullptr);
}

void AudioPlayer::lcdUpdateTrackTitle(const char* buf)
{
    size_t len;
    if (!buf || !(len = strlen(buf))) {
        mTitleScrollEnabled = false;
        lcdSetupForTrackTitle();
        mLcd.clear(mLcd.cursorX, mLcd.cursorY, mLcd.width(), mLcd.fontHeight());
        return;
    }
    mLcdTrackTitle.reserve(len + 4);
    mLcdTrackTitle.assign(buf, len);
    mLcdTrackTitle.append(" * ", 4);
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
    if (mLcdTrackTitle.dataSize() <= 1) {
        return;
    }
    lcdSetupForTrackTitle();

    auto title = mLcdTrackTitle.buf() + mTitleScrollCharOffset;
    auto titleEnd = mLcdTrackTitle.buf() + mLcdTrackTitle.dataSize() - 1; // without terminating null
    if (title >= titleEnd) {
        title = mLcdTrackTitle.buf();
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
            title = mLcdTrackTitle.buf();
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

