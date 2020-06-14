#ifndef TIMER_HPP_INCLUDED
#define TIMER_HPP_INCLUDED
#include <esp_timer.h>
#include <memory>

class Timer: public std::enable_shared_from_this<Timer>
{
protected:
    esp_timer_handle_t mTimer = nullptr;
public:
    void cancel() {
        if (!mTimer) {
            return;
        }
        esp_timer_stop(mTimer);
        esp_timer_delete(mTimer);
        mTimer = nullptr;
    }
    bool running() const { return mTimer != nullptr; }
    ~Timer() { cancel(); }
};

template <class F, bool isOneShot>
class LambdaTimer: Timer
{
protected:
    typedef LambdaTimer<F, isOneShot> Self;
    F mUserCb;
    std::shared_ptr<Timer> mSharedSelf;
    LambdaTimer(F&& userCb, uint64_t us): mUserCb(std::forward<F>(userCb))
    {
        esp_timer_create_args_t args = {};
        args.dispatch_method = ESP_TIMER_TASK;
        args.callback = cFunc;
        args.arg = this;
        args.name = isOneShot ? "setTimeout" : "setInterval";
        ESP_ERROR_CHECK(esp_timer_create(&args, &mTimer));
        ESP_ERROR_CHECK(isOneShot
            ? esp_timer_start_once(mTimer, us)
            : esp_timer_start_periodic(mTimer, us));
    }
    static void cFunc(void* ctx)
    {
        auto self = static_cast<Self*>(ctx);
        self->mUserCb();
        if (isOneShot) {
            self->cancel();
            self->mSharedSelf.reset(); // triggers destroy if no external shared pointers
        }
    }
public:
    static std::shared_ptr<Timer>& create(uint32_t us, F&& cb)
    {
        auto timer = std::make_shared<Self>(std::forward<F>(cb), us);
        timer->mSharedSelf = timer;
        return timer->mSharedSelf;
    }
};

template <class F>
std::shared_ptr<Timer> setTimeout(uint32_t ms, F&& cb)
{
    return LambdaTimer<F, true>::create(std::forward<F>(cb), ms * 1000);
}

template <class F>
std::shared_ptr<Timer> setInterval(uint32_t ms, F&& cb)
{
    return LambdaTimer<F, false>::create(std::forward<F>(cb), ms * 1000);
}

class CbTimer: public Timer
{
public:
    typedef void(*Callback)(void*);
    void start(uint64_t ms, bool isOneShot, Callback cb, void* ctx)
    {
        assert(!mTimer);
        esp_timer_create_args_t args = {};
        args.dispatch_method = ESP_TIMER_TASK;
        args.callback = cb;
        args.arg = ctx;
        args.name = "cbTimer";
        ESP_ERROR_CHECK(esp_timer_create(&args, &mTimer));
        ESP_ERROR_CHECK(isOneShot
            ? esp_timer_start_once(mTimer, ms * 1000)
            : esp_timer_start_periodic(mTimer, ms * 1000));
    }
};

#endif
