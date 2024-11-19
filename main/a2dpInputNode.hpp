#ifndef A2DP_INPUT_NODE_HPP
#define A2DP_INPUT_NODE_HPP

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "audioNode.hpp"
#include "streamRingQueue.hpp"
#include "bluetooth.hpp"
#include "speedProbe.hpp"
#include "utils.hpp"

class AudioPlayer;
class A2dpInputNode: public AudioNodeWithState, public IInputAudioNode
{
public:
    enum { kBufferSize = 8 * 1024 };
    typedef void(*ConnectCb)();
protected:
    static ConnectCb gConnectCb;
    static A2dpInputNode* gSelf; // bluetooth callbacks don't have a user pointer
    AudioPlayer& mPlayer;
    StreamRingQueue<100> mRingBuf;
    StreamFormat mSourceFormat;
    LinkSpeedProbe mSpeedProbe;
    uint32_t mWaitingPrefill = 0;
    unique_ptr_mfree<const char> mTrackTitle;
    unique_ptr_mfree<const char> mTrackArtist;
    esp_avrc_rn_evt_cap_mask_t mAvrcPeerCaps = (esp_avrc_rn_evt_cap_mask_t)0;
    StreamId mStreamId = 0;
    uint8_t mPeerAddr[6] = {0};
    uint8_t mAvrcTransId = 0;
    bool mAvrcNotifyVolChange = false;
    bool mManageDiscoverable;
    static void sA2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    static void sAvrcCtrlCallback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
    static void sAvrcTargetCallback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
    static void sDataCallback(const uint8_t* data, uint32_t len);
    void a2dpHandleConnEvent(uint8_t* bda, esp_a2d_connection_state_t state);
    void a2dpHandleEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t& param);
    void avrcCtrlHandleEvent(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t& param);
    void avrcCtrlHandleNotifyEvent(uint8_t event, esp_avrc_rn_param_t& param);
    void avrcTargetHandleEvent(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t& param);
    void avrcCtrlSubscribeToEvent(esp_avrc_rn_event_ids_t event);
    void avrcCtrlRequestTrackInfo();


    virtual void onStopped() override;
    void postStreamStart(StreamFormat fmt);
    void onData(DataPacket* pkt);
    uint8_t avrcNewTransId() { return (++mAvrcTransId) & 0x0f; }
    int bufferDelay() const;
public:
    /* Registers connection listener, and optionally makes us discoverable.
     * Whean an A2DP source connects to us, notifies the application, which should create an
     * A2dpInputNode instance and switch to bluetooth input
     */
    static bool install(ConnectCb connectCb, bool makeDiscoverable);
    static void uninstall();
    virtual Type type() const override { return AudioNode::kTypeA2dpIn; }
    A2dpInputNode(AudioPlayer& player, bool manageDiscoverable);
    ~A2dpInputNode();
    virtual StreamEvent pullData(PacketResult& dpr) override;
    virtual IInputAudioNode* inputNodeIntf() override { return static_cast<IInputAudioNode*>(this); }
    virtual uint32_t pollSpeed() override;
    virtual uint32_t bufferedDataSize() const override { return mRingBuf.dataSize(); }
    virtual void onVolumeChange(int vol) override;
};
#endif
