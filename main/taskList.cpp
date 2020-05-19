#include "taskList.hpp"

void TaskList::update(std::string* result)
{
    mFreshnessCtr++;
    uint32_t totalRunTime;
    // Take a snapshot of the number of tasks in case it changes while this
    // function is executing.
    auto nTasks = uxTaskGetNumberOfTasks();
    std::vector<TaskStatus_t>tasks(nTasks);
    auto actual = uxTaskGetSystemState(tasks.data(), nTasks, &totalRunTime);
     if (actual != nTasks) {
         myassert(actual < nTasks);
         nTasks = actual;
     }
     uint32_t period = (totalRunTime < mLastTotalRunTime)
         ? 0xffffffff - (mLastTotalRunTime - totalRunTime)
         : totalRunTime - mLastTotalRunTime;
     mLastTotalRunTime = totalRunTime;

     for (auto& task: tasks) {
         auto currTime = task.ulRunTimeCounter;
         uint32_t runTime;
         auto it = mRunTimes.find(task.xTaskNumber);
         if (it == mRunTimes.end()) {
             runTime = currTime;
             mRunTimes.emplace(std::piecewise_construct,
                 std::forward_as_tuple(task.xTaskNumber),
                 std::forward_as_tuple(currTime, mFreshnessCtr));
         } else {
             auto prevTime = it->second.lastRunTime;
             it->second.lastRunTime = currTime;
             it->second.ctr = mFreshnessCtr;
             runTime = (currTime < prevTime)
                 ? 0xffffffff - (prevTime - currTime) // counter wrapped
                 : currTime - prevTime;
         }
         // use ulRunTimeCounter to store CPU percent load
         task.ulRunTimeCounter = ((int64_t)runTime * 100 + period / 2) / period;
     }
     for (auto it = mRunTimes.begin(); it != mRunTimes.end();) {
         if (it->second.ctr != mFreshnessCtr) {
             auto erased = it;
             it++;
             mRunTimes.erase(erased);
         } else {
             it++;
         }
     }

     if (!result) {
         return;
     }
     std::sort(tasks.begin(), tasks.end(), [](TaskStatus_t& a, TaskStatus_t& b) {
         return a.ulRunTimeCounter > b.ulRunTimeCounter;
     });
     result->reserve(50 * tasks.size());
     *result = "name             cpu%  lowstk  prio  core\n";
     char buf[32];
     for (auto& task: tasks) {
         auto nameLen = strlen(task.pcTaskName);
         result->append(task.pcTaskName).append(std::string(18 - nameLen, ' '))
               .append(itoa(task.ulRunTimeCounter, buf, 10)).append("\t")
               .append(itoa(task.usStackHighWaterMark, buf, 10)). append("\t")
               .append(itoa(task.uxBasePriority, buf, 10)).append("\t")
               .append((task.xCoreID < 16) ? itoa(task.xCoreID, buf, 10) : "?")+= "\n";
     }
}
