#ifndef EVENT_GROUP_HPP
#define EVENT_GROUP_HPP
#include <freertos/event_groups.h>

struct EventGroup
{
    StaticEventGroup_t mEventStruct;
    EventGroupHandle_t mEventGroup;
    EventBits_t mNeverResetBit;
    EventGroup(EventBits_t neverResetBit=0)
    : mEventGroup(xEventGroupCreateStatic(&mEventStruct)),
      mNeverResetBit(neverResetBit) {}
    EventBits_t get() const { return xEventGroupGetBits(mEventGroup); }
    void setBits(EventBits_t bit) { xEventGroupSetBits(mEventGroup, bit); }
    void clearBits(EventBits_t bit) { xEventGroupClearBits(mEventGroup, bit); }

    EventBits_t wait(EventBits_t waitBits, BaseType_t all, BaseType_t autoReset, int msTimeout)
    {
        auto ret = xEventGroupWaitBits(mEventGroup, waitBits, autoReset, all,
            (msTimeout < 0) ? portMAX_DELAY : (msTimeout / portTICK_PERIOD_MS));
        if ((ret & mNeverResetBit) && autoReset) {
            setBits(mNeverResetBit);
        }
        return ret & waitBits;
    }
    EventBits_t waitForOneNoReset(EventBits_t waitBits, int msTimeout) {
        return wait(waitBits, pdFALSE, pdFALSE, msTimeout);
    }
    EventBits_t waitForAllNoReset(EventBits_t waitBits, int msTimeout) {
        return wait(waitBits, pdTRUE, pdFALSE, msTimeout);
    }
    EventBits_t waitForOneAndReset(EventBits_t waitBits, int msTimeout) {
        return wait(waitBits, pdFALSE, pdTRUE, msTimeout);
    }
};

#endif
