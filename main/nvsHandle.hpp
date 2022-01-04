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
        }
    }
    bool isValid() const { return mHandle != 0; }
    nvs_handle handle() { return mHandle; }
    virtual ~NvsHandle() {
        if (mCommitTimer) {
            esp_timer_stop(mCommitTimer);
            esp_timer_delete(mCommitTimer);
            mCommitTimer = nullptr;
        }
        commit();
        nvs_close(mHandle);
    }
    /** This starts a periodic timer with period msDelay. The timer handler
     * checks if the time elapsed since the last write is >= msDelay. If not,
     * the handler does nothing, leaving the timer running. On the next tick,
     * if no new writes were performed, the timer handler commits che changes
     * and stops the timer. Thus, the maximum commit delay is 2 * msDelay
     */
    void enableAutoCommit(uint32_t msDelay) {
        mAutoCommitDelayMs = msDelay;
        if (mCommitTimer) {
            if (mTimerRunning) {
                esp_timer_stop(mCommitTimer);
            }
        } else {
            esp_timer_create_args_t args = {};
            args.dispatch_method = ESP_TIMER_TASK;
            args.callback = &commitTimerHandler;
            args.arg = this;
            args.name = "commitTimer";
            ESP_ERROR_CHECK(esp_timer_create(&args, &mCommitTimer));
        }
        mTimerRunning = false;
    }
    void onWrite()
    {
        mTsLastChange = esp_timer_get_time();
        if (!mCommitTimer) {
            return;
        }
        if (!mTimerRunning) {
            ESP_ERROR_CHECK(esp_timer_start_periodic(mCommitTimer, mAutoCommitDelayMs * 1000 / 2));
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
    esp_err_t eraseKey(const char* key) {
        MutexLocker locker(mMutex);
        onWrite();
        return nvs_erase_key(mHandle, key);
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
    static const char* valTypeToStr(nvs_type_t type)
    {
        switch(type) {
            case NVS_TYPE_U8: return "u8";
            case NVS_TYPE_I8: return "i8";
            case NVS_TYPE_U16: return "u16";
            case NVS_TYPE_I16: return "i16";
            case NVS_TYPE_U32: return "u32";
            case NVS_TYPE_I32: return "i32";
            case NVS_TYPE_U64: return "u64";
            case NVS_TYPE_I64: return "i64";
            case NVS_TYPE_STR: return "str";
            case NVS_TYPE_BLOB: return "blob";
            case NVS_TYPE_ANY: return "any";
            default: return "(invalid)";
        }
    }
    static bool typeIsNumeric(nvs_type_t type) { return (type & 0xe0) == 0; } // 0x1x or 0x0x
    static bool typeIsSignedInt(nvs_type_t type) { return (type & 0xf0) == 0x10; } // 0x1x
    static bool typeIsUnsignedInt(nvs_type_t type) { return (type & 0xf0) == 0; } // 0x0x
    template <class T>
    esp_err_t numValToStr(const char* key, DynBuffer& buf)
    {
        T val;
        auto err = read(key, val);
        if (err != ESP_OK) {
            return err;
        }
        auto str = std::to_string(val);
        buf.appendStr(str.c_str());
        return ESP_OK;
    }
    esp_err_t valToString(const char* key, nvs_type_t type, DynBuffer& buf, bool quoteStr=true)
    {
        if (type == NVS_TYPE_STR) {
            size_t len = 0;
            auto err = readString(key, nullptr, len);
            if (err != ESP_OK) {
                return err;
            }
            if (quoteStr) {
                auto wptr = buf.getAppendPtr(len + 2);
                *wptr = '"';
                err = readString(key, wptr + 1, len);
                if (err) {
                    *wptr = 0;
                    return err;
                }
                wptr[len] = '"';
                wptr[len + 1] = 0;
                buf.expandDataSize(len + 2);
            } else {
                auto wptr = buf.getAppendPtr(len);
                err = readString(key, wptr, len);
                if (err != ESP_OK) {
                    *wptr = 0;
                    return err;
                }
                wptr[len-1] = 0;
                buf.expandDataSize(len);
            }
            return ESP_OK;
        }
        else if (type == NVS_TYPE_BLOB) {
            buf.appendStr("\"(BLOB)\"");
            return ESP_OK;
        }
        switch(type) {
            case NVS_TYPE_U8: return numValToStr<uint8_t>(key, buf);
            case NVS_TYPE_I8: return numValToStr<int8_t>(key, buf);
            case NVS_TYPE_U16: return numValToStr<uint16_t>(key, buf);
            case NVS_TYPE_I16: return numValToStr<int16_t>(key, buf);
            case NVS_TYPE_U32: return numValToStr<uint32_t>(key, buf);
            case NVS_TYPE_I32: return numValToStr<int32_t>(key, buf);
            case NVS_TYPE_U64: return numValToStr<uint64_t>(key, buf);
            case NVS_TYPE_I64: return numValToStr<int64_t>(key, buf);
            default: return ESP_ERR_INVALID_ARG;
        }
    }
    esp_err_t writeValueFromString(const char* key, const char* type, const char* strVal)
    {
        switch (type[0]) {
            case 'i': {
                auto sz = type + 1;
                if (strcmp(sz, "64") == 0) {
                    auto val = strtoll(strVal, nullptr, 10);
                    if (!val && errno != 0) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    return write(key, val);
                }
                long val = strtol(strVal, nullptr, 10);
                if (!val && errno != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                if (strcmp(sz, "8") == 0) {
                    return write(key, (int8_t)val);
                } else if (strcmp(sz, "16") == 0) {
                    return write(key, (int16_t)val);
                } else if (strcmp(sz, "32") == 0) {
                    return write(key, (int32_t)val);
                } else {
                    return ESP_ERR_INVALID_ARG;
                }
            }
            case 'u': {
                auto sz = type + 1;
                if (strcmp(sz, "64") == 0) {
                    uint64_t val = strtoull(strVal, nullptr, 10);
                    if (!val && errno != 0) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    return write(key, val);
                }
                unsigned long val = strtoul(strVal, nullptr, 10);
                if (!val && errno != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                if (strcmp(sz, "8") == 0) {
                    return write(key, (uint8_t)val);
                } else if (strcmp(sz, "16") == 0) {
                    return write(key, (uint16_t)val);
                } else if (strcmp(sz, "32") == 0) {
                    return write(key, (uint32_t)val);
                } else {
                    return ESP_ERR_INVALID_ARG;
                }
            }
            case 's': {
                return writeString(key, strVal);
            }
            default:
                return ESP_ERR_NOT_SUPPORTED;
        }
    }
};
#endif
