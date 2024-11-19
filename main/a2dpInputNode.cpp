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
#include "audioPlayer.hpp"
#include "magic_enum.hpp"

static constexpr const char* TAG = "a2dp-in";
static constexpr const char* TAG_AVRC = "a2dp-in:avrc";
#define bda2str(arg) BluetoothStack::bda2str(arg).data()
A2dpInputNode* A2dpInputNode::gSelf = nullptr;
A2dpInputNode::ConnectCb A2dpInputNode::gConnectCb = nullptr;
static const char* a2dpEventToStr(esp_a2d_cb_event_t event);
static const char* avrcNotifIdToStr(uint8_t id);

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
void A2dpInputNode::sA2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        uint8_t* bda = param->conn_stat.remote_bda;
        auto state = param->conn_stat.state;
        if (state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            ESP_LOGI(TAG, "Peer %s connected", bda2str(bda));
            gConnectCb();
            if (!gSelf) {
                ESP_LOGW(TAG, "...app didn't create A2DP sink, ignoring event");
            }
            return;
        }
        if (gSelf) {
            gSelf->a2dpHandleConnEvent(bda, state);
        }
        else {
            ESP_LOGW(TAG, "Ignoring A2DP conn event %d: No instance to handle it", state);
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
    if (gSelf) {
        gSelf->a2dpHandleEvent(event, *param);
    }
    else {
        ESP_LOGW(TAG, "Ignoring %s event %s: No instance to handle it", "A2DP", a2dpEventToStr(event));
    }
}
void A2dpInputNode::sAvrcCtrlCallback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (gSelf) {
        gSelf->avrcCtrlHandleEvent(event, *param);
    }
    else {
        ESP_LOGW(TAG_AVRC, "Ignoring %s event %s: No instance to handle it", "CTRL", magic_enum::enum_name(event).data());
    }
}
void A2dpInputNode::sAvrcTargetCallback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param)
{
    if (gSelf) {
        gSelf->avrcTargetHandleEvent(event, *param);
    }
    else {
        ESP_LOGW(TAG_AVRC, "Ignoring %s event %s: No instance to handle it", "TARGET", magic_enum::enum_name(event).data());
    }
}
void A2dpInputNode::a2dpHandleConnEvent(uint8_t* bda, esp_a2d_connection_state_t state)
{
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        memcpy(mPeerAddr, bda, sizeof(mPeerAddr));
        ESP_LOGI(TAG, "Peer %s connected", bda2str(bda));
        if (mManageDiscoverable) {
            BtStack.becomeDiscoverableAndConnectable(false);
        }
        plSendEvent(kEventConnected);
    }
    else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        ESP_LOGI(TAG, "Peer %s disconnected", bda2str(bda));
        if (mManageDiscoverable) {
            BtStack.becomeDiscoverableAndConnectable(true);
        }
        memset(mPeerAddr, 0, sizeof(mPeerAddr));
        plSendEvent(kEventDisconnected);
    }
    else {
        ESP_LOGI(TAG, "Unhandled A2DP conn state %d", state);
    }
}
void A2dpInputNode::a2dpHandleEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t& param)
{
    switch(event) {
        case ESP_A2D_AUDIO_STATE_EVT: {
            auto state = param.audio_stat.state;
            ESP_LOGI(TAG, "Audio state: %s", audioStateToStr(state));
            if (state == ESP_A2D_AUDIO_STATE_STARTED) {
                setState(kStateRunning);
            } else if (state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
                setState(kStateStopped);
            } else if (state == ESP_A2D_AUDIO_STATE_STOPPED) {
                setState(kStateStopped);
            }
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT: {
            if (param.audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
                int samplerate = codeToSampleRate(param.audio_cfg.mcc.cie.sbc[0]);
                ESP_LOGI(TAG, "Bluetooth stream config: sample rate=%d", samplerate);
                postStreamStart(StreamFormat(Codec::KCodecSbc, samplerate, 16, 2));
            }
            break;
        }
        case ESP_A2D_SNK_GET_DELAY_VALUE_EVT: {
            auto delay = param.a2d_get_delay_value_stat.delay_value;
            ESP_LOGI(TAG, "Reported delay value: %d * 1/10 ms", delay);
            esp_a2d_sink_set_delay_value(delay + bufferDelay());
            break;
        }
        case ESP_A2D_SNK_SET_DELAY_VALUE_EVT:
            if (ESP_A2D_SET_INVALID_PARAMS == param.a2d_set_delay_value_stat.set_state) {
                ESP_LOGW(TAG, "Failed to set delay");
            }
            else {
                ESP_LOGI(TAG, "Confirmed set delay to %d * 1/10 ms", param.a2d_set_delay_value_stat.delay_value);
            }
            break;
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
    mRingBuf.pushBack(new NewStreamEvent(mStreamId, fmt, 16));
    if (!mRingBuf.dataSize()) {
        StreamFormat actualFmt = mSourceFormat;
        actualFmt.setCodec(Codec::kCodecWav);
        mWaitingPrefill = actualFmt.prefillAmount();
        mRingBuf.pushBack(new PrefillEvent(mStreamId, ""));
    }
}
void A2dpInputNode::avrcCtrlHandleEvent(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t& param)
{
    switch(event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
            auto connected = param.conn_stat.connected;
            ESP_LOGI(TAG_AVRC, "Peer %s %s", bda2str(param.conn_stat.remote_bda),
                connected ? "connected" : "disconnected");
            if (connected) {
                /* get remote supported event_ids of peer AVRCP Target */
                esp_avrc_ct_send_get_rn_capabilities_cmd(avrcNewTransId());
            }
            else {
                /* clear peer notification capability record */
                mAvrcPeerCaps.bits = 0;
            }
            break;
        }
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
            ESP_LOGI(TAG_AVRC, "Received remote caps: count %d, bitmask 0x%x",
                param.get_rn_caps_rsp.cap_count, param.get_rn_caps_rsp.evt_set.bits);
            mAvrcPeerCaps.bits = param.get_rn_caps_rsp.evt_set.bits;
            avrcCtrlRequestTrackInfo();
            avrcCtrlSubscribeToEvent(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
            avrcCtrlSubscribeToEvent(ESP_AVRC_RN_PLAY_POS_CHANGED);
            break;
        }
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            auto& attr = param.meta_rsp;
            auto id = attr.attr_id;
            auto txt = (const char*)attr.attr_text;
            auto len = attr.attr_length;
            ESP_LOGI(TAG_AVRC, "Stream metadata: attribute 0x%x, val: %s", id, attr.attr_text);
            if (id == ESP_AVRC_MD_ATTR_TITLE) {
                mTrackTitle.reset(txt && len ? strndup(txt, len) : nullptr);
            }
            else if (id == ESP_AVRC_MD_ATTR_ARTIST) {
                mTrackArtist.reset(txt && len ? strndup(txt, len) : nullptr);
            }
            else {
                break;
            }
            if (mTrackTitle && mTrackArtist) {
                mRingBuf.pushBack(new TitleChangeEvent(mTrackTitle.release(), mTrackArtist.release()));
            }
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
            auto& notif = param.change_ntf;
            avrcCtrlHandleNotifyEvent(notif.event_id, notif.event_parameter);
            break;
        }
        case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
            ESP_LOGI(TAG_AVRC, "Remote features %lx, TG features %x", param.rmt_feats.feat_mask, param.rmt_feats.tg_feat_flag);
            break;
        }
        case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT: {
            auto& pt = param.psth_rsp;
            ESP_LOGI(TAG_AVRC, "Passthrough response: key_code 0x%x, key_state %d, rsp_code %d",
                pt.key_code, pt.key_state, pt.rsp_code);
            break;
        }
        default:
            ESP_LOGI(TAG_AVRC, "Unhandled CTRL event %d", event);
            break;
    }
}
void A2dpInputNode::avrcCtrlHandleNotifyEvent(uint8_t event, esp_avrc_rn_param_t& param)
{
    ESP_LOGI(TAG_AVRC, "Notif. event %s", avrcNotifIdToStr(event));
    switch(event) {
        case ESP_AVRC_RN_TRACK_CHANGE:
            avrcCtrlRequestTrackInfo();
            break;
        case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
            avrcCtrlSubscribeToEvent(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
            break;
        case ESP_AVRC_RN_PLAY_POS_CHANGED:
            avrcCtrlSubscribeToEvent(ESP_AVRC_RN_PLAY_POS_CHANGED);
            ESP_LOGI(TAG_AVRC, "Play position changed: %lums", param.play_pos);
            break;
        default:
            ESP_LOGI(TAG_AVRC, "...unhandled");
            break;
    }
}
void A2dpInputNode::avrcCtrlSubscribeToEvent(esp_avrc_rn_event_ids_t event)
{
    /* register notification if peer supports the event_id */
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &mAvrcPeerCaps, event)) {
        esp_avrc_ct_send_register_notification_cmd(avrcNewTransId(), event, 0);
    }
}
void A2dpInputNode::avrcCtrlRequestTrackInfo()
{
    /* request metadata */
    esp_avrc_ct_send_metadata_cmd(avrcNewTransId(),
        ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_GENRE);
    avrcCtrlSubscribeToEvent(ESP_AVRC_RN_TRACK_CHANGE);
}
void A2dpInputNode::avrcTargetHandleEvent(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t& param)
{
    switch(event) {
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
            int vol = ((int)param.set_abs_vol.volume * 100 + 64) / 127;
            ESP_LOGI(TAG_AVRC, "Remote set absolute volume to %d%%", vol);
            mPlayer.volumeSet(vol);
            break;
        }
        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
            if (param.reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                ESP_LOGI(TAG_AVRC, "Requested subscribe for volume change, vol = %lu", param.reg_ntf.event_parameter);
                mAvrcNotifyVolChange = true;
                esp_avrc_rn_param_t rn_param = {0};
                rn_param.volume = (mPlayer.volumeGet() * 127 + 50) / 100;
                esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
            }
            else {
                ESP_LOGI(TAG_AVRC, "Requested subscribe for unsupported TG event: %d, param: 0x%lx",
                    param.reg_ntf.event_id, param.reg_ntf.event_parameter);
            }
            break;
        }
        case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
            ESP_LOGI(TAG_AVRC, "=====================TG connection to %s state %d", bda2str(param.conn_stat.remote_bda),
                param.conn_stat.connected);
            break;
        }
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
            ESP_LOGI(TAG_AVRC, "TG passthrough cmd: key_code 0x%x, key_state %d", param.psth_cmd.key_code, param.psth_cmd.key_state);
            break;
        }
        default:
            ESP_LOGI(TAG_AVRC, "TG event %d not handled", event);
            break;
    }
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
        ESP_LOGI(mTag, "Prefill %lu complete", mWaitingPrefill);
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
    // initialize AVRCP controller
    MY_ESP_ERRCHECK(esp_avrc_ct_init(), TAG_AVRC, "init avrc CT",);
//    esp_avrc_ct_register_callback(sAvrcCtrlCallback);
    MY_ESP_ERRCHECK(esp_avrc_tg_init(), TAG_AVRC, "init avrc TG",);
//    esp_avrc_tg_register_callback(sAvrcTargetCallback);
    MY_ESP_ERRCHECK(esp_a2d_sink_init(), TAG, "init a2dp sink",);
    esp_a2d_register_callback(sA2dpCallback);
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
    myassert(!gSelf);
    esp_a2d_register_callback(nullptr);
    esp_avrc_ct_deinit();
    esp_a2d_sink_deinit();
}
A2dpInputNode::A2dpInputNode(AudioPlayer& player, bool manageDiscoverable)
: AudioNodeWithState(player, TAG), mPlayer(player), mManageDiscoverable(manageDiscoverable)
{
    if (gSelf) {
        ESP_LOGE(TAG, "Only a single instance is allowed, and one already exists");
        abort();
    }
    esp_avrc_ct_register_callback(sAvrcCtrlCallback);
    esp_avrc_tg_register_callback(sAvrcTargetCallback);
    esp_a2d_sink_register_data_callback(sDataCallback);
    gSelf = this;
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    assert(esp_avrc_tg_set_rn_evt_cap(&evt_set) == ESP_OK);
}
void A2dpInputNode::onStopped()
{
    auto err = esp_a2d_sink_disconnect(mPeerAddr); // FIXME: Maybe move this to the destructor
    if (err) {
        ESP_LOGW(mTag, "Error %s disconnecting bluetooth source", esp_err_to_name(err));
    }
}
void A2dpInputNode::onVolumeChange(int vol)
{
    if (mAvrcNotifyVolChange) {
        esp_avrc_rn_param_t rn_param = {0};
        rn_param.volume = vol;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
        mAvrcNotifyVolChange = false;
    }
}
A2dpInputNode::~A2dpInputNode()
{
    esp_a2d_sink_register_data_callback(nullptr);
    esp_avrc_ct_register_callback(nullptr);
    esp_avrc_tg_register_callback(nullptr);
    gSelf = nullptr;
}
StreamEvent A2dpInputNode::pullData(PacketResult& dpr)
{
    if (!mRingBuf.dataSize()) {
        ESP_LOGW(mTag, "Underrun");
    }
    auto pkt = mRingBuf.popFront();
    if (pkt) {
        return dpr.set(pkt);
    }
    else {
        return kErrStreamStopped;
    }
}
int A2dpInputNode::bufferDelay() const
{
    int bytesPerMs = (mSourceFormat.bitsPerSample() * mSourceFormat.sampleRate() * 4000) / 8000;
    return mRingBuf.dataSize() / bytesPerMs;
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
#define AVRCN_CASE(name) case ESP_AVRC_RN_##name: return #name
const char* avrcNotifIdToStr(uint8_t id)
{
    switch(id) {
        AVRCN_CASE(PLAY_STATUS_CHANGE);
        AVRCN_CASE(TRACK_CHANGE);
        AVRCN_CASE(TRACK_REACHED_END);
        AVRCN_CASE(TRACK_REACHED_START);
        AVRCN_CASE(PLAY_POS_CHANGED);
        AVRCN_CASE(BATTERY_STATUS_CHANGE);
        AVRCN_CASE(SYSTEM_STATUS_CHANGE);
        AVRCN_CASE(APP_SETTING_CHANGE);
        default: return "(unknown)";
    }
}
