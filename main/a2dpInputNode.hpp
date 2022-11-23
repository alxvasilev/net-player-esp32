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
#include "bluetooth.hpp"

class A2dpInputNode: public AudioNodeWithState
{
public:
    enum { kBufferSize = 8 * 1024 };
    enum: uint16_t {
        kEventDisconnect = 1,
        kEventConnect,

    };
protected:
    static A2dpInputNode* gSelf; // bluetooth callbacks don't have a user pointer
    RingBuf mRingBuf;
    StreamFormat mFormat;
    static void eventCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
    static void dataCallback(const uint8_t* data, uint32_t len);
    bool doRun();
    void doStop();
public:
    virtual Type type() const override { return AudioNode::kTypeA2dpIn; }
    A2dpInputNode(IAudioPipeline& parent, const char* btName);
    ~A2dpInputNode();
    virtual StreamError pullData(DataPullReq& dpr) override;
    virtual void confirmRead(int amount);
};
#endif
