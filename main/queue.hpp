#ifndef QUEUE_HPP
#define QUEUE_HPP
#include <freertos/queue.h>
#include <stdint.h>

template <class Item, int N>
class Queue
{
protected:
    StaticQueue_t mStaticStruct;
    uint8_t mStorage[N * sizeof(Item)];
    QueueHandle_t mHandle;
public:
    Queue()
    :mHandle(xQueueCreateStatic(N, sizeof(Item), mStorage, &mStaticStruct)) {}
    void post(Item& item)
    {
        xQueueSendToBack(mHandle, &item, portMAX_DELAY);
    }
    template <class... Args>
    void post(Args...args)
    {
        Item item(args...);
        post(item);
    }
    bool get(Item& item, int msTimeout)
    {
        auto ret = xQueueReceive(mHandle, &item,
            (msTimeout < 0) ? portMAX_DELAY: msTimeout / portTICK_PERIOD_MS);
        return ret == pdTRUE;
    }
    bool waitForMessage(int msTimeout)
    {
        Item item;
        auto ret = xQueuePeek(mHandle, &item, msTimeout / portTICK_PERIOD_MS);
        return ret == pdTRUE;
    }
    UBaseType_t numMessages()
    {
        return uxQueueMessagesWaiting(mHandle);
    }
};

#endif // QUEUE_HPP
