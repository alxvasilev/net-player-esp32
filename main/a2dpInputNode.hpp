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
#include "audioNode.hpp"
#include "streamRingQueue.hpp"
#include "bluetooth.hpp"

class A2dpInputNode: public AudioNodeWithState
{
public:
    enum { kBufferSize = 8 * 1024 };
    typedef void(*ConnectCb)();
protected:
    static ConnectCb gConnectCb;
    static A2dpInputNode* gSelf; // bluetooth callbacks don't have a user pointer
    StreamRingQueue<100> mRingBuf;
    StreamFormat mFormat;
    uint8_t mPeerAddr[6] = {0};
    static void sEventCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    static void sDataCallback(const uint8_t* data, uint32_t len);
    virtual void onStopped() override;
    void postStreamStart(StreamFormat fmt);
public:
    /* Registers connection listener, and optionally makes us discoverable.
     * Whean an A2DP source connects to us, notifies the application, which should create an
     * A2dpInputNode instance and switch to bluetooth input
     */
    static bool install(ConnectCb connectCb, bool makeDiscoverable);
    static void uninstall();
    virtual Type type() const override { return AudioNode::kTypeA2dpIn; }
    A2dpInputNode(IAudioPipeline& parent);
    ~A2dpInputNode();
    virtual StreamEvent pullData(PacketResult& dpr) override;
};
#endif
