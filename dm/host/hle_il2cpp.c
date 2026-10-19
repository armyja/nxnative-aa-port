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
