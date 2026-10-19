// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// Key map (Nintendo layout): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick, Return=+ Tab=−.
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "dm.h"
#include "dm_input.h"
#include "dm_uiscreen.h"
void mtl_set_ui_rect_fn(void (*fn)(double *y, double *h));
static void ui_rect_frac(double *fy, double *fh) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); if (dm_screen_real(&rw, &rh) && rh > 0) { *fy = (double)y / rh; *fh = (double)h / rh; } }   // Same as front_ios.m: vertical position and height of the UI rectangle (as fractions of the frame height)
__attribute__((constructor)) static void ui_shift_reg(void) { mtl_set_ui_rect_fn(ui_rect_frac); }   // Must also take effect when running without a window (screenshots / gate checks), so it is not placed where the layer is created
#include "front.h"
#include "hle.h"
#include <signal.h>

static CAMetalLayer *g_layer;
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
// The original game's 16:9 constants, behavior unchanged; DM_GAME_RES=<width>x<height> (docked mode; handheld scaled by 720/900) is only for side-by-side comparison with iOS widescreen
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}

// Physical controller (Xbox / DualSense / ...): GameController framework, polled from the main thread.
// Nintendo layout vs the pad's own: A (bottom) = confirm, B (right) = cancel -- the same as the keyboard map.
static void pad_poll(void) {
    uint64_t b = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    NSArray<GCController *> *cs = [GCController controllers];
    static long last_n = -1; if ((long)cs.count != last_n) { last_n = (long)cs.count; LOG("[手柄] 当前 %ld 个：%s", last_n, last_n ? [[[cs firstObject] vendorName] UTF8String] ?: "?" : "无"); }
    for (GCController *c in cs) {
        GCExtendedGamepad *p = c.extendedGamepad; if (!p) continue;
        if (p.buttonA.isPressed) b |= 1ull << 0;
        if (p.buttonB.isPressed) b |= 1ull << 1;
        if (p.buttonX.isPressed) b |= 1ull << 2;
        if (p.buttonY.isPressed) b |= 1ull << 3;
        if (p.leftThumbstickButton.isPressed) b |= 1ull << 4;
        if (p.rightThumbstickButton.isPressed) b |= 1ull << 5;
        if (p.leftShoulder.isPressed) b |= 1ull << 6;
        if (p.rightShoulder.isPressed) b |= 1ull << 7;
        if (p.leftTrigger.isPressed) b |= 1ull << 8;
        if (p.rightTrigger.isPressed) b |= 1ull << 9;
        if (p.buttonMenu.isPressed) b |= 1ull << 10;
        if (p.buttonOptions.isPressed) b |= 1ull << 11;
        if (p.dpad.left.isPressed) b |= 1ull << 12;
        if (p.dpad.up.isPressed) b |= 1ull << 13;
        if (p.dpad.right.isPressed) b |= 1ull << 14;
        if (p.dpad.down.isPressed) b |= 1ull << 15;
        lx = (int32_t)(p.leftThumbstick.xAxis.value * 32767.0f);
        ly = (int32_t)(p.leftThumbstick.yAxis.value * 32767.0f);
        rx = (int32_t)(p.rightThumbstick.xAxis.value * 32767.0f);
        ry = (int32_t)(p.rightThumbstick.yAxis.value * 32767.0f);
        break;
    }
    dm_pad_set(b, lx, ly, rx, ry);
}
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    dm_input_set(g_btn, lx, ly, 0, 0);
}
static int key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return 10; case 48: return 11;                                          // Return Tab → + −
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
static void on_key(NSEvent *e, int down) {
    unsigned short c = e.keyCode;
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    int b = key_bit(c);
    if (b >= 0) g_btn = down ? (g_btn | 1ull << b) : (g_btn & ~(1ull << b));
    push();
}

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];                       // The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("客体线程结束"); exit(0);
}

// Render scale menu (View menu): the game re-selects its resolution when it pops the notification dm_render_scale_set queues
static void window_fit(NSWindow *w, double factor);
static int g_win_sized;   // the window has been sized to the picture (the game renders 1920x1080; 1:1 keeps the text pixel-exact)
// Keep 16:9 while the user drags the corner: contentAspectRatio alone was not honoured here, the delegate always is
@interface DMWinDelegate : NSObject <NSWindowDelegate>
@end
@implementation DMWinDelegate
- (NSSize)windowWillResize:(NSWindow *)w toSize:(NSSize)fsz {
    if (w.styleMask & NSWindowStyleMaskFullScreen) return fsz;
    NSRect c = [w contentRectForFrameRect:NSMakeRect(0, 0, fsz.width, fsz.height)];
    if (c.size.width < 64 || c.size.height < 36) return fsz;
    double cw = c.size.width, ch = c.size.height;
    NSRect cur = w.frame;
    double dw = fabs(fsz.width - cur.size.width), dh = fabs(fsz.height - cur.size.height);
    double cwFit;
    if (dh < 1e-6 && dw >= 1e-6) {
        cwFit = cw;                     // left/right edge: only the width moved, so the width drives
    } else if (dw < 1e-6 && dh >= 1e-6) {
        cwFit = ch * 16.0 / 9.0;        // top/bottom edge: only the height moved, so the height drives
    } else {
        // corner: the opposite corner is the anchor, so take the smaller of the two candidates (the window stays
        // inside the drag and the result is continuous - nothing to oscillate between)
        cwFit = cw < ch * 16.0 / 9.0 ? cw : ch * 16.0 / 9.0;
    }
    NSRect f = [w frameRectForContentRect:NSMakeRect(0, 0, cwFit, cwFit * 9.0 / 16.0)];
    return NSMakeSize(round(f.size.width), round(f.size.height));
}
@end
@interface DMScaleTarget : NSObject
- (void)pick:(NSMenuItem *)it;
@end
@implementation DMScaleTarget
- (void)pick:(NSMenuItem *)it {
    NSWindow *w = NSApp.windows.count ? NSApp.windows[0] : nil; if (!w) return;
    void window_fit(NSWindow *w, double factor); g_win_sized = 1;
    window_fit(w, [it.representedObject doubleValue]);   // 0 = 1:1
}
@end

static int g_fs_guard;   // fullscreen transition in progress: its resign/become-key notifications are not real focus changes
// Size the window so the drawable is picture x factor pixels (0 = 1:1). Called from the 120 Hz timer until the picture size is known.
static void window_fit(NSWindow *w, double factor) {
    int pw = 0, ph = 0; int mtl_pic_size(int *w, int *h);
    if (!mtl_pic_size(&pw, &ph)) return;
    double sc = w.backingScaleFactor > 0 ? w.backingScaleFactor : 1.0;
    if (factor < 0) { pw = 1920; ph = 1080; factor = 1.0; }   // fixed 1080p drawable: the bigger picture is downscaled (supersampling)
    if (factor <= 0) factor = 1.0;
    NSSize want = NSMakeSize(pw * factor / sc, ph * factor / sc);
    NSRect c = [w contentRectForFrameRect:w.frame];
    NSRect nf = [w frameRectForContentRect:NSMakeRect(c.origin.x, c.origin.y, want.width, want.height)];
    [w setFrame:nf display:YES];
    LOG("[窗口] 内容 %.0fx%.0f 点（画布 %.0fx%.0f 像素），游戏画面 %dx%d", want.width, want.height, want.width * sc, want.height * sc, pw, ph);
}
static void fs_toggle(NSWindow *w) {
    g_fs_guard = 1;
    [w toggleFullScreen:nil];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ g_fs_guard = 0; });
}

void front_run(void (*guest)(void)) {
    if (!getenv("DM_WINDOW")) { guest(); return; }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect r = NSMakeRect(0, 0, 1280, 720);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @DM_GAME_NAME;   // the game profile's name (game name label)
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;   // green button = real fullscreen (without this it only zooms)
        { static DMWinDelegate *wd; wd = [DMWinDelegate new]; win.delegate = wd; }   // 16:9 while dragging (see windowWillResize)
        if (getenv("DM_WINDOW_PROBE")) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{   // probe: which points belong to the game view
            NSView *cv = win.contentView; NSView *theme = cv.superview; if (!theme) return;
            // Scan from the bottom-left corner inwards: tells us exactly how wide AppKit's resize border is here.
            for (int d = 1; d <= 12; d++) {
                NSPoint wp = [cv convertPoint:NSMakePoint(d, d) toView:nil];
                NSView *h = [theme hitTest:[theme convertPoint:wp fromView:nil]];
                LOG("[窗口] 命中测试 距角 %d 点: %s", d, h ? object_getClassName(h) : "nil");
            }
        });
        // Minimal main menu: without one there is no "Enter Full Screen" item and no Command-Control-F, and Command-Q does not quit
        {
            NSMenu *bar = [[NSMenu alloc] init];
            NSMenuItem *appItem = [[NSMenuItem alloc] init]; [bar addItem:appItem];
            NSMenu *appMenu = [[NSMenu alloc] init];
            [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
            appItem.submenu = appMenu;
            NSMenuItem *viewItem = [[NSMenuItem alloc] init]; [bar addItem:viewItem];
            NSMenu *viewMenu = [[NSMenu alloc] init];
            NSMenuItem *fs = [[NSMenuItem alloc] initWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
            fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;   // the system's Command-Control-F
            [viewMenu addItem:fs];
            viewItem.submenu = viewMenu;
            [NSApp setMainMenu:bar];
        }
        NSView *v = win.contentView;
        v.wantsLayer = YES;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        v.layer = g_layer;
        [win center];
        [win makeKeyAndOrderFront:nil];
        {   // the picture / drawable ratio decides whether the text can be sharp
            NSSize cs = v.bounds.size; double sc = win.backingScaleFactor > 0 ? win.backingScaleFactor : 1.0;
            LOG("[窗口] 内容 %.0fx%.0f 点，画布 %.0fx%.0f 像素（显示缩放 %.1f）", cs.width, cs.height, cs.width * sc, cs.height * sc, sc); }
        [NSApp activateIgnoringOtherApps:YES];
        {   // DM_RENDER_SCALE_AT=<seconds>:<scale>[,<seconds>:<scale>...]: same as picking a Render Scale menu item, at fixed times (tests)
            const char *e = getenv("DM_RENDER_SCALE_AT");
            for (const char *q = e; q && *q;) { double at = 0, sc = 0;
                if (sscanf(q, "%lf:%lf", &at, &sc) != 2) break;
                if (at > 0) [NSTimer scheduledTimerWithTimeInterval:at repeats:NO block:^(NSTimer *t) { (void)t; dm_render_scale_set(sc); }];
                q = strchr(q, ','); if (q) q++; }
        }
        {   // DM_WINDOW_1TO1_AT=<seconds>[:<factor>]: same as View > Window Size (tests)
            const char *e = getenv("DM_WINDOW_1TO1_AT"); double at = 0, fac = 0;
            if (e) sscanf(e, "%lf:%lf", &at, &fac);
            if (e && at > 0) [NSTimer scheduledTimerWithTimeInterval:at repeats:NO block:^(NSTimer *t) { (void)t;
                NSMenuItem *fake = [[NSMenuItem alloc] init]; fake.representedObject = @(fac); static DMScaleTarget *tg2; tg2 = [DMScaleTarget new]; [tg2 pick:fake]; }];
        }
        if (getenv("DM_FULLSCREEN")) {   // start in fullscreen (same as the green button)
            [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:NO block:^(NSTimer *t) { (void)t; fs_toggle(win); }];
        }
        [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
        [NSTimer scheduledTimerWithTimeInterval:1.0 / 120.0 repeats:YES block:^(NSTimer *t) { (void)t; pad_poll();
            if (!g_win_sized && !getenv("DM_WINDOW_KEEP_SIZE")) { int pw = 0, ph = 0; int mtl_pic_size(int *w, int *h);
                if (mtl_pic_size(&pw, &ph)) { g_win_sized = 1; window_fit(win, 1.0); } } }];
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual
            if ((e.modifierFlags & NSEventModifierFlagCommand) && !(e.modifierFlags & NSEventModifierFlagControl)) {
                if (e.keyCode == 3 && e.type == NSEventTypeKeyDown) { fs_toggle(win); return nil; }   // Command-F: fullscreen
                return e;   // Command-Q / Command-Control-F and the rest of the menu bar keep working
            }
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // Swallow it to avoid the system beep
        }];
        // Mouse as a single-point touchscreen (for testing touch on the local machine, front.h dm_touch_set): left button down / drag = finger 0 at the normalized coordinates, release = lifted
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;
            // Who owns the current drag: decided by where the button went down, not by where the cursor is now.
            // A live resize that starts on the window border must keep every drag, even while the cursor crosses
            // the game view, otherwise the resize freezes.
            static int owner;   // 0 none, 1 touch (started in the game view), 2 system (title bar / border / buttons)
            // Ask AppKit itself: the theme frame's hit test returns the game view only where a click belongs to the
            // game. In the title bar, the traffic lights and the resize border it returns other views, so there is
            // nothing to guess about the width of the resize zone.
            NSView *theme = win.contentView.superview;
            NSView *hit = theme ? [theme hitTest:[theme convertPoint:e.locationInWindow fromView:nil]] : nil;
            BOOL inView = (hit == v) || (hit && [hit isDescendantOf:v]);
            if (e.type == NSEventTypeLeftMouseDown) {   // the clicks we consume never reach AppKit, so activate here
                if (!NSApp.isActive) [NSApp activateIgnoringOtherApps:YES];
                if (!win.isKeyWindow) [win makeKeyAndOrderFront:nil];
                NSButton *zoom = [win standardWindowButton:NSWindowZoomButton];
                if (zoom && !zoom.hidden && NSPointInRect(e.locationInWindow, [zoom convertRect:zoom.bounds toView:nil])) {
                    LOG("[窗口] 绿色按钮 → %s全屏", win.styleMask & NSWindowStyleMaskFullScreen ? "退出" : "进入");
                    owner = 2; fs_toggle(win); return nil;
                }
                owner = inView ? 1 : 2;
                if (owner == 2) return e;   // title bar, traffic lights and the resize border go to AppKit
            }
            if (e.type == NSEventTypeLeftMouseUp) { int o = owner; owner = 0; dm_touch_set(NULL, 0); return o == 1 ? nil : e; }
            if (owner == 2) return e;   // a system drag (live resize / window move): never swallow it
            if (!inView) return e;
            NSPoint p = [v convertPoint:e.locationInWindow fromView:nil]; NSSize sz = v.bounds.size;
            if (sz.width <= 0 || sz.height <= 0) return e;
            dm_touch t = {0, (float)(p.x / sz.width), (float)(1.0 - p.y / sz.height)};   // The AppKit view origin is at bottom left, the touchscreen origin is at top left
            dm_touch_set(&t, 1); return nil;
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; exit(0); }];
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1)
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        // DM_KEEP_FOCUS=1: never report focus loss (Unity pauses its own rendering when it thinks it is in the
        // background -- keeps the game running while the player switches to another app)
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (!g_fs_guard && !win.miniaturized) dm_focus_set(getenv("DM_KEEP_FOCUS") ? 1 : 2); }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (!g_fs_guard) dm_focus_set(1); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        // Fullscreen: the drawable follows the window, the present path scales the game picture to it keeping 16:9 (black bars)
        [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n;
            [NSTimer scheduledTimerWithTimeInterval:0.8 repeats:NO block:^(NSTimer *t) { (void)t; CGSize d = g_layer.drawableSize; LOG("[窗口] 进入全屏：画布 %.0fx%.0f（等比放大，多余区域留黑边）", d.width, d.height); }]; }];
        [nc addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n;
            [NSTimer scheduledTimerWithTimeInterval:0.8 repeats:NO block:^(NSTimer *t) { (void)t; CGSize d = g_layer.drawableSize; LOG("[窗口] 退出全屏：画布 %.0fx%.0f", d.width, d.height); }]; }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// Software keyboard input box (front.h): the main thread shows an NSAlert + text field and the guest thread waits synchronously for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel)
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    __block int r = 0;
    __block NSString *text = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new];
        a.messageText = title && *title ? @(title) : @"输入";
        NSString *info = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
        a.informativeText = info;
        [a addButtonWithTitle:@"确定"]; [a addButtonWithTitle:@"取消"];
        NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
        f.stringValue = initial ? @(initial) : @"";
        if (guide && *guide) f.placeholderString = @(guide);
        a.accessoryView = f; a.window.initialFirstResponder = f;
        r = [a runModal] == NSAlertFirstButtonReturn;
        text = f.stringValue;
    });
    if (!r) return 0;
    (void)max_chars;                              // Truncation is done uniformly in hle_swkbd.c in UTF-16 units
    snprintf(out, out_cap, "%s", text.UTF8String);
    return 1;
}
