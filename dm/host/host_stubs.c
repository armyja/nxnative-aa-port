// Hook points left behind by the game-specific modules split out of d2r-mobile (diagnostic tick gates, TACT/Granny, UI screen, touch UI interaction, ...):
// the generic host still calls them, and they get empty implementations here. When a feature is redone for this game, replace the matching stub with the real thing.
#include "dm.h"
#include "front.h"
#include <stdatomic.h>
#include <math.h>

void crash_sample_self(void) {}
void inflate_bench_maybe(void) {}
void ticks_install(void) {}
void target_rank_install(void) {}
void dm_padmode_boot(void) {}
void dm_prof_mark(const char *seg) { (void)seg; }
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }
void tact_chk_init(void) {}
void tact_prefetch_start(const char *outdir) { (void)outdir; }
void tact_prefetch_stop(void) {}
void granny_prof_dump(void) {}
void uimsg_subscribe(void (*fn)(const char *cat, const char *act, const char *arg)) { (void)fn; }
void dm_ui_text_entered(const char *text) { (void)text; }
int dm_custom_lang_explicit(void) { return 0; }
int dm_ingame_now(void) { return 0; }
void dm_uiscreen_res_reread(void) {}
// Render scale: multiplies the resolution the game renders at (1.0 = the game's own resolution). The game profile's
// render_scale sets the default, DM_RENDER_SCALE overrides it; changing it at run time calls dm_oe_res_changed(), which
// queues the "performance mode changed" notification the game answers by re-reading the resolution parameter slots.
static _Atomic float g_rs;   // 0 = not read yet
static double rs_value(void) {
    float v = atomic_load(&g_rs);
    if (v == 0) {
        double d = DM_GAME_RENDER_SCALE;
        const char *e = getenv("DM_RENDER_SCALE");
        if (e && *e) { double x = atof(e); if (x > 0.1 && x < 8.0) d = x; }
        v = (float)d; atomic_store(&g_rs, v);
    }
    return (double)v;
}
double dm_render_scale(void) { return rs_value(); }
void dm_render_scale_set(double v) {
    if (!(v > 0.1 && v < 8.0)) return;
    atomic_store(&g_rs, (float)v);
    LOG("[分辨率] 渲染缩放 → %.2f", v);
    void dm_oe_res_changed(void); dm_oe_res_changed();   // let the game re-select, if it enabled the notification
}
// Game resolution: whatever the front end gives (Mac window / iOS screen), scaled, without D2R's UI-screen rewriting
uint64_t dm_game_res(int docked) {
    uint64_t r = front_game_res(docked);
    double s = rs_value();
    if (s <= 0.1 || (s > 0.999 && s < 1.001)) return r;
    double w = (double)(uint32_t)r * s + 0.5, h = (double)(uint32_t)(r >> 32) * s + 0.5;
    if (w > 4096) w = 4096;   // the output resolution cap the rewriter patches is 4096x2160
    if (h > 2160) h = 2160;
    return ((uint64_t)h << 32) | (uint32_t)w;
}
int dm_screen_real(int *w, int *h) { *w = *h = 0; return 0; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { *x = *y = *w = *h = 0; }
// svc #0x80.. host gates: only D2R's rewrite rules create them; they should not appear in this game's build output
uint64_t svc_host_call(int num, uint64_t *regs) { (void)regs; die("svc_host_call：本作没有宿主门 svc #%#x", num); }
