#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_a2dp_api.h"
#include "sdkconfig.h"
#include "audioNode.hpp"
#include "bluetooth.hpp"
#include "a2dpInputNode.hpp"

static const char *TAG = "a2dp_in";
A2dpInputNode* A2dpInputNode::gSelf = nullptr;
A2dpInputNode::ConnectCb A2dpInputNode::gConnectCb = nullptr;
static const char* a2dpEventToStr(esp_a2d_cb_event_t event);
static const char* audioStateToStr(esp_a2d_audio_state_t state)
{
    switch(state) {
        case ESP_A2D_AUDIO_STATE_SUSPEND: return "stopped";
        case ESP_A2D_AUDIO_STATE_STARTED: return "started";
        default: return "(unknown)";
    }
}
void notifySourceConnected(esp_a2d_cb_event_t event)
{
}
static int codeToSampleRate(uint8_t code)
{
    // defined in bt/host/bluedroid/stack/include/stack/a2d_sbc.h, but it's not public API
    code &= 0xf0;
    switch (code) {
        case 0x20: return 44100;
        case 0x10: return 48000;
        case 0x40: return 32000;
        case 0x80: return 16000;
        default: return 44100;
    }
}
static constexpr const char* kMsgIgnoring = ": ignoring - no instance to handle it";
void A2dpInputNode::sEventCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        uint8_t* bda = param->conn_stat.remote_bda;
        auto state = param->conn_stat.state;
        if (state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            //memcpy(gSelf->mPeerAddr, bda, sizeof(gSelf->mPeerAddr));
            ESP_LOGI(TAG, "Peer %02x:%02x:%02x:%02x:%02x:%02x connected",
                bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            gConnectCb();
            if (!gSelf) {
                ESP_LOGW(TAG, "...app didn't create A2DP sink, ignoring event");
            }
            return;
        }
        if (!gSelf) {
            ESP_LOGW(TAG, "Ignoring CONNECTION_STATE_EVT(%d): no instance to handle it", state);
            return;
        }
        if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "Peer %02x:%02x:%02x:%02x:%02x:%02x disconnected",
                bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            //memset(gSelf->mPeerAddr, 0, sizeof(gSelf->mPeerAddr));
            gSelf->plSendEvent(kEventDisconnected);
        }
        return;
    }
    else if (event == ESP_A2D_PROF_STATE_EVT) {
        auto state = param->a2d_prof_stat.init_state;
        if (state == ESP_A2D_INIT_SUCCESS) {
            ESP_LOGW(TAG, "A2DP initialized");
        }
        else if (state == ESP_A2D_DEINIT_SUCCESS) {
            ESP_LOGW(TAG, "A2DP de-initialized");
        }
        return;
    }
    if (!gSelf) {
        ESP_LOGW(TAG, "Ignoring A2DP event %s: no instance to handle it", a2dpEventToStr(event));
        return;
    }
    switch(event) {
        case ESP_A2D_AUDIO_STATE_EVT: {
            auto state = param->audio_stat.state;
            ESP_LOGD(TAG, "Audio state: %s", audioStateToStr(state));
            if (state == ESP_A2D_AUDIO_STATE_STARTED) {
                gSelf->setState(kStateRunning);
            } else if (state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
                gSelf->setState(kStateStopped);
            } else if (state == ESP_A2D_AUDIO_STATE_STOPPED) {
                gSelf->setState(kStateStopped);
            }
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT: {
            ESP_LOGI(TAG, "Audio stream configuration, codec type %d", param->audio_cfg.mcc.type);
            if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
                int samplerate = codeToSampleRate(param->audio_cfg.mcc.cie.sbc[0]);
                ESP_LOGI(TAG, "Bluetooth configured, sample rate=%d", samplerate);
                if (samplerate != gSelf->mSourceFormat.sampleRate()) {
                    StreamFormat fmt(Codec::KCodecSbc, samplerate, 16, 2);
                    gSelf->postStreamStart(fmt);
                }
            }
            break;
        }
        default:
            ESP_LOGI(TAG, "Unhandled A2DP event %s(%d)", a2dpEventToStr(event), event);
            break;
    }
}
void A2dpInputNode::postStreamStart(StreamFormat fmt)
{
    if (!mSourceFormat) {
        plSendEvent(kEventConnected);
    }
    mSourceFormat = fmt;
    mStreamId = mPipeline.getNewStreamId();
    auto pkt = new NewStreamEvent(mStreamId, fmt, 16);
    if (!mRingBuf.dataSize()) {
        StreamFormat actualFmt = mSourceFormat;
        actualFmt.setCodec(Codec::kCodecWav);
        mWaitingPrefill = actualFmt.prefillAmount();
        pkt->flags |= StreamPacket::kFlagWaitPrefill;
    }
    mRingBuf.pushBack(pkt);
}
void A2dpInputNode::sDataCallback(const uint8_t* data, uint32_t len)
{
    if (!gSelf || gSelf->state() != AudioNode::kStateRunning) {
        return;
    }
    auto pkt = DataPacket::create(len);
    memcpy(pkt->data, data, len);
    gSelf->onData(pkt);
}
void A2dpInputNode::onData(DataPacket* pkt)
{
    mRingBuf.pushBack(pkt);
    MutexLocker locker(mMutex);
    mSpeedProbe.onTraffic(pkt->dataLen);
    if (mWaitingPrefill && mRingBuf.dataSize() > mWaitingPrefill) {
        printf("prefill %lu complete\n", mWaitingPrefill);
        mWaitingPrefill = 0;
        plSendEvent(kEventPrefillComplete, mStreamId);
    }
}
uint32_t A2dpInputNode::pollSpeed()
{
    MutexLocker locker(mMutex);
    return mSpeedProbe.poll();
}
bool A2dpInputNode::install(ConnectCb connectCb, bool becomeDiscoverable)
{
    if (!BtStack.started()) {
        ESP_LOGE(TAG, "Bluetooth stack not started");
        return false;
    }
    gConnectCb = connectCb;
    esp_a2d_register_callback(sEventCallback);
    esp_a2d_sink_register_data_callback(sDataCallback);
    esp_a2d_sink_init();
    if (becomeDiscoverable) {
        auto err = BtStack.becomeDiscoverableAndConnectable();
        if (!err) {
            ESP_LOGW(TAG, "Device is now discoverable");
        }
        else {
            ESP_LOGW(TAG, "Error %s becoming discoverable", esp_err_to_name(err));
            return false;
        }
    }
    return true;
}
void A2dpInputNode::uninstall()
{
    esp_a2d_register_callback(nullptr);
    esp_a2d_sink_register_data_callback(nullptr);
    esp_a2d_sink_deinit();
}
A2dpInputNode::A2dpInputNode(IAudioPipeline& parent): AudioNodeWithState(parent, TAG)
{
    if (gSelf) {
        ESP_LOGE(TAG, "Only a single instance is allowed, and one already exists");
        abort();
    }
    gSelf = this;
}
void A2dpInputNode::onStopped()
{
    auto err = esp_a2d_sink_disconnect(mPeerAddr);
    if (err) {
        ESP_LOGW(mTag, "Error %s disconnecting bluetooth soure", esp_err_to_name(err));
    }
    else {
        printf("=============== Succcess calling sink_disconnect\n");
    }
}
A2dpInputNode::~A2dpInputNode()
{
    gSelf = nullptr;
}
StreamEvent A2dpInputNode::pullData(PacketResult& dpr)
{
    auto pkt = mRingBuf.popFront();
    if (pkt) {
        return dpr.set(pkt);
    }
    else {
        return kErrStreamStopped;
    }
}
#define EVTCASE(name) case ESP_A2D_##name: return #name
const char* a2dpEventToStr(esp_a2d_cb_event_t event)
{
    switch(event) {
        EVTCASE(CONNECTION_STATE_EVT);
        EVTCASE(AUDIO_STATE_EVT);
        EVTCASE(AUDIO_CFG_EVT);
        EVTCASE(MEDIA_CTRL_ACK_EVT);
        EVTCASE(PROF_STATE_EVT);
        EVTCASE(SNK_PSC_CFG_EVT);
        EVTCASE(SNK_SET_DELAY_VALUE_EVT);
        EVTCASE(SNK_GET_DELAY_VALUE_EVT);
        EVTCASE(REPORT_SNK_DELAY_VALUE_EVT);
        default: return "(unknown)";
    }
}

