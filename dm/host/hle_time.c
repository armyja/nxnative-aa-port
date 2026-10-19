// nn::time: a real device gets the network-calibrated user clock through the time service; the host reads the system clock directly (single machine, offline).
// Layout follows the public Horizon ABI: PosixTime = int64 seconds; CalendarTime = {s16 year, s8 month/day/hour/minute/second};
// CalendarAdditionalInfo = {u32 day of week, u32 day of year, char time zone name[8], u32 DST, s32 UTC offset in seconds}.
#include "hle.h"
#include <stdatomic.h>
#include <time.h>
#include <sys/time.h>

typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;

// DM_FIXED_TIME=seconds (for deterministic comparison; the ground-truth side counterpart is dm/verify/ryu/drive.py --fix-time):
// ① the user clock (source of the save's d2s +0x20 last-played time and .meta last_modified_time) always returns this value;
// ② the one time() at the game-creation random seed (main 0x3509a0 bl time, return point 0x3509a4, same point as drive.py) returns this value.
// This SDK's libc time() is uptime (clock_gettime(0) → __nnmusl_ClockGetTime → GetSystemTick), not wall clock;
// the other 17 time() call sites in main are left alone and forwarded unchanged to the SDK's own time().
static int64_t fixed_time(void) {
    static int64_t v = -2;
    if (v == -2) { const char *e = getenv("DM_FIXED_TIME"); v = e ? strtoll(e, 0, 0) : -1;
                   if (v >= 0) LOG("DM_FIXED_TIME=%lld：用户时钟与建局 time()（main 0x3509a4）返回定值", (long long)v); }
    return v;
}
HLE_FORCE("time", "按返回点认出建局种子那一处（DM_FIXED_TIME）；其余原样转调 SDK 的 time()", int64_t, h_time_time, int64_t *t) {
    uint64_t lr = (uint64_t)__builtin_return_address(0);
    static int64_t (*sdk_time)(int64_t *); static uint64_t seed_ret;
    if (!sdk_time) {
        module_t *ms = NULL, *mm = NULL;
        for (int i = 0; i < g_nmod; i++) { if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i]; if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i]; }
        void *f = ms ? module_sym(ms, "time") : NULL; if (!f) die("SDK 里找不到 time");
        uint32_t w = mm && !strcmp(mm->build_id, DM_D2R_BUILD) ? *(uint32_t *)(mm->base + 0x3509a0) : 0;   // read only on the D2R build (elsewhere this is unrelated code, and a main smaller than 3.3 MB would be read past its end)
        if ((w & 0xfc000000) == 0x94000000) seed_ret = (uint64_t)mm->base + 0x3509a4;               // bl time (build db0507f3…; the rewriter changes the branch target, so only the bl itself is matched)
        else if (fixed_time() >= 0) LOG("【红】DM_FIXED_TIME：main 0x3509a0 不是预期的 bl time（%#x），建局 time() 未固定", w);
        sdk_time = (int64_t (*)(int64_t *))f;
    }
    if (getenv("DM_TIME_WALL")) { int64_t w = (int64_t)time(NULL); if (t) *t = w; return w; }   // experiment: wall clock instead of the SDK uptime
    int64_t ft = fixed_time();
    if (ft >= 0 && lr == seed_ret) {
        static int once; if (!once++) LOG("DM_FIXED_TIME：建局 time() 返回 %lld", (long long)ft);
        if (t) *t = ft; return ft;
    }
    return sdk_time(t);
}

HLE_FORCE("_ZN2nn4time10InitializeEv", "连 time 服务；默认直接 OK（宿主接管时钟），DM_TIME_REALINIT=1 时转调 SDK 真实初始化", Result, h_time_Initialize, void) {
    if (!getenv("DM_TIME_REALINIT")) return RES_OK;
    // .NET（DateTime.Now）走的是 SDK 内部的时间服务对象，它只在真实 Initialize 里被创建；
    // 我们直接返回 OK 会让那个对象一直为 null → guest 时钟恒为 0 → 存档日期显示 0001/01/01。
    static Result (*real)(void);
    if (!real) {
        module_t *ms = NULL;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i];
        if (ms) real = (Result (*)(void))module_sym(ms, "_ZN2nn4time10InitializeEv");
    }
    return real ? real() : RES_OK;
}
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "time 服务已被接管，恒为已初始化", int, h_time_IsInitialized, void) { return 1; }
// Steady clock (seconds since boot + a clock source ID, a 16-byte UUID): the host's monotonic seconds with a fixed source ID. GetSpanBetween runs as is in sdk: it subtracts only when the source IDs match
HLE_FORCE("_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", "稳定时钟 → 宿主单调时钟（秒），源号固定", Result, h_time_SteadyNow, uint8_t *out) {
    int64_t s = (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000ull);
    memcpy(out, &s, 8); memcpy(out + 8, "dmhost-steady-01", 16); return RES_OK;
}
// 时钟上下文（时区偏移 + 时间点）：不接管的话 SDK 会去连 time 服务的 IPC，失败后返回全 0 的上下文，
// 存档界面就把时间显示成 0001-01-01（御剑精选集的存档时间戳就是从这里算的）。宿主按本地时区给出。
typedef struct { int64_t offset; int64_t time_point; uint8_t source_id[16]; } SystemClockContextDM;
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "用户时钟上下文 → 宿主本地时区", Result, h_time_GetContext, void *out) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    SystemClockContextDM c = { .offset = (int64_t)tm.tm_gmtoff, .time_point = (int64_t)t, .source_id = {0} };
    if (out) memcpy(out, &c, sizeof c);
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "用户时钟 → 宿主 time()", Result, h_time_GetCurrentTime, int64_t *out) {
    int64_t ft = fixed_time(); *out = ft >= 0 ? ft : time(NULL);
    if (getenv("DM_TIMELOG")) { static int n; if (n++ < 8) LOG("timelog GetCurrentTime → %lld", (long long)*out); }
    return RES_OK;
}
// Forced Terror Zone for stages 6–10 of the boss rush (set by dm_bossrush.c): when g_dm_tz_time is non-zero and not expired, only the clock reads at the "Terror Zone" sites get it; save timestamps and all other calls are unchanged.
// Which sites: the return point falls in the cluster starting at the zone function 0x58f1e0 (0x58f1e0–0x591440), or the zone schedule 0x58f310 is called within 8 instructions after the return (server side 0x636b38 applying the zone, 0x694b48 and so on, 9 sites).
// Expiry: dm_bossrush renews it by 1 second every tick; once this run stops ticking (exit / crash) it no longer applies, and the next run gets the real clock when the game is created.
_Atomic int64_t g_dm_tz_time;
_Atomic uint64_t g_dm_tz_live_ns;
static int tz_site(uint64_t lr) {   // Checked only while forcing is active, reading the 8 instructions after the return point each time, no caching (several threads read the clock)
    static _Atomic uint64_t base; uint64_t b = atomic_load(&base);
    if (!b) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; atomic_store(&base, b); }
    if (!b || lr < b) return 0;
    uint64_t off = lr - b; if (off > 0x58f1e0 && off < 0x591440) return 1;
    for (int k = 0; k < 8; k++) { uint32_t w = *(const uint32_t *)(uintptr_t)(lr + 4 * k);
        if ((w & 0xfc000000) == 0x94000000) { int64_t imm = w & 0x3ffffff; if (imm & 0x2000000) imm -= 1 << 26; if (off + 4 * k + imm * 4 == 0x58f310) return 1; } }
    return 0;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock3nowEv", "用户时钟 → 宿主 time()；首领连战强制地带时恐怖地带那几处另返回", int64_t, h_time_now, void) {
    int64_t tz = atomic_load(&g_dm_tz_time);
    if (tz && (uint64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC) < atomic_load(&g_dm_tz_live_ns) && tz_site((uint64_t)__builtin_return_address(0))) return tz;
    int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL); }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE",
          "SDK 实现与 now() 配套，一并接管", int64_t, h_time_to_time_t, const int64_t *tp) { return *tp; }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "连 time 服务的时区规则；改用宿主 localtime_r", Result, h_time_ToCalendarTime, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt) {
    time_t t = *pt; struct tm tm; localtime_r(&t, &tm);
    static int once; if (!once++) LOG("ToCalendarTime：首次换算 %lld → %04d-%02d-%02d %02d:%02d（时区 %s，偏移 %ld 秒）", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
// Guest libc gettimeofday: the SDK's implementation has no RTC and returns 0 (or uptime), which made AA456's RE Engine store 0 as the save time and
// show 01/01/1970 in every slot. The host returns its own wall clock (the guest struct timeval is two 64-bit fields on this ABI).
HLE_FORCE("gettimeofday", "墙钟：宿主 gettimeofday", int, h_gettimeofday, void *tv, void *tz) {
    (void)tz;
    struct timeval t; gettimeofday(&t, NULL);
    if (tv) { int64_t v[2] = { (int64_t)t.tv_sec, (int64_t)t.tv_usec }; memcpy(tv, v, sizeof v); }
    if (getenv("DM_TIMELOG")) { static int n; if (n++ < 8) LOG("timelog gettimeofday → %lld.%06lld", (long long)t.tv_sec, (long long)t.tv_usec); }
    return 0;
}
// The 4-argument overload with an explicit TimeZoneRule (AA456 imports this one): the rule comes from the guest's location settings and our host has
// none, so the same host localtime_r conversion is used. Without this hook the SDK's own implementation ran and left the calendar empty, which made
// every save slot show 01/01/1970.
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE",
          "带时区规则版的 ToCalendarTime：同样用宿主 localtime_r", Result, h_time_ToCalendarTimeRule,
          CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt, const void *rule) {
    (void)rule;
    time_t t = *pt; struct tm tm; localtime_r(&t, &tm);
    static int once; if (!once++) LOG("ToCalendarTime(规则版)：首次换算 %lld → %04d-%02d-%02d %02d:%02d（时区 %s，偏移 %ld 秒）", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
// The reverse: local calendar → absolute seconds (13 Sentinels). Around a daylight-saving switch one local time maps to 0 or 2 absolute seconds; the host's mktime gives one
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE",
          "连 time 服务的时区规则；改用宿主 mktime", Result, h_time_ToPosixTime, int *count, int64_t *out, int n, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second, .tm_isdst = -1};
    time_t t = mktime(&tm);
    if (count) *count = n > 0;
    if (n > 0 && out) out[0] = (int64_t)t;
    return RES_OK;
}
// Local time conversion is forced onto the host libc: the SDK's own localtime / localtime_r / mktime have no time zone data and always use UTC (measured on the host: the chronicle's 「首次发现」(First Discovered) is produced by
// main+0xb04d34 calling localtime and then strftime, and was off by 8 hours). The SDK on a real Switch converts by the system time zone, so only following the host system time zone here is consistent;
// the host TZ normally = the system time zone, and is pinned to UTC only for deterministic comparison with DM_FIXED_TIME (main.c). The struct tm layout is the same on both sides (9 ints, long offset, time zone name pointer).
// gmtime / gmtime_r / timegm are UTC anyway and are not taken over. The same-named pass-through entries in hle_table.c only take effect when the symbol has no definition (the SDK defines them), hence HLE_FORCE
HLE_FORCE("localtime", "本地时刻 → 宿主 localtime（跟系统时区）", struct tm *, h_localtime, const int64_t *t) { time_t v = (time_t)*t; return localtime(&v); }
HLE_FORCE("localtime_r", "本地时刻 → 宿主 localtime_r（跟系统时区）", struct tm *, h_localtime_r, const int64_t *t, struct tm *out) { time_t v = (time_t)*t; return localtime_r(&v, out); }
HLE_FORCE("mktime", "本地日历 → 绝对秒，宿主 mktime（跟系统时区）", int64_t, h_mktime, struct tm *tm) { return (int64_t)mktime(tm); }
