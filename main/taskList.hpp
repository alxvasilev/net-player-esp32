#ifndef TASKLIST_HPP
#define TASKLIST_HPP

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>
#include <map>
#include <algorithm>
#include "utils.hpp"

class TaskList
{
protected:
    struct TaskPersist
    {
        uint32_t lastRunTime;
        uint8_t ctr;
        TaskPersist(uint32_t aLrt, uint8_t aCtr)
        : lastRunTime(aLrt), ctr(aCtr){}
    };
    std::map<UBaseType_t, TaskPersist> mRunTimes;
    int64_t mLastTotalRunTime = 0;
    uint8_t mFreshnessCtr = 0;
public:
    void update(std::string* result = nullptr);
};

#endif
