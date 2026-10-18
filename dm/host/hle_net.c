// Networking (out of scope: online play/friends/store/cloud saves): all hooked up as "network unavailable", the game takes the offline branch.
// Cuts: nifm reports no network; socket only hooks up Initialize (returns success, creates no session), the rest of socket/ssl/friends are hooked up as failures when they come into use.
#include "hle.h"
#include <sys/mman.h>

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "连 bsd 套接字服务；离线，不建会话（剪）", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
// Ace Attorney 123 (SDK 5.5): with the privacy policy accepted, the save flow waits for Unity's network-platform
// init completion, which the game only publishes from an IL2CPP callback that never runs under the host
// (main+0xe1e640: signals the event the NetworkInterfaceWrapper thread parks on, then clears the flag). Without it
// "Saving..." hangs and no file is ever written. The init flag is set by the game right after nifm::Initialize
// returns (byte main+0x29c6c38); once it appears we publish the completion ourselves.
static void aa_save_msg_gate(const uint64_t *regs) {
    if (!getenv("DM_AALOG")) return;
    // the gate stub lives in the patch area, so regs[30] is inside the stub; the real caller's return address is
    // the first frame's [x29 + 8] (patch_nso's convention), and the x29 chain gives the callers above it
    uint64_t fp = regs[29], chain[8] = {0}; int n = 0;
    for (int i = 0; i < 8 && fp >= 0x1000; i++) { uint64_t nxt = *(volatile uint64_t *)fp, ret = *(volatile uint64_t *)(fp + 8); chain[n++] = ret; if (nxt <= fp) break; fp = nxt; }
    extern uint64_t dm_guest_frame(void);
    LOG("[aa] 消息设置 帧%llu id=%#llx caller=%#llx 链=%#llx,%#llx,%#llx,%#llx,%#llx", (unsigned long long)dm_guest_frame(), (unsigned long long)regs[1],
        (unsigned long long)chain[0], (unsigned long long)chain[0], (unsigned long long)chain[1], (unsigned long long)chain[2], (unsigned long long)chain[3], (unsigned long long)chain[4]);
}
static void aa_save_gate(const uint64_t *regs) {
    if (!getenv("DM_AALOG")) return;
    uint64_t fp = regs[29], chain[8] = {0}; int n = 0;
    for (int i = 0; i < 8 && fp >= 0x1000; i++) { uint64_t nxt = *(volatile uint64_t *)fp, ret = *(volatile uint64_t *)(fp + 8); chain[n++] = ret; if (nxt <= fp) break; fp = nxt; }
    LOG("[aa] 存档方法 id=%#llx x0=%#llx x1=%#llx x2=%#llx caller=%#llx 链=%#llx,%#llx,%#llx", (unsigned long long)regs[1] /*unused*/,
        (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2],
        (unsigned long long)chain[0], (unsigned long long)chain[0], (unsigned long long)chain[1], (unsigned long long)chain[2]);
}
static void aa_gate_save_game_req(const uint64_t *r) { LOG("[aa] SaveGameDataRequest"); aa_save_gate(r); }
static void aa_gate_save_sys_req(const uint64_t *r)  { LOG("[aa] SaveSystemDataRequest"); aa_save_gate(r); }
static void aa_gate_save_create_req(const uint64_t *r) { LOG("[aa] SaveCreateSystemDataRequest"); aa_save_gate(r); }
static void aa_gate_save(const uint64_t *r) { LOG("[aa] Save"); aa_save_gate(r); }
static void aa_gate_ondece(const uint64_t *r) { LOG("[aa] OnDecide"); aa_save_gate(r); }
static void aa_gate_msgset(const uint64_t *r) { LOG("[aa] Mess_window_set"); aa_save_gate(r); }
// Text.set_text(this, value): the value is an Il2CppString (length at +0x10, UTF-16 chars at +0x14). Only log strings
// that contain 存档 / 保存 / 电源 -- the save dialog's text -- so the log stays small.
static void aa_gate_save_game2(const uint64_t *r) { LOG("[aa] SaveGameData"); aa_save_gate(r); }
static void aa_gate_tempdata(const uint64_t *r) { LOG("[aa] tempDataSet"); aa_save_gate(r); }
static void aa_gate_issave(const uint64_t *r) { LOG("[aa] get_is_save_"); aa_save_gate(r); }
static void aa_gate_uiconf(const uint64_t *r) { LOG("[aa] SaveLoadUICtrl.SaveConfirmation"); aa_save_gate(r); }
static void aa_gate_corosave(const uint64_t *r) { LOG("[aa] SaveConfirmationWindow.CoroutineSave"); aa_save_gate(r); }
static void aa_gate_corosaveanim(const uint64_t *r) { LOG("[aa] CoroutineSaveAnim"); aa_save_gate(r); }
static module_t *aa_main_module(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return &g_mod[i];
    return NULL;
}
static volatile uint64_t g_aa_save_window;   // SaveConfirmationWindow instance of the open dialog
static volatile int g_aa_save_slot = -1;      // slot it was opened for
static void aa_gate_openwin(const uint64_t *r) {
    LOG("[aa] SaveConfirmationWindow.OpenWindow");
    g_aa_save_window = r[0];
    g_aa_save_slot = (int)*(volatile int32_t *)(uintptr_t)(r[0] + 0x20);   // OpenWindow stores the slot at +0x20
    extern uint64_t dm_guest_frame(void);
    LOG("[aa] 存档对话框打开：window=%#llx slot=%d 帧%llu", (unsigned long long)r[0], g_aa_save_slot, (unsigned long long)dm_guest_frame());
    aa_save_gate(r);
}
static uint64_t g_aa_release_b_at, g_aa_refresh_at;
// Host-side save trigger: run the game's own SaveGameDataRequest for the slot the dialog was opened for. The save UI's
// coroutine never resumes on the host, so without this the dialog stays forever and nothing is written.
void aa_host_save_request(void) {
    static int busy;
    if (busy) return;
    module_t *m = aa_main_module();
    if (!m || strcmp(m->build_id, "c5b5d28842207c47bd0fc97137e5e75f")) return;
    int slot = g_aa_save_slot;
    if (slot < 0) slot = 0;
    busy = 1;
    LOG("宿主触发存档：SaveGameDataRequest(slot=%d)（游戏内存档代码）", slot);
    ((void (*)(uint64_t, uint64_t))(m->base + 0x3348e0))(0, (uint64_t)(uint32_t)slot);
    LOG("宿主触发存档：返回（应已写盘并提交）");
    busy = 0;
    // Press B on the scripted input channel for a few frames: the game's own handler closes the save dialog (its
    // coroutine never resumes, so it cannot close itself)
    extern void dm_script_set(uint64_t, int32_t, int32_t, int32_t, int32_t);
    extern uint64_t dm_guest_frame(void);
    if (!getenv("DM_AA_NO_AUTOB")) { dm_script_set(0x2, 0, 0, 0, 0); g_aa_release_b_at = dm_guest_frame() + 4; }
    // Optional: dismiss the dialog ourselves (the save coroutine that would normally hide it never resumes)
    const char *ce = getenv("DM_AA_AUTOCLOSE");
    if (ce && atoi(ce) && g_aa_save_window) {
        uint64_t inst = ((uint64_t (*)(void))(m->base + 0x337b40))();   // SaveLoadUICtrl.get_instance()
        LOG("宿主关闭存档对话框：UICtrl=%#llx", (unsigned long long)inst);
        if (inst) ((void (*)(uint64_t))(m->base + 0x33a2f0))(inst);     // SaveLoadUICtrl.Close()
        LOG("宿主关闭存档对话框：返回");
    }
}
static void aa_gate_loadplay(const uint64_t *r) { LOG("[aa] LoadingPlay"); aa_save_gate(r); }
static void aa_gate_loadwait(const uint64_t *r) { LOG("[aa] LoadingWait"); aa_save_gate(r); }
static void aa_gate_uissaveload(const uint64_t *r) { LOG("[aa] SaveLoadUICtrl.CoroutineSaveLoad"); aa_save_gate(r); }
// StartCoroutine(this, routine): log the iterator class when it is save related -- tells us whether the save
// coroutine (<CoroutineSave>c__Iterator4) is ever started at the press
extern uint64_t g_meta_buf;
// GameObject.SetActive(this, active): log only around the save press, so the log stays readable
static void aa_gate_save_branch(const uint64_t *r) {
    if (!getenv("DM_AALOG")) return;
    extern uint64_t dm_guest_frame(void);
    LOG("[aa] 存档协程分支：枚举=%lld（帧%llu this=%#llx）", (long long)(int32_t)r[8], (unsigned long long)dm_guest_frame(), (unsigned long long)r[19]);
}
static void aa_gate_setactive(const uint64_t *r) {
    if (!getenv("DM_AALOG")) return;
    extern uint64_t dm_guest_frame(void);
    uint64_t f = dm_guest_frame();
    if (f < 10150 || f > 10700) return;
    LOG("[aa] SetActive 帧%llu go=%#llx active=%d", (unsigned long long)f, (unsigned long long)r[0], (int)(r[1] & 1));
}
static void aa_gate_stopcoro(const uint64_t *r) {
    if (!getenv("DM_AALOG")) return;
    extern uint64_t dm_guest_frame(void);
    LOG("[aa] StopCoroutine 帧%llu this=%#llx arg=%#llx", (unsigned long long)dm_guest_frame(), (unsigned long long)r[0], (unsigned long long)r[1]);
}
static void aa_gate_stopall(const uint64_t *r) {
    if (!getenv("DM_AALOG")) return;
    extern uint64_t dm_guest_frame(void);
    LOG("[aa] StopAllCoroutines 帧%llu this=%#llx", (unsigned long long)dm_guest_frame(), (unsigned long long)r[0]);
}
// SaveControl.is_save_ (static fields + 6) at this instant: tells which branch the save coroutine takes
static int aa_is_save_flag(module_t *m) {
    if (!m) return -1;
    uint64_t v = *(volatile uint64_t *)(m->base + 0x283eca8);
    if (!v) return -1;
    uint64_t klass = *(volatile uint64_t *)(uintptr_t)v;
    if (!klass) return -1;
    uint64_t sf = *(volatile uint64_t *)(uintptr_t)(klass + 0xa0);
    if (!sf) return -1;
    return *(volatile uint8_t *)(uintptr_t)(sf + 6);
}
static volatile uint64_t g_aa_coro_iter;   // last <CoroutineSave> iterator seen (state polling below)
static void aa_gate_corosave_next(const uint64_t *r) {
    if (!getenv("DM_AALOG")) return;
    extern uint64_t dm_guest_frame(void);
    module_t *m = aa_main_module();
    LOG("[aa] CoroutineSave.MoveNext 帧%llu this=%#llx win=%#llx state=%d is_save=%d", (unsigned long long)dm_guest_frame(),
        (unsigned long long)r[0], (unsigned long long)*(volatile uint64_t *)(uintptr_t)(r[0] + 0x18),
        (int)*(volatile int32_t *)(uintptr_t)(r[0] + 0x2c), aa_is_save_flag(m));
    g_aa_coro_iter = r[0];
    aa_save_gate(r);
}
static void aa_gate_coroanim_next(const uint64_t *r) {
    if (!getenv("DM_AALOG")) return;
    extern uint64_t dm_guest_frame(void);
    LOG("[aa] CoroutineSaveAnim.MoveNext 帧%llu", (unsigned long long)dm_guest_frame());
}
static void aa_gate_startcoro(const uint64_t *r) {
    if (!getenv("DM_AALOG")) return;
    uint64_t it = r[1];
    if (!it || !g_meta_buf) return;
    uint64_t klass = *(volatile uint64_t *)(uintptr_t)it;
    if (!klass) return;
    uint64_t np = *(volatile uint64_t *)(uintptr_t)(klass + 0x10);
    uint64_t lo = g_meta_buf + 0x45e14, hi = lo + 0xe42b4;
    if (np < lo || np >= hi) return;
    char nm[96]; int i = 0;
    for (; i < 95; i++) { char c = *(volatile char *)(uintptr_t)(np + i); if (!c) break; nm[i] = c; }
    nm[i] = 0;
    if (!strstr(nm, "Save") && !strstr(nm, "save")) return;
    extern uint64_t dm_guest_frame(void);
    LOG("[aa] StartCoroutine 帧%llu owner=%#llx %s", (unsigned long long)dm_guest_frame(), (unsigned long long)r[0], nm);
    if (strstr(nm, "<CoroutineSave>")) g_aa_coro_iter = it;   // poll this iterator's state machine each frame
}
static void aa_gate_settext(const uint64_t *r) {
    if (!getenv("DM_AALOG")) return;
    const volatile uint8_t *sp = (const volatile uint8_t *)(uintptr_t)r[1];
    if (!sp) return;
    int32_t len = *(const volatile int32_t *)(sp + 0x10);
    if (len <= 0 || len > 400) return;
    const uint16_t *ch = (const uint16_t *)(sp + 0x14);
    int hit = 0; char utf8[256]; int n = 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = ch[i];
        if (c == 0x5B58 || c == 0x4FDD) hit = 1;         // 存 / 保
        if (n < (int)sizeof utf8 - 4) {
            if (c < 0x80) utf8[n++] = (char)c;
            else if (c < 0x800) { utf8[n++] = (char)(0xC0 | (c >> 6)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
            else { utf8[n++] = (char)(0xE0 | (c >> 12)); utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
        }
    }
    if (!hit) return;
    utf8[n] = 0;
    uint64_t fp = r[29], chain[6] = {0};
    for (int i = 0; i < 6 && fp >= 0x1000; i++) { uint64_t nxt = *(volatile uint64_t *)fp, ret = *(volatile uint64_t *)(fp + 8); chain[i] = ret; if (nxt <= fp) break; fp = nxt; }
    extern uint64_t dm_guest_frame(void);
    LOG("[aa] set_text 帧%llu \"%s\" 调用者=%#llx 链=%#llx,%#llx,%#llx", (unsigned long long)dm_guest_frame(), utf8,
        (unsigned long long)chain[0], (unsigned long long)chain[0], (unsigned long long)chain[1], (unsigned long long)chain[2]);
}
__attribute__((constructor)) static void aa_obs_gates(void) {
    gate_obs_set(OBS_AA_SAVE_MSG, aa_save_msg_gate);
    gate_obs_set(OBS_AA_SAVE_GAME_REQ, aa_gate_save_game_req);
    gate_obs_set(OBS_AA_SAVE_SYS_REQ, aa_gate_save_sys_req);
    gate_obs_set(OBS_AA_SAVE_CREATE_REQ, aa_gate_save_create_req);
    gate_obs_set(OBS_AA_SAVE, aa_gate_save);
    gate_obs_set(OBS_AA_ONDECIDE, aa_gate_ondece);
    gate_obs_set(OBS_AA_MSGSET, aa_gate_msgset);
    gate_obs_set(OBS_AA_SETTEXT, aa_gate_settext);
    gate_obs_set(OBS_AA_SAVE_GAME2, aa_gate_save_game2);
    gate_obs_set(OBS_AA_TEMPDATA, aa_gate_tempdata);
    gate_obs_set(OBS_AA_ISSAVE, aa_gate_issave);
    gate_obs_set(OBS_AA_UICONF, aa_gate_uiconf);
    gate_obs_set(OBS_AA_COROSAVE, aa_gate_corosave);
    gate_obs_set(OBS_AA_COROSAVEANIM, aa_gate_corosaveanim);
    gate_obs_set(OBS_AA_OPENWIN, aa_gate_openwin);
    gate_obs_set(OBS_AA_LOADPLAY, aa_gate_loadplay);
    gate_obs_set(OBS_AA_LOADWAIT, aa_gate_loadwait);
    gate_obs_set(OBS_AA_UI_SAVELOAD, aa_gate_uissaveload);
    gate_obs_set(OBS_AA_STARTCORO, aa_gate_startcoro);
    gate_obs_set(OBS_AA_COROSAVE_NEXT, aa_gate_corosave_next);
    gate_obs_set(OBS_AA_COROANIM_NEXT, aa_gate_coroanim_next);
    gate_obs_set(OBS_AA_SETACTIVE, aa_gate_setactive);
    gate_obs_set(OBS_AA_STOPCORO, aa_gate_stopcoro);
    gate_obs_set(OBS_AA_STOPALL, aa_gate_stopall);
    gate_obs_set(OBS_AA_SAVE_BRANCH, aa_gate_save_branch);
}
void aa_net_publish_init(void) {
    static int off = -1; if (off < 0) off = getenv("DM_NET_NOHOOK") != NULL; if (off) return;
    static int done;
    if (done) return;
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || strcmp(m->build_id, "c5b5d28842207c47bd0fc97137e5e75f")) return;   // only this build's addresses
    if (!*(volatile uint8_t *)(m->base + 0x29c6c38)) return;
    done = 1;
    LOG("网络平台初始化完成通知补发（main+0xe1e640）");
    ((void (*)(void))(m->base + 0xe1e640))();
}
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "连 nifm 网络管理服务；离线（剪）", Result, h_nifm_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "离线：网络请求不提交", void, h_nifm_Submit, void) { aa_net_publish_init(); }
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "离线：无请求可取消", void, h_nifm_Cancel, void) {}
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "离线：请求不在等待中", int, h_nifm_OnHold, void) { aa_net_publish_init(); return 0; }
// Probe: call the game's own C# save request from the host at a given frame (bypassing the stuck save menu), to tell
// "the menu never calls it" apart from "the save code itself is broken". DM_AA_CALL_SAVE=<frame>:<slot>
// DM_TEXT_WRITABLE keeps the guest text writable for a debugger: the loader's later mprotect calls put the text back
// to RX, so re-apply RWX periodically while debugging (harmless when no debugger is attached)
static void aa_keep_text_writable(void) {
    static int on = -1, tick;
    if (on < 0) on = getenv("DM_TEXT_WRITABLE") != NULL;
    if (!on) return;
    if (++tick % 60) return;
    module_t *m = aa_main_module();
    if (m) mprotect(m->base, 0x2afc000, PROT_READ | PROT_WRITE | PROT_EXEC);
}
void aa_call_save_probe(void) {
    aa_keep_text_writable();
    // Poll the save coroutine's state machine: if Unity ever calls MoveNext again the state changes; if it stays 1
    // forever, Unity really dropped the coroutine (that is the current situation)
    if (g_aa_coro_iter) {
        static int last = -99; extern uint64_t dm_guest_frame(void);
        int st = *(volatile int32_t *)(uintptr_t)(g_aa_coro_iter + 0x2c);
        int c28 = *(volatile uint8_t *)(uintptr_t)(g_aa_coro_iter + 0x28);
        static int last28 = -99;
        if (st != last || c28 != last28) { LOG("[aa] 存档协程状态 %d → %d（取消标志 +0x28=%d，帧%llu）", last, st, c28, (unsigned long long)dm_guest_frame()); last = st; last28 = c28; }
    }
    if (g_aa_release_b_at) {
        extern void dm_script_set(uint64_t, int32_t, int32_t, int32_t, int32_t);
        extern uint64_t dm_guest_frame(void);
        if (dm_guest_frame() >= g_aa_release_b_at) {
            dm_script_set(0, 0, 0, 0, 0); g_aa_release_b_at = 0;
            g_aa_refresh_at = dm_guest_frame() + 20;   // let the UI settle, then refresh the slot list
        }
    }
    if (g_aa_refresh_at) {
        extern uint64_t dm_guest_frame(void);
        if (dm_guest_frame() >= g_aa_refresh_at) {
            g_aa_refresh_at = 0;
            module_t *m = aa_main_module();
            if (m && !strcmp(m->build_id, "c5b5d28842207c47bd0fc97137e5e75f")) {
                uint64_t inst = ((uint64_t (*)(void))(m->base + 0x337b40))();      // SaveLoadUICtrl.get_instance()
                LOG("刷新存档槽列表：UICtrl=%#llx", (unsigned long long)inst);
                if (inst) ((void (*)(uint64_t))(m->base + 0x338460))(inst);        // UpdateSlotList()
                LOG("刷新存档槽列表：返回");
            }
        }
    }
    static int done, autosave_frames = -2; static uint64_t opened_at;
    // DM_AA_AUTOSAVE=<frames after the dialog opens>; unset = on with the default 20 frames for this build (the save
    // UI's coroutine never resumes on the host, so the game's own SaveGameDataRequest is called by the host instead),
    // DM_AA_AUTOSAVE=0 disables it
    if (autosave_frames == -2) { const char *e = getenv("DM_AA_AUTOSAVE"); autosave_frames = e ? atoi(e) : 20; }
    if (autosave_frames > 0) {
        extern uint64_t dm_guest_frame(void);
        static uint64_t last_win;
        if (g_aa_save_window && g_aa_save_window != last_win) { last_win = g_aa_save_window; opened_at = dm_guest_frame(); }
        if (g_aa_save_window && !done && dm_guest_frame() >= opened_at + (uint64_t)autosave_frames) { done = 1; aa_host_save_request(); return; }
    }
    const char *e = getenv("DM_AA_CALL_SAVE");
    if (!e || done) return;
    unsigned frame = 0, slot = 0;
    if (sscanf(e, "%u:%u", &frame, &slot) != 2) return;
    extern uint64_t dm_guest_frame(void);
    if (dm_guest_frame() < frame) return;
    done = 1;
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || strcmp(m->build_id, "c5b5d28842207c47bd0fc97137e5e75f")) return;
    LOG("探针：直接调用 SaveGameDataRequest(slot=%u) main+0x3348e0", slot);
    ((void (*)(uint64_t, uint64_t))(m->base + 0x3348e0))(0, slot);
    LOG("探针：SaveGameDataRequest 返回");
}
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "离线：网络恒不可用", int, h_nifm_Available, void) { aa_net_publish_init(); aa_call_save_probe(); const char *e = getenv("DM_NET_FAKE"); return e && atoi(e) ? 1 : 0; }
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "离线：报失败，不弹系统联网界面", Result, h_nifm_HandleErr, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "离线：无 IP", Result, h_nifm_GetIp, uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "离线：无代理设置", Result, h_nifm_GetProxy, void *p) { (void)p; return RES_FAIL; }
HLE_FORCE("nnsslInitialize", "连 ssl 服务；网络库全局初始化依赖它成功，离线不建会话（剪）", Result, h_ssl_Initialize, void) { return RES_OK; }
HLE_FORCE("nnsslFinalize", "与 nnsslInitialize 配对（剪）", Result, h_ssl_Finalize, void) { return RES_OK; }

// ---------- socket: offline, all calls that contact the service return -1 in BSD convention, error code ENETDOWN ----------
// Not cut: Inet* / nnsocketInet* byte order and address text conversion (pure computation).
#define SK_ENETDOWN 50                  // Horizon sockets use FreeBSD error codes
static __thread int t_skerr = SK_ENETDOWN;
static int sk_fail(void) { t_skerr = SK_ENETDOWN; return -1; }
#define SKF(m, cname, ...) HLE_FORCE(m, "bsd 套接字服务（剪）：回 -1/ENETDOWN", int, cname, __VA_ARGS__)
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { (void)a; (void)b; (void)c; return sk_fail(); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { (void)s; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { (void)s; return sk_fail(); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c) { (void)s; (void)c; return sk_fail(); }
SKF("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { (void)f; (void)n; (void)t; return sk_fail(); }
SKF("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { (void)n; (void)r; (void)w; (void)e; (void)t; return sk_fail(); }
SKF("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", h_sk_EventFd, unsigned long v, int f) { (void)v; (void)f; return sk_fail(); }
SKF("_ZN2nn6socket11EventFdReadEiPm", h_sk_EventFdRead, int s, unsigned long *v) { (void)s; (void)v; return sk_fail(); }
SKF("_ZN2nn6socket12EventFdWriteEim", h_sk_EventFdWrite, int s, unsigned long v) { (void)s; (void)v; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "bsd 套接字服务（剪）", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "bsd 套接字服务（剪）", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "bsd 套接字服务（剪）", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "域名解析（剪）：EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    (void)h; (void)s; (void)hint; if (res) *res = NULL; return 4;   // EAI_FAIL
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "与 GetAddrInfo 配对（剪）", void, h_sk_FreeAddrInfo, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "域名解析（剪）：NULL", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "套接字错误码由宿主记录", int, h_sk_GetLastError, void) { return t_skerr; }
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { (void)a; (void)b; (void)c; return sk_fail(); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketClose", h_nsk_Close, int s) { (void)s; return sk_fail(); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c) { (void)s; (void)c; return sk_fail(); }
SKF("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) { (void)f; (void)n; (void)t; return sk_fail(); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("nnsocketRecv", "bsd 套接字服务（剪）", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("nnsocketSend", "bsd 套接字服务（剪）", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("nnsocketGetHostByNameWithOptions", "域名解析（剪）：NULL", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("nnsocketGetLastErrno", "套接字错误码由宿主记录", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "套接字错误码由宿主记录", void, h_nsk_SetErrno, int e) { t_skerr = e; }

// ---------- ssl: offline, Initialize succeeds (the network library's global initialization depends on it), everything else that contacts the service fails ----------
// Not cut: Context / Connection construction and destruction, Context::GetContextId (only read and write game objects).
#define SSF(m, cname, ...) HLE_FORCE(m, "ssl 服务（剪）：失败", Result, cname, __VA_ARGS__)
HLE_FORCE("_ZN2nn3ssl10InitializeEv", "与 nnsslInitialize 一致：成功、不建会话（剪）", Result, h_ssl_InitCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl8FinalizeEv", "与 Initialize 配对（剪）", Result, h_ssl_FinCpp, void) { return RES_OK; }
SSF("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", h_ssl_CtxCreate, void *self, int v) { (void)self; (void)v; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context7DestroyEv", h_ssl_CtxDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", h_ssl_CtxRegPki, void *self, uint64_t *id, int p) { (void)self; (void)id; (void)p; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context9RemovePkiEm", h_ssl_CtxRemovePki, void *self, uint64_t id) { (void)self; (void)id; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_BiBufSize, uint32_t *o, void *ids, unsigned n) { (void)ids; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", h_ssl_BiCerts, void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) { (void)o; (void)b; (void)bs; (void)ids; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", h_ssl_CnCreate, void *self, void *ctx) { (void)self; (void)ctx; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection7DestroyEv", h_ssl_CnDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11DoHandshakeEv", h_ssl_CnHandshake, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11SetHostNameEPKcj", h_ssl_CnHost, void *self, const char *h, unsigned n) { (void)self; (void)h; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", h_ssl_CnIoMode, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", h_ssl_CnCache, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", h_ssl_CnSock, void *self, int s) { (void)self; (void)s; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", h_ssl_CnLastErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", h_ssl_CnVerifyErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PeekEPcPij", h_ssl_CnPeek, void *self, char *b, int *o, unsigned n) { (void)self; (void)b; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", h_ssl_CnPoll, void *self, void *o, void *i, unsigned t) { (void)self; (void)o; (void)i; (void)t; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl10Connection4ReadEPcj", "ssl 服务（剪）：读到 -1", int, h_ssl_CnRead, void *self, char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
HLE_FORCE("_ZN2nn3ssl10Connection5WriteEPKcj", "ssl 服务（剪）：写出 -1", int, h_ssl_CnWrite, void *self, const char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
// C interface: all return nnResult
#define SSC(m, cname) HLE_FORCE(m, "ssl 服务（剪）：失败", Result, cname, void) { return RES_FAIL; }
SSC("nnsslConnectionCreate", h_nssl_Create) SSC("nnsslConnectionDestroy", h_nssl_Destroy)
SSC("nnsslConnectionDoHandshakeWithCertBuffer", h_nssl_Handshake) SSC("nnsslConnectionGetConnectionId", h_nssl_GetId)
SSC("nnsslConnectionGetNextAlpnProto", h_nssl_GetAlpn) SSC("nnsslConnectionGetVerifyCertError", h_nssl_VerifyErr)
SSC("nnsslConnectionPeek", h_nssl_Peek) SSC("nnsslConnectionPending", h_nssl_Pending) SSC("nnsslConnectionRead", h_nssl_Read)
SSC("nnsslConnectionSetHostName", h_nssl_SetHost) SSC("nnsslConnectionSetIoMode", h_nssl_SetIo)
SSC("nnsslConnectionSetNextAlpnProto", h_nssl_SetAlpn) SSC("nnsslConnectionSetOption", h_nssl_SetOpt)
SSC("nnsslConnectionSetSessionCacheMode", h_nssl_SetCache) SSC("nnsslConnectionSetSocketDescriptor", h_nssl_SetSock)
SSC("nnsslConnectionSetVerifyOption", h_nssl_SetVerify) SSC("nnsslConnectionWrite", h_nssl_Write)
SSC("nnsslContextDestroy", h_nssl_CtxDestroy) SSC("nnsslContextGetContextId", h_nssl_CtxGetId)
