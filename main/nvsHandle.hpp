#ifndef NVS_HANDLE_HPP_INCLUDED
#define NVS_HANDLE_HPP_INCLUDED

#include <nvs.h>
#include "utils.hpp"
class NvsHandle
{
protected:
    nvs_handle mHandle;
    Mutex mMutex;
    int64_t mTsLastChange = 0;
    uint32_t mAutoCommitDelayMs = 0;
    esp_timer_handle_t mCommitTimer = 0;
    bool mTimerRunning = false;
    static const char* tag() { static const char* sTag = "nvs-handle"; return sTag; }
public:
    NvsHandle(const char* nsName, nvs_open_mode mode=NVS_READONLY)
    {
        auto err = nvs_open_from_partition("nvs", nsName, mode, &mHandle);
        if (err != ESP_OK) {
            ESP_LOGE(tag(), "Error opening NVS handle: %s", esp_err_to_name(err));
            mHandle = 0;
            return;
        }
    }
    virtual ~NvsHandle() {
        if (mCommitTimer) {
            esp_timer_stop(mCommitTimer);
            esp_timer_delete(mCommitTimer);
            mCommitTimer = nullptr;
        }
        commit();
        nvs_close(mHandle);
    }
    void enableAutoCommit(uint32_t delay) {
        mAutoCommitDelayMs = delay;
        if (mCommitTimer) {
            if (mTimerRunning) {
                esp_timer_stop(mCommitTimer);
                mTimerRunning = false;
            }
        } else {
            mTimerRunning = false;
            esp_timer_create_args_t args = {};
            args.dispatch_method = ESP_TIMER_TASK;
            args.callback = &commitTimerHandler;
            args.arg = this;
            args.name = "commitTimer";
            ESP_ERROR_CHECK(esp_timer_create(&args, &mCommitTimer));
        }
    }
    void onWrite()
    {
        mTsLastChange = esp_timer_get_time();
        if (!mCommitTimer) {
            return;
        }
        if (!mTimerRunning) {
            ESP_ERROR_CHECK(esp_timer_start_periodic(mCommitTimer, mAutoCommitDelayMs * 1000));
            mTimerRunning = true;
        }
    }
    static void commitTimerHandler(void* ctx) {
        static_cast<NvsHandle*>(ctx)->onCommitTimer();
    }
    void onCommitTimer() {
        MutexLocker locker(mMutex);
        if (!mTsLastChange) {
            ESP_LOGI(tag(), "onCommitTimer: no changes");
            return;
        }
        if ((esp_timer_get_time() - mTsLastChange) / 1000 < mAutoCommitDelayMs) {
            ESP_LOGI(tag(), "onCommitTimer: too soon");
            return;
        }
        ESP_LOGI(tag(), "onCommitTimer: commit");
        esp_timer_stop(mCommitTimer);
        mTimerRunning = false;
        mTsLastChange = 0;
        nvs_commit(mHandle);
    }
    esp_err_t readString(const char* key, char* str, size_t& len) {
        return nvs_get_str(mHandle, key, str, &len);
    }
    esp_err_t readBlob(const char* key, void* data, size_t& len) {
        return nvs_get_blob(mHandle, key, data, &len);
    }
    esp_err_t writeString(const char* key, const char* str) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_str(mHandle, key, str);
    }
    esp_err_t writeBlob(const char* key, void* data, size_t len) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_blob(mHandle, key, data, len);
    }
    esp_err_t read(const char* key, uint64_t& val) {
        return nvs_get_u64(mHandle, key, &val);
    }
    esp_err_t read(const char* key, int64_t& val) {
        return nvs_get_i64(mHandle, key, &val);
    }
    esp_err_t read(const char* key, uint32_t& val) {
        return nvs_get_u32(mHandle, key, &val);
    }
    esp_err_t read(const char* key, int32_t& val) {
        return nvs_get_i32(mHandle, key, &val);
    }
    esp_err_t read(const char* key, uint16_t& val) {
        return nvs_get_u16(mHandle, key, &val);
    }
    esp_err_t read(const char* key, int16_t& val) {
        return nvs_get_i16(mHandle, key, &val);
    }
    esp_err_t read(const char* key, uint8_t& val) {
        return nvs_get_u8(mHandle, key, &val);
    }
    esp_err_t read(const char* key, int8_t& val) {
        return nvs_get_i8(mHandle, key, &val);
    }
    esp_err_t write(const char* key, uint64_t val) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_u64(mHandle, key, val);
    }
    esp_err_t write(const char* key, int64_t val) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_i64(mHandle, key, val);
    }
    esp_err_t write(const char* key, uint32_t val) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_u32(mHandle, key, val);
    }
    esp_err_t write(const char* key, int32_t val) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_i32(mHandle, key, val);
    }
    esp_err_t write(const char* key, uint16_t val) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_u16(mHandle, key, val);
    }
    esp_err_t write(const char* key, int16_t val) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_i16(mHandle, key, val);
    }
    esp_err_t write(const char* key, uint8_t val) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_u8(mHandle, key, val);
    }
    esp_err_t write(const char* key, int8_t val) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_set_i8(mHandle, key, val);
    }
    template <typename T>
    T readDefault(const char* key, T defVal) {
        T val;
        auto err = read(key, val);
        return (err == ESP_OK) ? val : defVal;
    }
    void commit()
    {
        MutexLocker locker(mMutex);
        mTsLastChange = 0;
        nvs_commit(mHandle);
    }
};
#endif
