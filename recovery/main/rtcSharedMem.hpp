#ifndef RTC_SHARED_MEM
#define RTC_SHARED_MEM
// This file should be included only once, as it defines variables

#include <esp32/ulp.h>

enum { kRecoveryFlagEraseImmediately = 1, kRecoveryFlagsInvalid = 0xffffffff };
enum: uint32_t { kRecoveryFlagsChecksumKey = 0xcafebabe };

static constexpr uint32_t RECOVERY_FLAGS_RTCMEM_LOCATION = 123;
uint32_t& gRecoveryFlags = RTC_SLOW_MEM[RECOVERY_FLAGS_RTCMEM_LOCATION];
uint32_t& gRecoveryFlagsChecksum = RTC_SLOW_MEM[RECOVERY_FLAGS_RTCMEM_LOCATION + 1];

void recoveryWriteFlags(uint32_t flags)
{
    gRecoveryFlags = flags;
    gRecoveryFlagsChecksum = flags ^ kRecoveryFlagsChecksumKey;
}
uint32_t recoveryReadAndInvalidateFlags()
{
    auto flags = gRecoveryFlags;
    auto checksum = gRecoveryFlagsChecksum;
    gRecoveryFlags = 0;
    gRecoveryFlagsChecksum = 0;
    return ((flags ^ kRecoveryFlagsChecksumKey) == checksum) ? flags : kRecoveryFlagsInvalid;
}

#endif

