// libSceRtc HLE — real-time clock stubs using the host system clock.
//
// PS5 games call these during boot for timestamps, timers, and clock init.
// We implement them using Win32 FILETIME / SYSTEMTIME so the guest gets
// plausible values without any hardware access.
//
// SceRtcTick: u64 microseconds since 1970-01-01 00:00:00 UTC.
// SceRtcDateTime: year(u16), month(u8), day(u8), hour(u8), min(u8), sec(u8), ms(u32).
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>
#include <cstring>

namespace HLE {

namespace {

// Convert FILETIME (100-ns intervals since 1601-01-01) to PS5 RTC tick
// (microseconds since 1970-01-01).
constexpr u64 FILETIME_EPOCH_DIFF_US = 11644473600ULL * 1000000ULL;

u64 FileTimeToRtcTick(const FILETIME& ft) {
    u64 ft100ns = (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // Convert 100-ns to microseconds, subtract epoch difference
    u64 us = ft100ns / 10;
    if (us >= FILETIME_EPOCH_DIFF_US) {
        return us - FILETIME_EPOCH_DIFF_US;
    }
    return us;
}

void WriteRtcDateTime(guest_addr_t ptr, const SYSTEMTIME& st) {
    // SceRtcDateTime layout: u16 year, u16 month, u16 day, u16 hour,
    //                         u16 minute, u16 second, u32 microsecond
    if (!ptr) return;
    Memory::Write<u16>(ptr + 0,  st.wYear);
    Memory::Write<u16>(ptr + 2,  st.wMonth);
    Memory::Write<u16>(ptr + 4,  st.wDay);
    Memory::Write<u16>(ptr + 6,  st.wHour);
    Memory::Write<u16>(ptr + 8,  st.wMinute);
    Memory::Write<u16>(ptr + 10, st.wSecond);
    Memory::Write<u32>(ptr + 12, static_cast<u32>(st.wMilliseconds) * 1000);
}

constexpr u64 ORBIS_OK = 0;
constexpr u64 ORBIS_RTC_ERROR_INVALID_POINTER = 0x80251001;
constexpr u64 ORBIS_RTC_ERROR_INVALID_VALUE   = 0x80251002;

} // namespace

void RegisterLibRtc() {
    LOG_INFO(HLE, "Registering libSceRtc HLE symbols...");

    // sceRtcGetCurrentTick — fills SceRtcTick (u64 microseconds since epoch).
    auto GetCurrentTickImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t tick_ptr = args.arg1;
        if (!tick_ptr) return ORBIS_RTC_ERROR_INVALID_POINTER;
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const u64 tick = FileTimeToRtcTick(ft);
        Memory::Write<u64>(tick_ptr, tick);
        LOG_DEBUG(HLE, "sceRtcGetCurrentTick(0x%llx) -> %llu us", tick_ptr, tick);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcGetCurrentTick",     GetCurrentTickImpl);
    RegisterSymbol("libSceRtc", "sceRtcGetCurrentTick#T#T", GetCurrentTickImpl);

    // sceRtcGetCurrentClockLocalTime — fills SceRtcDateTime with local time.
    auto GetCurrentClockLocalImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dt_ptr = args.arg1;
        if (!dt_ptr) return ORBIS_RTC_ERROR_INVALID_POINTER;
        SYSTEMTIME st;
        GetLocalTime(&st);
        WriteRtcDateTime(dt_ptr, st);
        LOG_DEBUG(HLE, "sceRtcGetCurrentClockLocalTime(0x%llx) -> %04d-%02d-%02d %02d:%02d:%02d",
                  dt_ptr, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcGetCurrentClockLocalTime",     GetCurrentClockLocalImpl);
    RegisterSymbol("libSceRtc", "sceRtcGetCurrentClockLocalTime#T#T", GetCurrentClockLocalImpl);

    // sceRtcGetCurrentClockUtc — fills SceRtcDateTime with UTC time.
    auto GetCurrentClockUtcImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dt_ptr = args.arg1;
        if (!dt_ptr) return ORBIS_RTC_ERROR_INVALID_POINTER;
        SYSTEMTIME st;
        GetSystemTime(&st);
        WriteRtcDateTime(dt_ptr, st);
        LOG_DEBUG(HLE, "sceRtcGetCurrentClockUtc(0x%llx) -> %04d-%02d-%02d %02d:%02d:%02d UTC",
                  dt_ptr, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcGetCurrentClockUtc",     GetCurrentClockUtcImpl);
    RegisterSymbol("libSceRtc", "sceRtcGetCurrentClockUtc#T#T", GetCurrentClockUtcImpl);

    // sceRtcCheckValid — always valid (return 0).
    auto CheckValidImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceRtcCheckValid(0x%llx) -> 0 (always valid)", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcCheckValid",     CheckValidImpl);
    RegisterSymbol("libSceRtc", "sceRtcCheckValid#T#T", CheckValidImpl);

    // sceRtcTickAddSeconds / sceRtcTickAddMinutes / sceRtcTickAddHours / sceRtcTickAddDays
    // Prototype: int sceRtcTickAdd*(SceRtcTick* pDst, const SceRtcTick* pSrc, s64 delta)
    auto TickAddSecondsImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dst = args.arg1, src = args.arg2;
        const s64 delta = static_cast<s64>(args.arg3);
        if (!dst || !src) return ORBIS_RTC_ERROR_INVALID_POINTER;
        const u64 base = Memory::Read<u64>(src);
        Memory::Write<u64>(dst, base + static_cast<u64>(delta) * 1000000ULL);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcTickAddSeconds",     TickAddSecondsImpl);
    RegisterSymbol("libSceRtc", "sceRtcTickAddSeconds#T#T", TickAddSecondsImpl);

    auto TickAddMinutesImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dst = args.arg1, src = args.arg2;
        const s64 delta = static_cast<s64>(args.arg3);
        if (!dst || !src) return ORBIS_RTC_ERROR_INVALID_POINTER;
        const u64 base = Memory::Read<u64>(src);
        Memory::Write<u64>(dst, base + static_cast<u64>(delta) * 60ULL * 1000000ULL);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcTickAddMinutes",     TickAddMinutesImpl);
    RegisterSymbol("libSceRtc", "sceRtcTickAddMinutes#T#T", TickAddMinutesImpl);

    auto TickAddHoursImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dst = args.arg1, src = args.arg2;
        const s64 delta = static_cast<s64>(args.arg3);
        if (!dst || !src) return ORBIS_RTC_ERROR_INVALID_POINTER;
        const u64 base = Memory::Read<u64>(src);
        Memory::Write<u64>(dst, base + static_cast<u64>(delta) * 3600ULL * 1000000ULL);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcTickAddHours",     TickAddHoursImpl);
    RegisterSymbol("libSceRtc", "sceRtcTickAddHours#T#T", TickAddHoursImpl);

    auto TickAddDaysImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dst = args.arg1, src = args.arg2;
        const s64 delta = static_cast<s64>(args.arg3);
        if (!dst || !src) return ORBIS_RTC_ERROR_INVALID_POINTER;
        const u64 base = Memory::Read<u64>(src);
        Memory::Write<u64>(dst, base + static_cast<u64>(delta) * 86400ULL * 1000000ULL);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcTickAddDays",     TickAddDaysImpl);
    RegisterSymbol("libSceRtc", "sceRtcTickAddDays#T#T", TickAddDaysImpl);

    auto TickAddMsecsImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dst = args.arg1, src = args.arg2;
        const s64 delta = static_cast<s64>(args.arg3);
        if (!dst || !src) return ORBIS_RTC_ERROR_INVALID_POINTER;
        const u64 base = Memory::Read<u64>(src);
        Memory::Write<u64>(dst, base + static_cast<u64>(delta) * 1000ULL);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcTickAddMicroseconds",     TickAddMsecsImpl);
    RegisterSymbol("libSceRtc", "sceRtcTickAddMicroseconds#T#T", TickAddMsecsImpl);
    RegisterSymbol("libSceRtc", "sceRtcTickAddTicks",     TickAddMsecsImpl);
    RegisterSymbol("libSceRtc", "sceRtcTickAddTicks#T#T", TickAddMsecsImpl);

    // sceRtcConvertLocalTimeToUtc — identity (no TZ offset emulated).
    auto ConvertLocalToUtcImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dst = args.arg1, src = args.arg2;
        if (!dst || !src) return ORBIS_RTC_ERROR_INVALID_POINTER;
        Memory::Write<u64>(dst, Memory::Read<u64>(src));
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcConvertLocalTimeToUtc",     ConvertLocalToUtcImpl);
    RegisterSymbol("libSceRtc", "sceRtcConvertLocalTimeToUtc#T#T", ConvertLocalToUtcImpl);

    // sceRtcConvertUtcToLocalTime — also identity.
    RegisterSymbol("libSceRtc", "sceRtcConvertUtcToLocalTime",     ConvertLocalToUtcImpl);
    RegisterSymbol("libSceRtc", "sceRtcConvertUtcToLocalTime#T#T", ConvertLocalToUtcImpl);

    // sceRtcGetTick — convert SceRtcDateTime -> SceRtcTick.
    auto GetTickImpl = [](const GuestArgs& args) -> u64 {
        // We don't do a full calendar conversion; return current tick instead.
        const guest_addr_t tick_ptr = args.arg2;
        if (!tick_ptr) return ORBIS_RTC_ERROR_INVALID_POINTER;
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        Memory::Write<u64>(tick_ptr, FileTimeToRtcTick(ft));
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcGetTick",     GetTickImpl);
    RegisterSymbol("libSceRtc", "sceRtcGetTick#T#T", GetTickImpl);

    // sceRtcSetTick — sets a SceRtcDateTime from a tick value (no-op; return 0).
    auto SetTickImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dt_ptr = args.arg1;
        if (!dt_ptr) return ORBIS_RTC_ERROR_INVALID_POINTER;
        // Write current local time as approximation
        SYSTEMTIME st;
        GetLocalTime(&st);
        WriteRtcDateTime(dt_ptr, st);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRtc", "sceRtcSetTick",     SetTickImpl);
    RegisterSymbol("libSceRtc", "sceRtcSetTick#T#T", SetTickImpl);

    // sceRtcCompareTick — compare two ticks; return -1/0/1.
    auto CompareTickImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t a_ptr = args.arg1, b_ptr = args.arg2;
        if (!a_ptr || !b_ptr) return static_cast<u64>(-1LL);
        const u64 a = Memory::Read<u64>(a_ptr);
        const u64 b = Memory::Read<u64>(b_ptr);
        if (a < b) return static_cast<u64>(-1LL);
        if (a > b) return 1;
        return 0;
    };
    RegisterSymbol("libSceRtc", "sceRtcCompareTick",     CompareTickImpl);
    RegisterSymbol("libSceRtc", "sceRtcCompareTick#T#T", CompareTickImpl);
}

} // namespace HLE
