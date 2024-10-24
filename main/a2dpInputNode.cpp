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

static const char* audioStateToStr(esp_a2d_audio_state_t state)
{
    switch(state) {
    case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND: return "suspended";
    case ESP_A2D_AUDIO_STATE_STOPPED: return "stopped";
    case ESP_A2D_AUDIO_STATE_STARTED: return "started";
    default: return "(unknown)";
    }
}

void A2dpInputNode::eventCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    myassert(gSelf);
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            uint8_t* bda = param->conn_stat.remote_bda;
            ESP_LOGI(TAG, "Remote Bluetooth MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                     bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            auto state = param->conn_stat.state;
            if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "Disconnected");
                gSelf->plSendEvent(kEventDisconnect);
            } else if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "Connected");
                gSelf->plSendEvent(kEventConnect);
            }
            break;
        }
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
                char oct0 = param->audio_cfg.mcc.cie.sbc[0];
                int samplerate;
                if (oct0 & (0x01 << 6)) {
                    samplerate = 32000;
                } else if (oct0 & (0x01 << 5)) {
                    samplerate = 44100;
                } else if (oct0 & (0x01 << 4)) {
                    samplerate = 48000;
                } else {
                    samplerate = 16000;
                }
                ESP_LOGI(TAG, "Bluetooth configured, sample rate=%d", samplerate);
                if (samplerate == gSelf->mFormat.sampleRate()) {
                    return;
                }
                // TODO: Flush ringbuffer
                gSelf->mFormat.setNumChannels(2);
                gSelf->mFormat.setBitsPerSample(16);
                gSelf->mFormat.setSampleRate(samplerate);
            }
            break;
        }
        default:
            ESP_LOGI(TAG, "Unhandled A2DP event: %d", event);
            break;
    }
}

void A2dpInputNode::dataCallback(const uint8_t* data, uint32_t len)
{
//    ESP_LOGI(TAG, "Recv %d bytes, writing to ringbuf(%d)", len, gSelf->mRingBuf.totalDataAvail());
    auto pkt = DataPacket::create(len);
    memcpy(pkt->data, data, len);
    if (!gSelf->mRingBuf.pushBack(pkt)) {
        gSelf->stop(false);
    }
}

A2dpInputNode::A2dpInputNode(IAudioPipeline& parent, const char* btName)
: AudioNodeWithState(parent, TAG)
{
    if (gSelf) {
        ESP_LOGE(TAG, "Only a single instance is allowed, and one already exists");
        abort();
    }
    gSelf = this;
    if (!BtStack.started()) {
        BtStack.start(ESP_BT_MODE_CLASSIC_BT, btName);
    }
}

bool A2dpInputNode::doRun()
{
    esp_a2d_register_callback(eventCallback);
    esp_a2d_sink_register_data_callback(dataCallback);
    esp_a2d_sink_init();
    /* set discoverable and connectable mode, wait to be connected */
    BtStack.becomeDiscoverableAndConnectable();
    setState(kStateRunning);
    return true;
}
void A2dpInputNode::doStop()
{
    esp_a2d_sink_deinit();
    setState(kStateStopped);
}
A2dpInputNode::~A2dpInputNode()
{
}
StreamEvent A2dpInputNode::pullData(PacketResult& dpr)
{
    auto pkt = mRingBuf.popFront();
    if (pkt) {
        dpr.packet.reset(pkt);
        return kEvtData;
    }
    else {
        return kErrStreamStopped;
    }
}
