// Unity IL2CPP: the managed file API (.NET's System.IO) does not go through nn::fs.
//
// IL2CPP carries its own PAL inside the main module: System.IO.File.Exists ends up in Interop.Sys.Stat, which converts
// the path to UTF-8 and calls SystemNative_Stat (main, native), which calls the SDK's libc stat -- and that libc opens
// its own fsp-srv IPC session, which the host does not implement. Nothing of it passes our nn::fs hooks, so the guest
// sees 「file not found」 for files that are right there in the RomFS (Ace Attorney Investigations Collection stops at
// Addressables' settings.json: "Invalid path in TextDataProvider").
//
// This replaces the entry of the two PAL byte-path stat wrappers with a branch into the host, which answers from the
// same mount table the nn::fs hooks use. The managed FileStatus structure is written here directly (layout read off
// the PAL's own conversion code: Flags +0x00, Mode +0x04, Uid +0x08, Gid +0x0C, Size +0x10, ATime +0x18,
// ATimeNsec +0x20, MTime +0x28, MTimeNsec +0x30, CTime +0x38, CTimeNsec +0x40, BirthTime +0x48, BirthTimeNsec +0x50,
// Dev +0x58, Ino +0x60, RDev +0x68).
#include "dm.h"
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
int dm_fs_guest_path(const char *p, char *out, size_t n, int *ro);

// FileStatus, as the PAL's own conversion of a native stat fills it
typedef struct { int32_t flags, mode; uint32_t uid, gid; int64_t size, atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns, birth, birth_ns, dev, ino, rdev; } FileStatus;

static int fill_status(const char *path, FileStatus *out, int follow) {
    char hp[1024]; int ro = 0;
    if (!dm_fs_guest_path(path, hp, sizeof hp, &ro)) { errno = ENOENT; return -1; }
    struct stat st;
    int r = follow ? stat(hp, &st) : lstat(hp, &st);
    if (r) return -1;
    memset(out, 0, sizeof *out);
    out->mode = (int32_t)(st.st_mode & 0xFFFF);
    out->uid = st.st_uid; out->gid = st.st_gid;
    out->size = st.st_size;
    out->atime = st.st_atimespec.tv_sec; out->atime_ns = st.st_atimespec.tv_nsec;
    out->mtime = st.st_mtimespec.tv_sec; out->mtime_ns = st.st_mtimespec.tv_nsec;
    out->ctime = st.st_ctimespec.tv_sec; out->ctime_ns = st.st_ctimespec.tv_nsec;
    out->birth = st.st_birthtimespec.tv_sec; out->birth_ns = st.st_birthtimespec.tv_nsec;
    out->dev = st.st_dev; out->ino = st.st_ino;
    return 0;
}

// Offsets inside the main module of the PAL wrappers to replace; per main build (the rewriter records the build id).
static const struct { const char *build; uint32_t stat_byte, lstat_byte, stat_pal, lstat_pal; } k_pal[] = {
    { "d323b2484ad188863010dc0759ac2aeb", 0x321f4c0, 0x321f610, 0x3e4830, 0x3e4a10 },   // Ace Attorney Investigations Collection 1.0.0
};
// 同一个标题的 sdk 模块：nn::crypto / nn::os 的随机数在真机上开 csrng 会话（IPC），宿主没有那个服务，
// SDK 里 internal 的调用会走到 std::terminate → Break（存档加密 SaveEncrypt → AES → GenerateIV → RNGCryptoServiceProvider）。
// 把 sdk 模块里这两个函数本体直接换成宿主实现，import 调用和 sdk 内部调用都覆盖。
static const struct { const char *build; uint32_t crypto_random, os_random, system_clock_now; } k_sdk_rng[] = {
    { "311deab1f062b504f3d646a49ddcd4e8", 0x97120, 0x133110, 0x550494 },   // sdk of Ace Attorney Investigations Collection 1.0.0
};
// std::chrono::system_clock::now()（SDK 的实现走 nn::time 的 IPC，宿主没有那个服务，失败就 std::terminate → Break；
// .NET 的 RNGCryptoServiceProvider 构造、AES 存档加密都会先取时间）。返回宿主墙钟的纳秒数，与宿主的 time HLE 同一口径。
static int64_t h_system_clock_now(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
static void h_random_fill(void *p, size_t n) {
    extern uint64_t dm_entropy64(void); extern int dm_detclock_on(void);
    if (!dm_detclock_on()) { arc4random_buf(p, n); return; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = dm_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
}
// The PAL entries themselves (Interop.Sys.Stat's ReadOnlySpan overload calls them directly with a UTF-8 C string)
static int h_pal_stat (uint64_t path, FileStatus *out, uint64_t mi) { (void)mi; return fill_status((const char *)(uintptr_t)path, out, 1); }
static int h_pal_lstat(uint64_t path, FileStatus *out, uint64_t mi) { (void)mi; return fill_status((const char *)(uintptr_t)path, out, 0); }

static void write_branch(uint8_t *at, void *fn) {
    // Called by the loader while the module image is still writable (before its text is made read-execute)
    uint32_t code[2] = { 0x58000050u /* ldr x16, #8 */, 0xD61F0200u /* br x16 */ };
    memcpy(at, code, 8); memcpy(at + 8, &fn, 8);
    __builtin___clear_cache((char *)at, (char *)at + 16);
}

// (path utf8, FileStatus* out, MethodInfo*) -> int; the Byte*Path overloads of Interop.Sys.Stat / LStat
static int h_il2cpp_stat (uint64_t path, FileStatus *out, uint64_t mi) { (void)mi; return fill_status((const char *)(uintptr_t)path, out, 1); }
static int h_il2cpp_lstat(uint64_t path, FileStatus *out, uint64_t mi) { (void)mi; return fill_status((const char *)(uintptr_t)path, out, 0); }

void dm_il2cpp_file_patch(module_t *m) {   // loader.c, right before the module's text becomes read-execute
    if (!strcmp(m->name, "sdk")) {
        for (unsigned i = 0; i < sizeof k_sdk_rng / sizeof k_sdk_rng[0]; i++) {
            if (strncmp(k_sdk_rng[i].build, m->build_id, 32)) continue;
            write_branch((uint8_t *)m->base + k_sdk_rng[i].crypto_random, (void *)h_random_fill);
            write_branch((uint8_t *)m->base + k_sdk_rng[i].os_random, (void *)h_random_fill);
            write_branch((uint8_t *)m->base + k_sdk_rng[i].system_clock_now, (void *)h_system_clock_now);
            LOG("补丁：sdk 的 nn::crypto / nn::os 随机数走宿主（+%#x / +%#x）", k_sdk_rng[i].crypto_random, k_sdk_rng[i].os_random);
            return;
        }
        return;
    }
    if (strcmp(m->name, "main") || m->size <= 0x321f4c0 + 16) return;
    if (getenv("DM_IL2CPP_FILE") && !strcmp(getenv("DM_IL2CPP_FILE"), "0")) return;
    const char *build = m->build_id;
    for (unsigned i = 0; i < sizeof k_pal / sizeof k_pal[0]; i++) {
        if (strncmp(k_pal[i].build, build, 32)) continue;
        write_branch((uint8_t *)m->base + k_pal[i].stat_byte, (void *)h_il2cpp_stat);
        write_branch((uint8_t *)m->base + k_pal[i].lstat_byte, (void *)h_il2cpp_lstat);
        write_branch((uint8_t *)m->base + k_pal[i].stat_pal, (void *)h_pal_stat);
        write_branch((uint8_t *)m->base + k_pal[i].lstat_pal, (void *)h_pal_lstat);
        LOG("IL2CPP 补丁：托管侧 stat/lstat 走宿主挂载表（main+%#x / +%#x）", k_pal[i].stat_byte, k_pal[i].lstat_byte);
        return;
    }
}
