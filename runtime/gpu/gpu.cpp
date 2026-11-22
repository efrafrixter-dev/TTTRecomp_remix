#include "psp.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include "perf.h"
#include "patches/all.h"
#include "gpu/gl.h"
#include "disc/render_frame.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <array>
#include <deque>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <atomic>
#include <memory>
#include <set>

// Software GE executor. Direct mode (sprites, triangles, points) and 3D mode:remix
// bone skinning, world/view/projection matrices, diffuse lighting, near clipping,
// 16-bit z-buffer, culling and perspective texturing. Writes to guest VRAM.

static uint32_t R[256];
static uint32_t v_addr = 0, i_addr = 0, base = 0, offset_addr = 0;
static uint8_t clut_raw[1024];   // the palette is stored raw: the format is applied when sampling
static int clut_bytes = 0;
static uint32_t clut_dir = 0;

// deferred sampling: each thread keeps its own copy of wrap, palette format and palette
static thread_local uint32_t tl_wrap = 0, tl_clutf = 0;
static thread_local int tl_clut_bytes = 0;
static thread_local uint32_t tl_pal_seq = 0xFFFFFFFFu;
static thread_local uint8_t tl_clut[1024];
static uint8_t palettes[256][1024];
static uint32_t pal_seq[256];
static int pal_live[256];
static int pal_next = 0, pal_current = -1;
static uint32_t pal_seq_global = 0;
static uint64_t sprites = 0, prims_other = 0, prims_3d = 0;
static int warnings = 0;
static uint64_t frames_drawn = 0;
static int ef_now = 0;   // inside the bars-behind packet (marker 0xEF)

void psp_gpu_flush();
void psp_gpu_flush_by(int reason);
void psp_window_texture(unsigned t, int tw, int th, int uw, int uh, void* fence);
void psp_window_texture_mid(unsigned t, int mode);
void psp_window_mid_stats(int* n, double* avg, double* mx);
int psp_present_nvb();
int psp_window_debug();
void psp_window_repeat(int n);
int psp_window_aniso();
int psp_window_bias();
bool psp_window_dump_requested();
struct Tex; struct TexState;
static bool glr_enabled();
struct Chain;
static unsigned glr_upload_chain(const Chain& c, unsigned reuse, int* aw, int* ah);
static int glr_mips();
static int glr_up_of(int w, int h);
static unsigned glr_palette();
static unsigned glr_target_tex(uint32_t dir, int width, int tw, int th, int tfmt, int* fmt, int* rw, int* rh);
static void glr_report();
static void glr_flush_queue();
static void glr_cap_read();
static void glr_alpha_keep(bool save); static void glr_bars_back(bool begin);
static void glr_read(uint32_t dir, int width, int fmt);
static bool glr_present(uint32_t dir);
static unsigned glr_twin_tex(uint32_t dir, int* w, int* h);
void psp_window_texture_dual(unsigned t);

static const uint32_t VRAM = 0x04000000u;
static float asp_x = 1.0f, asp_y = 1.0f;
// split screen (items 20c, 20d): port-only marker 0xFB from the GE sets the view being drawn (1..4). With 2 views
// they are the left and right halves (3D shifted to the centre of its half, no scaling); with 4, the quarters
// (3D scaled to half and shifted). Everything drawn in a view is clipped to it
static int split_pass = 0, split_now = 0, split_views = 2, split_hud = 0;
// per-player HUD passes (0xFB 5..8): clipped to the view; the guest already placed their 2D
// 2 views: left edge of the part of the full render each view shows (120 = centred), follows its fighter
static float sp_crop[3] = {120.0f, 120.0f, 120.0f};
void psp_gpu_split_crop(int v, float start) { if (v >= 1 && v <= 2) sp_crop[v] = start; }
// per-player HUD piece being drawn (0xFD view | ring << 3, box from 0xFE/0xFF). Pieces of the left half move as one
// block to the view's left edge (right-column views: right edge), those of the right half the other way; the block
// is the combo counter's span (48..107) so pieces keep their layout. Centred pieces: 2 views centred, shrunk to fit
static const float HUD_GL = 48.0f, HUD_GR = 107.0f, HUD_EDGE = 8.0f;
static int hud_code = 0;
static inline bool dual2();
static float hud_bb[4], hud_dx = 0, hud_sc = 1, hud_cx = 0, hud_cy = 0;
static bool hud_flip = false;
static float hud_piv[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};   // damage counter centre per view: hits counters mirror with it
static void hud_setup() {
    int v = hud_code & 7;
    bool ring = (hud_code >> 3) & 1, right = ((v - 1) & 1) != 0, four = split_views == 4, wide = four || dual2();
    float a = hud_bb[0], b = hud_bb[1], W = wide ? 480.0f : 240.0f, m = four ? 2.0f * HUD_EDGE : HUD_EDGE;
    hud_dx = 0; hud_sc = 1; hud_flip = false;
    hud_cx = (a + b) * 0.5f; hud_cy = (hud_bb[2] + hud_bb[3]) * 0.5f;
    if (ring || a > b) return;
    if (hud_code & 16) {                       // split layout AGS: left design; right views mirror each piece around the view centre
        if (hud_code & 128) hud_piv[v] = hud_cx;
        if (!right) return;
        hud_dx = W - 2.0f * ((hud_code & 64) && hud_piv[v] >= 0 ? hud_piv[v] : hud_cx);
        hud_flip = (hud_code & 32) != 0;      // technique window: art flipped too (its text was pre-flipped by the hooks)
        return;
    }
    if (b <= 240.0f) hud_dx = right ? (W - m) - HUD_GR : m - HUD_GL;
    else if (a >= 240.0f) hud_dx = right ? m - (480.0f - HUD_GR) : (W - m) - (480.0f - HUD_GL);
    else if (!wide) { hud_dx = 120.0f - hud_cx; if (b - a > 232.0f) hud_sc = 232.0f / (b - a); }
}
static bool split_clip = false;
static float sp_sx = 1, sp_sy = 1, sp_ox = 0, sp_oy = 0;
// panel (item 33): markers 0xE7/0xE8 around a 3D draw put the whole render into a screen rectangle (scaled to cover it,
// centred, clipped to it); 0xE9 ends it. Used by the 3D model of the character select
// map select 3D (map3d.inc): the panel's 3D is drawn at frame end, copied out and the screen restored (marker 0xF4 before 0xE7); the page
// pastes that copy where the map's render goes (marker 0xF3) one frame later, so everything the page draws after it stays in front
static bool m3_mode = false, m3_have = false;
static int m3_r[4] = {0, 0, 0, 0};
static void glr_m3d_copy(int k, int x0, int y0, int x1, int y1);
static bool glr_m3d_paste(int k, int x0, int y0, int x1, int y1);
static bool pnl_on = false, pnl_hide = false;   // pnl_hide (marker 0xEA): draws inside the panel reduced to one pixel (a model drawn only for its bone matrices)
static uint32_t pnl_R[256];   // GE state before the panel's 3D, given back after it (the menu's 2D of the next frame must not inherit the 3D's)
static int pnl_x0 = 0, pnl_x1 = 480, pnl_y0 = 0, pnl_y1 = 272;
static float pnl_s = 1, pnl_ox = 0, pnl_dy = 0;
static inline void pnl_apply(float* o) {
    float ys = (R[0x43] >> 23) & 1 ? 1.0f : -1.0f;
    o[0] = o[0] * pnl_s + pnl_ox * o[3]; o[1] = o[1] * pnl_s + pnl_dy * ys * o[3];
}
// render capture (item 33 part 2): markers 0xEB/0xEC around the panel draw of a generated render. The screen is cleared to a key
// colour at the capture's first draw; at the model's first primitive its head and waist give the box (bk spans tall, the head
// bh of it from the top) that every 3D vertex is then zoomed into, the largest rect of the big render's aspect on the screen;
// at 0xEC that rect is read back with the game's own cel, outline and antialiasing (that one frame of the menu is lost)
static bool cap_map = false;   // EB arg 1: a map's render, the whole 300:152 rect of the screen, opaque
static bool cap_on = false, cap_fresh = false, cap_zdone = false, cap_pt_done = false, cap_ready = false, cap_zoom = false, cap_face_prim = false;
static float cap_face[4] = {1e9f, 1e9f, -1e9f, -1e9f};   // screen box of the face parts (part flag 6) drawn in the capture
static float pnl_face[4] = {1e9f, 1e9f, -1e9f, -1e9f}, pnl_face_last[4] = {0, 0, -1, -1};   // the same in each panel frame, the last good one
static int pnl_face_age = 99;
static float cap_bk = 1.45f, cap_bh = 0.33f, cap_rx = 0, cap_ry = 0, cap_kz = 1, cap_sx0 = 0, cap_sy0 = 0, cap_sx1 = 480, cap_sy1 = 272;
static int cap_w = 0, cap_h = 0;
static uint32_t cap_tag = 0, cap_done_tag = 0, cap_dir = 0;
static int cap_dg = 0, cap_dg_n = 0, cap_dg_post = -1;   // TTT_CAP_DIAG=<personaje>: every primitive of its capture, then the translucent ones after it
void psp_cap_diag(bool on) { cap_dg = on; }
static float cap_pt[3][3], cap_px[3][2], cap_out_face[4], cap_out_u = 0;   // head, waist, mouth bones
static rf::Tweak cap_tw;
static std::vector<uint32_t> cap_out;
static std::mutex cap_mx;
void psp_cap_request_map(uint32_t tag) { std::lock_guard<std::mutex> g(cap_mx); cap_tag = tag; cap_ready = false; }
void psp_cap_request(uint32_t tag, float rw, const float* tweak, const float* head, const float* waist, const float* mouth) {
    std::lock_guard<std::mutex> g(cap_mx);
    cap_tag = tag; cap_bk = rw > 0.1f ? rw : 1.0f;
    cap_tw.zoom = tweak ? tweak[0] : 1.0f; cap_tw.dy = tweak ? tweak[1] : 0.0f; cap_tw.dx = tweak ? tweak[2] : 0.0f; cap_tw.bdy = tweak ? tweak[3] : 0.0f; cap_tw.bzoom = tweak ? tweak[4] : 1.0f;
    for (int k = 0; k < 3; k++) { cap_pt[0][k] = head[k]; cap_pt[1][k] = waist[k]; cap_pt[2][k] = mouth ? mouth[k] : 1e9f; }
    cap_ready = false;
}
bool psp_cap_take(uint32_t tag, std::vector<uint32_t>& px, int* w, int* h, float* pts) {
    std::lock_guard<std::mutex> g(cap_mx);
    if (!cap_ready || cap_done_tag != tag) return false;
    bool ok = !cap_out.empty();
    px.swap(cap_out); cap_out.clear(); *w = ok ? cap_w : 0; *h = ok ? cap_h : 0;
    for (int k = 0; k < 2; k++) { pts[2 * k] = cap_px[k][0]; pts[2 * k + 1] = cap_px[k][1]; }
    bool mo = cap_pt[2][0] < 1e8f;
    pts[4] = mo ? cap_px[2][0] : -1.0f; pts[5] = mo ? cap_px[2][1] : -1.0f; pts[6] = cap_out_face[2]; pts[7] = cap_out_face[3];
    pts[8] = cap_out_u;
    cap_ready = false;
    return true;
}
// game room 3D (item 33): where each big render is drawn, skipped while its row's model (marker 0xEE k) is shown
struct RoomQ { int menu = -1; float x0 = 0, y0 = 0, x1 = 0, y1 = 0, u0 = 0, v0 = 0, u1 = 0, v1 = 0, cy0 = 0, cy1 = 0; int tw = 0, th = 0; };
struct RoomS { int menu = -1; bool live = false; float pt[3][3]; float rw = 1; rf::Tweak tw; };
int (*psp_tex_bigmenu)(uint32_t addr, int* w, int* h) = nullptr;
void (*psp_room_cut)(float u0, float v0, float u1, float v1, float aspect) = nullptr;   // renders.h: the part of the big render the game room shows
static std::mutex rm_mx;
static RoomS rm_s[4];
static RoomQ rm_at[4];
static int rm_row[4] = {-1, -1, -1, -1}, rm_rowl[4] = {-1, -1, -1, -1}, rm_k = -1;   // menu drawn in each row this frame, last frame
static uint64_t rm_frame = 1, rm_atf[4] = {0, 0, 0, 0};
static bool rm_track = false, rm_fit = false, rm_zoom = false, rm_show = false, rm_done = false;
static float rm_bx = 0, rm_by = 0, rm_kz = 1, rm_full[4] = {0, 0, 1, 1}, rm_tgt[2] = {0, 0};
static float rm_pos[2] = {30.0f, -3.0f};   // TTT_ROOM3D_POS: face centre from the centre of the row's render, right and down
static int room_row(float y0, float y1) { int r = (int)std::lround(((y0 + y1) * 0.5f - 60.0f) / 54.0f); return r >= 0 && r < 4 ? r : -1; }   // rows J1..J4, 54 px apart
void psp_room_track(bool on) { rm_track = on; }
void psp_room_set(int k, int menu, bool live, const float* hd, const float* ws, const float* nk, float rw, const float* tweak) {
    if (k < 0 || k > 3) return;
    std::lock_guard<std::mutex> g(rm_mx);
    RoomS& r = rm_s[k];
    r.menu = menu; r.live = live; r.rw = rw > 0.1f ? rw : 1.0f;
    for (int i = 0; i < 3; i++) { r.pt[0][i] = hd ? hd[i] : 0.0f; r.pt[1][i] = ws ? ws[i] : 0.0f; r.pt[2][i] = nk ? nk[i] : 1e9f; }
    r.tw = rf::Tweak();
    if (tweak) { r.tw.zoom = tweak[0]; r.tw.dy = tweak[1]; r.tw.dx = tweak[2]; r.tw.bdy = tweak[3]; r.tw.bzoom = tweak[4]; r.tw.rup = tweak[5]; }
}
int psp_room_quads(int* menus, int max) {   // menu of each row drawn last frame (-1 none), the number of rows drawn
    std::lock_guard<std::mutex> g(rm_mx);
    int n = 0;
    for (int i = 0; i < 4 && i < max; i++) { menus[i] = rm_rowl[i]; n += rm_rowl[i] >= 0; }
    return n;
}
static uint64_t split_tris[5], split_prims[5], split_culled[5], split_us[5], split_frames = 0;
static int split_first = 0;
static uint64_t split_rt_skip = 0;
struct SplitRt { int view; uint32_t dir; int width; uint64_t prims, verts; };
static SplitRt rt[24];
static int rt_n = 0;
// TTT_SPLIT_PROF=1: host cycles per frame in vertex read/skinning, transform, triangle setup and GPU flush
static int sprof_on = -1;
static uint64_t sprof_c[4];
struct SProf {
    int k; uint64_t t;
    explicit SProf(int k_) : k(k_), t(0) { if (sprof_on < 0) { const char* e = getenv("TTT_SPLIT_PROF"); sprof_on = e && *e == '1'; } if (sprof_on) t = perf_clock(); }
    ~SProf() { if (t) sprof_c[k] += perf_clock() - t; }
};
static uint64_t gp_n[8];   // vertices read, skinned, skin reused, clip transforms, triangles, strip, indexed list, other 3D vertices
static int sp_x0 = 0, sp_x1 = 480, sp_y0 = 0, sp_y1 = 272;
// alternate views: 0xFC from the GE gives the views the guest skipped this frame (bits 0..3) and bit 4 = eligible
static int alt_mask = 0, alt_drew = 0;
static uint64_t alt_frame = 1, alt_pasted = 0, alt_saved[5];
static uint32_t alt_dir = 0; static int alt_w = 0;
// two screens (2 views): each view keeps the whole render squeezed to half width, the window stretches it back;
// shared 2D: the bars (top 80 lines) keep to their outer edge, the timer and everything below in both halves (pause: its owner's)
static volatile int dual_on = 0;
static volatile uint64_t split2_frame = 0;
static int dual_dup = 0;
static inline bool dual2() { return dual_on && split_views == 2; }
// full resolution: view 2, its HUD and the shared 2D of screen 2 go to a twin of the display target (dir | 0x80000000)
static int dual_side = 0, dual_logged = 0, twin_n = 0;
static uint32_t disp_dir[2] = {0, 0}, twin_set[8];
static uint64_t twin_drawn = 0, battle_seen = 0;
void psp_gpu_battle_frame() { battle_seen = alt_frame; }
static inline bool dual_full() { return dual2() && patch_dual_full.val && glr_enabled(); }
static inline uint32_t twin_dir(uint32_t d) { return d | 0x80000000u; }
static inline bool is_twin(uint32_t d) { return (d & 0x80000000u) != 0; }
void psp_gpu_dual(int on) { dual_on = on ? 1 : 0; dual_logged = 0; twin_n = 0; }
int psp_gpu_split2() { return alt_frame - split2_frame <= 1 ? 1 : 0; }
static uint64_t alt_saves = 0, alt_pastes = 0;

static void view_rect(int p, int& x0, int& y0, int& x1, int& y1) {
    if (split_views == 2) { x0 = p == 1 ? 0 : 240; x1 = x0 + 240; y0 = 0; y1 = 272; return; }
    x0 = ((p - 1) & 1) * 240; x1 = x0 + 240; y0 = ((p - 1) >> 1) * 136; y1 = y0 + 136;
}

static int icon_force = -1;
void psp_gpu_icon_player(int k) { icon_force = k; }
int psp_local_view_player(int v);
int psp_gpu_hud_player() { return icon_force >= 0 ? icon_force : split_hud && split_pass >= 1 ? psp_local_view_player(split_pass - 1) : 0; }
void psp_gpu_split_views(int n) { split_views = n == 4 ? 4 : 2; }

static void split_params(int p) {
    if (split_views == 2) {
        sp_sx = sp_sy = 1; sp_oy = 0; sp_ox = ((p == 1 ? 0.0f : 240.0f) - sp_crop[p]) / 240.0f;
        if (dual2()) { sp_sx = 0.5f; sp_ox = p == 1 ? -0.5f : 0.5f; }
        sp_x0 = p == 1 ? 0 : 240; sp_x1 = sp_x0 + 240; sp_y0 = 0; sp_y1 = 272;
        if (dual_full()) { sp_ox = 0; sp_sx = 1; sp_x0 = 0; sp_x1 = 480; }
        return;
    }
    int col = (p - 1) & 1, row = (p - 1) >> 1;
    float ys = (R[0x43] >> 23) & 1 ? 1.0f : -1.0f;
    sp_sx = sp_sy = 0.5f; sp_ox = col ? 0.5f : -0.5f; sp_oy = (row ? -0.5f : 0.5f) * ys;
    sp_x0 = col * 240; sp_x1 = sp_x0 + 240; sp_y0 = row * 136; sp_y1 = sp_y0 + 136;
}

// TTT_SCALE: internal resolution. Render targets are relocated outside guest VRAM
// at N times their size; read as textures they are declared just as large
// and coordinates scale by themselves.
static int scale_var = -1;

// TTT_SCALE: integer from 1 to 10, or "auto" to cover the monitor with supersampling.
int psp_scale() {
    if (scale_var > 0) return scale_var;
    const char* e = getenv("TTT_SCALE");
    int n = 1;
    if (!e || !*e || e[0] == 'a' || e[0] == 'A') {
        int mw = 1920, mh = 1080;
#ifdef _WIN32
        mw = GetSystemMetrics(SM_CXSCREEN);
        mh = GetSystemMetrics(SM_CYSCREEN);
#endif
        int nx = (mw + 479) / 480, ny = (mh + 271) / 272;
        n = nx > ny ? nx : ny;
    } else if (e) n = atoi(e);
    if (n < 1) n = 1;
    if (n > 10) n = 10;
    scale_var = n;
    printf("resolucion interna x%d (%d x %d)\n", n, 480 * n, 272 * n);
    fflush(stdout);
    return n;
}
static int scale_int() { return psp_scale(); }

static const uint32_t VRAM_ALTA = 0x10000000u;   // outside the 28-bit range the game can give the GE
// the address may come through the uncached mirror (0x44000000) and must be normalized
static inline bool is_vram(uint32_t d) {
    d &= 0x0FFFFFFFu;
    return d >= 0x04000000u && d < 0x04200000u;
}
// real size of the relocated area: 2 MB of VRAM times the scale squared
static inline bool is_vram_high(uint32_t d) {
    int n = scale_int();
    return d >= VRAM_ALTA && d < VRAM_ALTA + 0x200000u * (uint32_t)(n * n);
}
static inline uint32_t high_map(uint32_t d) {
    int n = scale_int();
    if (n == 1 || !is_vram(d)) return d;
    return VRAM_ALTA + ((d & 0x0FFFFFFFu) - 0x04000000u) * (uint32_t)(n * n);
}
static inline uint32_t norm_dir(uint32_t d) { return is_vram(d) ? (d & 0x0FFFFFFFu) : d; }
static inline bool is_disp(uint32_t d) { d = norm_dir(d); return d && (d == disp_dir[0] || d == disp_dir[1]); }
// offscreen targets filled from the display (outline post-process) get a twin too
static inline bool is_twinned(uint32_t d) { d = norm_dir(d); for (int i = 0; i < twin_n; i++) if (twin_set[i] == d) return true; return false; }
static void twin_add(uint32_t d) {
    d = norm_dir(d);
    if (is_twinned(d) || twin_n >= 8) return;
    twin_set[twin_n++] = d;
    printf("dual: destino 0x%08X copia la pantalla, se duplica para la pantalla 2\n", d);
    fflush(stdout);
}

#ifndef TTT_DIAG
#define TTT_DIAG 0
#endif

static void warning(const char* que, uint32_t v) {
    if (warnings++ < 25) printf("GE aviso: %s 0x%X\n", que, v);
}

static inline uint32_t rel(uint32_t arg) {
    return (offset_addr + (((R[0x10] & 0x000F0000) << 8) | arg)) & 0x0FFFFFFF;
}

// colour conversions to RGBA8 (R in the low byte)
static inline uint32_t from5650(uint16_t p) {
    uint32_t r = (p & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = ((p >> 11) & 31) * 255 / 31;
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}
static inline uint32_t from5551(uint16_t p) {
    uint32_t r = (p & 31) * 255 / 31, g = ((p >> 5) & 31) * 255 / 31, b = ((p >> 10) & 31) * 255 / 31;
    return r | (g << 8) | (b << 16) | ((p & 0x8000) ? 0xFF000000u : 0u);
}
static inline uint32_t from4444(uint16_t p) {
    uint32_t r = (p & 15) * 17, g = ((p >> 4) & 15) * 17, b = ((p >> 8) & 15) * 17, a = ((p >> 12) & 15) * 17;
    return r | (g << 8) | (b << 16) | (a << 24);
}
static inline uint16_t a5650(uint32_t c) {
    return (uint16_t)(((c & 0xFF) >> 3) | (((c >> 8) & 0xFF) >> 2) << 5 | (((c >> 16) & 0xFF) >> 3) << 11);
}
static inline uint16_t a5551(uint32_t c) {
    return (uint16_t)(((c & 0xFF) >> 3) | (((c >> 8) & 0xFF) >> 3) << 5 | (((c >> 16) & 0xFF) >> 3) << 10 | ((c >> 24) >= 128 ? 0x8000 : 0));
}
static inline uint16_t a4444(uint32_t c) {
    return (uint16_t)(((c & 0xFF) >> 4) | (((c >> 8) & 0xFF) >> 4) << 4 | (((c >> 16) & 0xFF) >> 4) << 8 | ((c >> 24) >> 4) << 12);
}

static uint32_t convert(uint32_t raw, int fmt) {
    switch (fmt) {
    case 0: return from5650((uint16_t)raw);
    case 1: return from5551((uint16_t)raw);
    case 2: return from4444((uint16_t)raw);
    default: return raw;
    }
}

// framebuffer
struct FB { uint32_t dir; int width, fmt; };
static FB fb() {
    FB f;
    f.dir = VRAM | (R[0x9C] & 0xFFFFFF) | ((R[0x9D] & 0xFF0000) << 8);
    f.width = R[0x9D] & 0x7FF;
    f.fmt = R[0xD2] & 3;
    if (f.width == 0) f.width = 512;
    f.dir = high_map(f.dir);
    f.width *= scale_int();
    return f;
}

static uint32_t read_pix(const FB& f, int x, int y) {
    if (f.fmt == 3) return psp_l32(f.dir + (y * f.width + x) * 4);
    return convert(psp_l16(f.dir + (y * f.width + x) * 2), f.fmt);
}

static void write_pix(const FB& f, int x, int y, uint32_t c);

// in clear mode bit 8 masks only colour and bit 9 only alpha
static void clear_pix(const FB& f, int x, int y, uint32_t c, uint32_t modo) {
    uint32_t out = c;
    if ((modo & 0x300) != 0x300) {
        uint32_t old = read_pix(f, x, y);
        if (!(modo & 0x100)) out = (out & 0xFF000000u) | (old & 0x00FFFFFFu);
        if (!(modo & 0x200)) out = (out & 0x00FFFFFFu) | (old & 0xFF000000u);
    }
    write_pix(f, x, y, out);
}

static void write_pix(const FB& f, int x, int y, uint32_t c) {
    uint32_t mask = R[0xE8] & 0xFFFFFF;
    uint32_t amask = R[0xE9] & 0xFF;
    if (mask || amask) {
        uint32_t old = read_pix(f, x, y);
        uint32_t m = mask | (amask << 24);
        c = (c & ~m) | (old & m);
    }
    if (f.fmt == 3) { psp_s32(f.dir + (y * f.width + x) * 4, c); return; }
    uint16_t p = f.fmt == 0 ? a5650(c) : f.fmt == 1 ? a5551(c) : a4444(c);
    psp_s16(f.dir + (y * f.width + x) * 2, p);
}

static int stencil_bits(const FB& f) { return f.fmt == 3 ? 8 : (f.fmt == 1 ? 1 : 0); }

static void write_stencil(const FB& f, int x, int y, uint32_t s) {
    int bits = stencil_bits(f);
    if (!bits) return;
    uint32_t amask = R[0xE9] & 0xFF;
    uint32_t old = read_pix(f, x, y);
    uint32_t a = ((s & ~amask) | (((old >> 24) & 0xFF) & amask)) & 0xFF;
    uint32_t c = (old & 0x00FFFFFFu) | (a << 24);
    if (f.fmt == 3) { psp_s32(f.dir + (y * f.width + x) * 4, c); return; }
    psp_s16(f.dir + (y * f.width + x) * 2, a5551(c));
}

static bool test_stencil(uint32_t dsta, uint32_t t) {
    int fn = t & 7, ref = (t >> 8) & 0xFF, mask = (t >> 16) & 0xFF;
    int s = (int)dsta & mask; ref &= mask;
    switch (fn) {
    case 0: return false; case 1: return true; case 2: return ref == s; case 3: return ref != s;
    case 4: return ref < s; case 5: return ref <= s; case 6: return ref > s; default: return ref >= s;
    }
}

static uint32_t stencil_op(int op, uint32_t old, int bits, uint32_t st = 0) {
    uint32_t max = bits >= 8 ? 255u : 1u;
    switch (op & 7) {
    case 1: return 0;
    case 2: return ((st >> 8) & 0xFF) & max;
    case 3: return (~old) & max;
    case 4: return old < max ? old + 1 : max;
    case 5: return old > 0 ? old - 1 : 0;
    default: return old & max;
    }
}

static int mip_var = -2;

// TTT_MIP=1 enables the generated mipmap chain even when the game does not ask for it
static bool mip_forced() {
    if (mip_var == -2) { const char* e = getenv("TTT_MIP"); mip_var = e ? atoi(e) : 0; }
    return mip_var != 0;
}

static int alphafb_var = -2;

// TTT_ALPHAFB=0 writes the source alpha again even with stencil off
static bool keep_alpha() {
    if (alphafb_var == -2) { const char* e = getenv("TTT_ALPHAFB"); alphafb_var = e ? atoi(e) : 1; }
    return alphafb_var != 0;
}

static int stencil_var = -2;

// TTT_STENCIL=0 disables stencil for comparison
static bool stencil_on() {
    if (stencil_var == -2) { const char* e = getenv("TTT_STENCIL"); stencil_var = e ? atoi(e) : 1; }
    return stencil_var != 0 && (R[0x24] & 1) != 0;
}

// textures
struct Tex {
    uint32_t dir; int stride, w, h, fmt, swz;
    uint32_t ndir[8]; int nstride[8], nw[8], nh[8], levels;
    int rt;   // 1 normal, N for a relocated render target
};
static Tex tex() {
    Tex t;
    t.fmt = R[0xC3] & 15;
    t.swz = R[0xC2] & 1;
    t.levels = ((R[0xC2] >> 16) & 7) + 1;
    for (int i = 0; i < 8; i++) {
        t.ndir[i] = (R[0xA0 + i] & 0xFFFFFF) | ((R[0xA8 + i] & 0xFF0000) << 8);
        t.nstride[i] = R[0xA8 + i] & 0x7FF;
        t.nw[i] = 1 << (R[0xB8 + i] & 15);
        t.nh[i] = 1 << ((R[0xB8 + i] >> 8) & 15);
    }
    t.dir = t.ndir[0]; t.stride = t.nstride[0]; t.w = t.nw[0]; t.h = t.nh[0];
    // the game declares levels it sometimes never uploaded: trim to the last coherent one
    int ok = 1;
    while (ok < t.levels) {
        int ew = t.nw[0] >> ok, eh = t.nh[0] >> ok;
        if (ew < 1) ew = 1;
        if (eh < 1) eh = 1;
        if (t.ndir[ok] == 0 || t.ndir[ok] == t.ndir[0] || t.nstride[ok] == 0 || t.nw[ok] != ew || t.nh[ok] != eh) break;
        ok++;
    }
    t.levels = ok;
    t.rt = 1;
    int n = scale_int();
    if (n > 1 && is_vram(t.dir)) {           // reading a render target: it comes at N times
        t.rt = n;
        t.dir = high_map(t.dir);
        t.stride *= n; t.w *= n; t.h *= n;
        t.levels = 1;
        t.ndir[0] = t.dir; t.nstride[0] = t.stride; t.nw[0] = t.w; t.nh[0] = t.h;
    }
    return t;
}

static int bpp_tex(int fmt) {
    switch (fmt) { case 0: case 1: case 2: case 6: return 16; case 3: case 7: return 32; case 4: return 4; default: return 8; }
}

static uint32_t texel_addr(const Tex& t, int x, int y, int bpp) {
    uint32_t row = (uint32_t)t.stride * bpp / 8;
    uint32_t xb = (uint32_t)x * bpp / 8;
    if (!t.swz) return t.dir + y * row + xb;
    uint32_t blocks_per_row = row / 16;
    uint32_t block = (y / 8) * blocks_per_row + xb / 16;
    return t.dir + block * 128 + (y % 8) * 16 + xb % 16;
}

static int clut_var = -2;

// TTT_CLUT: 0 as now, 1 out-of-range index to entry 0, 2 clamp to the last valid one, 3 return zero
static int clut_variant() {
    if (clut_var == -2) { const char* e = getenv("TTT_CLUT"); clut_var = e ? atoi(e) : 0; }
    return clut_var;
}

static uint32_t clut_color(uint32_t idx) {
    uint32_t f = tl_clutf;
    uint32_t shift = (f >> 2) & 31, mask = (f >> 8) & 0xFF, off = ((f >> 16) & 31) << 4;
    uint32_t i = (((idx >> shift) & mask) | off) & 255;
    int fmt = f & 3;
    int md = clut_variant();
    if (md) {
        int inp = tl_clut_bytes / (fmt == 3 ? 4 : 2);
        if (inp > 0 && (int)i >= inp) {
            if (md == 3) return 0;
            i = md == 1 ? 0 : (uint32_t)(inp - 1);
        }
    }
    if (fmt == 3) { uint32_t v; memcpy(&v, tl_clut + i * 4, 4); return v; }
    uint16_t v; memcpy(&v, tl_clut + i * 2, 2);
    return convert(v, fmt);
}

static uint32_t clut_entry(int i) {
    int fmt = tl_clutf & 3;
    if (fmt == 3) { uint32_t v; memcpy(&v, tl_clut + i * 4, 4); return v; }
    uint16_t v; memcpy(&v, tl_clut + i * 2, 2);
    return convert(v, fmt);
}

static uint32_t texel_n(const Tex& t0, int lvl, int u, int v) {
    Tex t = t0;
    t.dir = t0.ndir[lvl]; t.stride = t0.nstride[lvl]; t.w = t0.nw[lvl]; t.h = t0.nh[lvl];
    if (tl_wrap & 1) { if (u < 0) u = 0; if (u >= t.w) u = t.w - 1; }        // 1 = clamp
    else { u &= t.w - 1; }                                                   // 0 = repeat
    if (tl_wrap & 0x100) { if (v < 0) v = 0; if (v >= t.h) v = t.h - 1; }
    else { v &= t.h - 1; }
    switch (t.fmt) {
    case 0: case 1: case 2: return convert(psp_l16(texel_addr(t, u, v, 16)), t.fmt);
    case 3: return psp_l32(texel_addr(t, u, v, 32));
    case 4: { uint8_t b = psp_l8(texel_addr(t, u, v, 4)); return clut_color((u & 1) ? (b >> 4) : (b & 15)); }
    case 5: return clut_color(psp_l8(texel_addr(t, u, v, 8)));
    case 6: return clut_color(psp_l16(texel_addr(t, u, v, 16)));
    case 7: return clut_color(psp_l32(texel_addr(t, u, v, 32)));
    default: warning("formato de textura sin cubrir", t.fmt); return 0xFF00FFFFu;
    }
}

static uint32_t texel(const Tex& t, int u, int v) { return texel_n(t, 0, u, v); }

static void load_clut() {
    uint32_t dir = (R[0xB0] & 0xFFFFFF) | ((R[0xB1] & 0x0F0000) << 8);
    clut_dir = dir;
    int bytes = (R[0xC4] & 0x3F) * 32;
    if (bytes > 1024) bytes = 1024;
    for (int i = 0; i < bytes; i++) clut_raw[i] = psp_l8(dir + i);
    clut_bytes = bytes;
    pal_current = -1;
}

// vertices
struct Vert { float x, y, z, w, u, v; float nx, ny, nz; uint32_t color; bool has_color; uint32_t src = 0; };
struct Fmt { int tc, col, nrm, pos, wt, idx, nw, nm, through, stride, o_wt, o_tc, o_col, o_nrm, o_pos; };

static int align(int v, int a) { return (v + a - 1) & ~(a - 1); }

static Fmt format(uint32_t vt) {
    Fmt f;
    f.tc = vt & 3; f.col = (vt >> 2) & 7; f.nrm = (vt >> 5) & 3; f.pos = (vt >> 7) & 3;
    f.wt = (vt >> 9) & 3; f.idx = (vt >> 11) & 3; f.nw = ((vt >> 14) & 7) + 1; f.nm = ((vt >> 18) & 3) + 1;
    f.through = (vt >> 23) & 1;
    static const int tam[4] = {0, 1, 2, 4};
    int o = 0, amax = 1;
    f.o_wt = 0;
    if (f.wt) { o += tam[f.wt] * f.nw; amax = tam[f.wt] > amax ? tam[f.wt] : amax; }
    if (f.tc) { o = align(o, tam[f.tc]); f.o_tc = o; o += tam[f.tc] * 2; amax = tam[f.tc] > amax ? tam[f.tc] : amax; }
    if (f.col) { int a = f.col == 7 ? 4 : 2; o = align(o, a); f.o_col = o; o += a; amax = a > amax ? a : amax; }
    if (f.nrm) { o = align(o, tam[f.nrm]); f.o_nrm = o; o += tam[f.nrm] * 3; amax = tam[f.nrm] > amax ? tam[f.nrm] : amax; }
    if (f.pos) { o = align(o, tam[f.pos]); f.o_pos = o; o += tam[f.pos] * 3; amax = tam[f.pos] > amax ? tam[f.pos] : amax; }
    f.stride = align(o, amax);
    return f;
}

static inline float f32(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }
static inline float f24(uint32_t arg) { return f32(arg << 8); }

// PPSSPP Step_PosS16Through: x, y signed, z unsigned 16 bits (65535 = nearest); TTT_THROUGH_Z=0 reads z signed as before
static bool through_z_u16() { static int on = -1; if (on < 0) { const char* e = getenv("TTT_THROUGH_Z"); on = !(e && *e == '0'); } return on; }
static float read_num(uint32_t d, int fmt, bool through) {
    if (fmt == 1) return through ? (float)psp_l8(d) : (float)(int8_t)psp_l8(d) / 128.0f;
    if (fmt == 2) return through ? (float)(int16_t)psp_l16(d) : (float)(int16_t)psp_l16(d) / 32768.0f;
    return f32(psp_l32(d));
}

static float read_weight(uint32_t d, int fmt) {
    if (fmt == 1) return (float)psp_l8(d) / 128.0f;
    if (fmt == 2) return (float)psp_l16(d) / 32768.0f;
    return f32(psp_l32(d));
}

// GE matrices: bone 8x12, world 12, view 12, projection 16, texture 12
static float m_bone[8][12], m_world[12], m_view[12], m_proj[16], m_tgen[12];
static uint32_t view_hash = 0;
static float view_up[3] = {0, 1, 0};
static float view_inv[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};   // GE view inverse (rows of R^-1, then t): eye space = (p - t) R^-1 (water)
static float proj_eye[3] = {0, 0, 0};   // the eye in view space: where the projection x, y and w are all zero
float psp_wat_cam[8];
float psp_fx_ev[8];       // animation events for screen effects (rumble.inc events_fx): [0] rush blur carrier slot + 1, [1] its serial, [2] impact serial, [3] impact strength
float psp_cam_m[20];      // game camera matrix (wrapper on 0x0880BB28) [16] serial; GE projection scale [17] x, [18] y   // game camera (wrapper on 0x0880BB28): world Y of an eye-space point = dot(xyz, p) + w; [4] valid
static int i_bone = 0, i_world = 0, i_view = 0, i_proj = 0, i_tgen = 0;
static uint64_t prims_3d_ok = 0, tris_out = 0, tris_cull = 0;

// census of GE features the game enables and we do not implement
static uint64_t ge_fog = 0, ge_dither = 0, ge_aa = 0, ge_ctest = 0, ge_logop = 0, ge_flat = 0;
static uint32_t ge_fog_col = 0;

// scene census: per frame, 3D triangles grouped by texture. F1 prints it.
#define ESC_N 256
static uint32_t sc_tex[ESC_N];
static uint32_t sc_tri[ESC_N];
static uint32_t sc_pri[ESC_N];
static float sc_x0[ESC_N], sc_y0[ESC_N], sc_x1[ESC_N], sc_y1[ESC_N];
static float sc_pz0[ESC_N], sc_pz1[ESC_N];
static int sc_a0[ESC_N], sc_a1[ESC_N];
static uint32_t sc_in[ESC_N], sc_near[ESC_N], sc_cul[ESC_N], sc_fmt[ESC_N], sc_type[ESC_N], sc_addr[ESC_N];
static uint32_t sc_va0[ESC_N], sc_va1[ESC_N], sc_orders[ESC_N];
static uint32_t cur_prim_addr = 0;
void psp_gpu_prim_addr(uint32_t d) { cur_prim_addr = d; }

// TTT_GUARD_CLIP=0 clips only at w > eps as before; TTT_ZBIAS = depth steps (16-bit) a depth-tested draw without depth write moves toward the camera
static bool guard_clip() { static int on = -1; if (on < 0) { const char* e = getenv("TTT_GUARD_CLIP"); on = !(e && *e == '0'); } return on; }
static float zbias() { static float b = -1.0f; if (b < 0.0f) { const char* e = getenv("TTT_ZBIAS"); b = e ? (float)atof(e) : 1.0f; if (!(b >= 0.0f)) b = 0.0f; } return b; }

static int sc_n = 0, sc_act = -1;
static uint32_t sc_prim3d = 0, sc_draw = 0, sc_close = 0, sc_cull = 0, sc_spr = 0;
static int sc_requested = 0;
static uint64_t sc_frame = 0;

static uint32_t sc_lost = 0;

static void sc_note(uint32_t tex, uint32_t n) {
    for (int i = 0; i < sc_n; i++) if (sc_tex[i] == tex) { sc_tri[i] += n; sc_fmt[i] = R[0x12]; sc_orders[i]++;
        if (v_addr < sc_va0[i]) sc_va0[i] = v_addr;
        if (v_addr > sc_va1[i]) sc_va1[i] = v_addr;
        if (((psp_l32(cur_prim_addr) >> 16) & 7) == 0) sc_addr[i] = cur_prim_addr;
        sc_act = i; return; }
    if (sc_n >= ESC_N) { sc_lost++; sc_act = -1; return; }
    int i = sc_n++;
    sc_tex[i] = tex; sc_tri[i] = n; sc_pri[i] = 0;
    sc_in[i] = sc_near[i] = sc_cul[i] = sc_type[i] = 0; sc_fmt[i] = R[0x12]; sc_addr[i] = cur_prim_addr;
    sc_va0[i] = sc_va1[i] = v_addr; sc_orders[i] = 1;
    sc_x0[i] = sc_y0[i] = sc_pz0[i] = 1e30f;
    sc_x1[i] = sc_y1[i] = sc_pz1[i] = -1e30f;
    sc_a0[i] = 256; sc_a1[i] = -1;
    sc_act = i;
}

// screen box, depth range and alpha range of what reaches the rasterizer
static void sc_vertex(float x, float y, float z, uint32_t col) {
    int i = sc_act;
    if (i < 0) return;
    if (x < sc_x0[i]) sc_x0[i] = x;
    if (x > sc_x1[i]) sc_x1[i] = x;
    if (y < sc_y0[i]) sc_y0[i] = y;
    if (y > sc_y1[i]) sc_y1[i] = y;
    if (z < sc_pz0[i]) sc_pz0[i] = z;
    if (z > sc_pz1[i]) sc_pz1[i] = z;
    int a = (int)((col >> 24) & 0xFF);
    if (a < sc_a0[i]) sc_a0[i] = a;
    if (a > sc_a1[i]) sc_a1[i] = a;
}

static void sc_print() {
    printf("escena %llu: 3D enviadas %u, dibujadas %u, descartadas cerca %u cull %u | sprites %u | %d texturas, %u sin hueco\n",
           (unsigned long long)sc_frame, sc_prim3d, sc_draw, sc_close, sc_cull, sc_spr, sc_n, sc_lost);
    int order_n[ESC_N];
    for (int i = 0; i < sc_n; i++) order_n[i] = i;
    for (int i = 1; i < sc_n; i++) { int v = order_n[i], j = i - 1;
        while (j >= 0 && sc_tri[order_n[j]] < sc_tri[v]) { order_n[j + 1] = order_n[j]; j--; }
        order_n[j + 1] = v; }
    for (int k = 0; k < sc_n && k < 40; k++) {
        int i = order_n[k];
        if (!sc_tri[i]) continue;
        if (sc_a1[i] < 0) {
            printf("  tex 0x%08X vtype 0x%06X tipos 0x%02X orden en 0x%08X -> %u vertices en %u ordenes, bufer 0x%08X..0x%08X | al recorte %u, fuera %u, cull %u, rasterizadas 0\n",
                   sc_tex[i], sc_fmt[i], sc_type[i], sc_addr[i], sc_tri[i], sc_orders[i], sc_va0[i], sc_va1[i], sc_in[i], sc_near[i], sc_cul[i]);
            continue;
        }
        printf("  tex 0x%08X vtype 0x%06X tipos 0x%02X orden en 0x%08X -> %u vertices en %u ordenes, bufer 0x%08X..0x%08X | al recorte %u, fuera %u, cull %u, rasterizadas %u | caja (%.0f,%.0f)-(%.0f,%.0f) | z %.0f..%.0f | alfa %d..%d\n",
               sc_tex[i], sc_fmt[i], sc_type[i], sc_addr[i], sc_tri[i], sc_orders[i], sc_va0[i], sc_va1[i], sc_in[i], sc_near[i], sc_cul[i], sc_pri[i],
               sc_x0[i], sc_y0[i], sc_x1[i], sc_y1[i], sc_pz0[i], sc_pz1[i], sc_a0[i], sc_a1[i]);
    }
    fflush(stdout);
}
static float ge_fog_f1 = 0.0f, ge_fog_f2 = 0.0f;

static void mat43(const float* m, float x, float y, float z, float* o, bool point) {
    o[0] = x * m[0] + y * m[3] + z * m[6] + (point ? m[9] : 0);
    o[1] = x * m[1] + y * m[4] + z * m[7] + (point ? m[10] : 0);
    o[2] = x * m[2] + y * m[5] + z * m[8] + (point ? m[11] : 0);
}

static void mat44(const float* m, float x, float y, float z, float* o) {
    for (int i = 0; i < 4; i++) o[i] = x * m[i] + y * m[4 + i] + z * m[8 + i] + m[12 + i];
}

static int ge_fast2_v = -1;
static bool ge_fast2() { if (ge_fast2_v < 0) { const char* e = getenv("TTT_GE_FAST2"); ge_fast2_v = !(e && *e == '0'); } return ge_fast2_v > 0; }
// ge_fast: skinned vertices reused within a frame when the same mesh is drawn again with the same bones (split
// views, shadow passes); key = bones, vertex format and frame
struct SkinSlot { uint64_t key; uint32_t d; float p[6]; };
static const int SKIN_N = 1 << 16;
static SkinSlot* skin_tab = new SkinSlot[SKIN_N]();
static uint64_t skin_key = 0, skin_frame = 1;

static int skc_var = -1;
static bool skc_on() { if (skc_var < 0) { const char* e = getenv("TTT_GE_SKIN_CACHE"); skc_var = e && *e ? atoi(e) : 0; } return skc_var > 0; }
static uint64_t sk_n[2];   // skinned vertices computed, reused from the cache
static void skin_begin(const Fmt& f) {
    skin_key = 0;
    if (!f.wt || !patch_ge_fast.val || !skc_on()) return;
    uint64_t h = 1469598103934665603ull ^ R[0x12] ^ (skin_frame << 32);
    for (int i = 0; i < f.nw && i < 8; i++)
        for (int k = 0; k < 12; k++) { uint32_t b; memcpy(&b, &m_bone[i][k], 4); h = (h ^ b) * 1099511628211ull; }
    skin_key = h | 1;
}

static Vert read_vert(const Fmt& f, uint32_t d, float* defer = nullptr, const float (*bn)[12] = nullptr, const uint32_t* ambp = nullptr) {
    const float (*BN)[12] = bn ? bn : m_bone;
    SProf sp_(0);
    Vert v; memset(&v, 0, sizeof(v));
    v.w = 1.0f; v.src = d;
    if (f.tc) {
        if (f.tc == 1) { v.u = psp_l8(d + f.o_tc); v.v = psp_l8(d + f.o_tc + 1); }
        else if (f.tc == 2) { v.u = (float)(uint16_t)psp_l16(d + f.o_tc); v.v = (float)(uint16_t)psp_l16(d + f.o_tc + 2); }   // unsigned: 0..2 after dividing
        else { v.u = f32(psp_l32(d + f.o_tc)); v.v = f32(psp_l32(d + f.o_tc + 4)); }
        if (!f.through && f.tc != 3) { v.u /= (f.tc == 1 ? 128.0f : 32768.0f); v.v /= (f.tc == 1 ? 128.0f : 32768.0f); }
    }
    if (f.col) {
        if (f.col == 7) v.color = psp_l32(d + f.o_col);
        else v.color = convert(psp_l16(d + f.o_col), f.col - 4);
        v.has_color = true;
    } else v.color = ambp ? *ambp : (R[0x55] & 0xFFFFFFu) | ((R[0x58] & 0xFFu) << 24);   // without vertex colour the material ambient wins, as on the GE
    if (f.nrm) {
        int t = f.nrm == 3 ? 4 : f.nrm;
        v.nx = read_num(d + f.o_nrm, f.nrm, false);
        v.ny = read_num(d + f.o_nrm + t, f.nrm, false);
        v.nz = read_num(d + f.o_nrm + 2 * t, f.nrm, false);
    }
    if (f.pos) {
        int t = f.pos == 3 ? 4 : f.pos;
        v.x = read_num(d + f.o_pos, f.pos, f.through);
        v.y = read_num(d + f.o_pos + t, f.pos, f.through);
        v.z = read_num(d + f.o_pos + 2 * t, f.pos, f.through);
        if (f.through && f.pos == 2 && through_z_u16()) v.z = (float)psp_l16(d + f.o_pos + 4);
    }
    if (f.through) return v;
    SkinSlot* ss = nullptr;
    if (!defer) gp_n[0]++;
    if (f.wt && skin_key) {
        ss = &skin_tab[((d >> 2) * 2654435761u ^ (uint32_t)skin_key) & (SKIN_N - 1)];
        if (ss->key == skin_key && ss->d == d) {
            v.x = ss->p[0]; v.y = ss->p[1]; v.z = ss->p[2];
            if (f.nrm) { v.nx = ss->p[3]; v.ny = ss->p[4]; v.nz = ss->p[5]; }
            if (!defer) { gp_n[2]++; sk_n[1]++; }
            return v;
        }
    }
    // skinning in model space
    if (f.wt) {
        if (!defer) { gp_n[1]++; sk_n[0]++; }
        int t = f.wt == 3 ? 4 : f.wt;
        float px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0;
        if (ge_fast2()) {   // one blended matrix (the transform is linear in the matrix): 12 multiplies per bone instead of 18
            float M[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            for (int i = 0; i < f.nw && i < 8; i++) {
                float w = read_weight(d + f.o_wt + i * t, f.wt);
                if (w == 0.0f) continue;
                const float* m = BN[i];
                for (int k = 0; k < 12; k++) M[k] += m[k] * w;
            }
            float o[3];
            mat43(M, v.x, v.y, v.z, o, true);
            px = o[0]; py = o[1]; pz = o[2];
            if (f.nrm) { mat43(M, v.nx, v.ny, v.nz, o, false); nx = o[0]; ny = o[1]; nz = o[2]; }
        } else
        for (int i = 0; i < f.nw && i < 8; i++) {
            float w = read_weight(d + f.o_wt + i * t, f.wt);
            if (w == 0.0f) continue;
            float o[3];
            mat43(BN[i], v.x, v.y, v.z, o, true);
            px += o[0] * w; py += o[1] * w; pz += o[2] * w;
            if (f.nrm) { mat43(BN[i], v.nx, v.ny, v.nz, o, false); nx += o[0] * w; ny += o[1] * w; nz += o[2] * w; }
        }
        v.x = px; v.y = py; v.z = pz;
        if (f.nrm) { v.nx = nx; v.ny = ny; v.nz = nz; }
        if (ss && defer) { defer[0] = px; defer[1] = py; defer[2] = pz; defer[3] = v.nx; defer[4] = v.ny; defer[5] = v.nz; defer[6] = 1.0f; }
        else if (ss) { ss->key = skin_key; ss->d = d; ss->p[0] = px; ss->p[1] = py; ss->p[2] = pz; ss->p[3] = v.nx; ss->p[4] = v.ny; ss->p[5] = v.nz; }
    }
    return v;
}

// blending and tests
static inline int channel(uint32_t c, int i) { return (c >> (8 * i)) & 0xFF; }
static inline uint32_t build(int r, int g, int b, int a) {
    if (r < 0) r = 0; if (r > 255) r = 255; if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255; if (a < 0) a = 0; if (a > 255) a = 255;
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static inline bool test_alpha2(uint32_t c, bool on, uint32_t t) {
    if (!on) return true;
    int fn = t & 7, ref = (t >> 8) & 0xFF, mask = (t >> 16) & 0xFF;
    int a = (c >> 24) & mask; ref &= mask;
    switch (fn) {
    case 0: return false; case 1: return true; case 2: return a == ref; case 3: return a != ref;
    case 4: return a < ref; case 5: return a <= ref; case 6: return a > ref; default: return a >= ref;
    }
}

static bool test_alpha(uint32_t c) { return test_alpha2(c, (R[0x22] & 1) != 0, R[0xDB]); }

static int factor(int fn, int src_c, int dst_c, int sa, int da, uint32_t fixed) {
    switch (fn) {
    case 0: return dst_c; case 1: return 255 - dst_c; case 2: return sa; case 3: return 255 - sa;
    case 4: return da; case 5: return 255 - da; case 6: return sa * 2 > 255 ? 255 : sa * 2;
    case 7: return 255 - (sa * 2 > 255 ? 255 : sa * 2); case 8: return da * 2 > 255 ? 255 : da * 2;
    case 9: return 255 - (da * 2 > 255 ? 255 : da * 2);
    default: return fixed;
    }
}

static uint32_t blend_px2(uint32_t src, uint32_t dst, bool on, uint32_t b, uint32_t fixed_a, uint32_t fixed_b) {
    if (!on) return src;
    int fs = b & 15, fd = (b >> 4) & 15, eq = (b >> 8) & 7;
    int out[4];
    int sa = channel(src, 3), da = channel(dst, 3);
    for (int i = 0; i < 3; i++) {
        int s = channel(src, i), d = channel(dst, i);
        int a = (fs == 0) ? d : (fs == 1) ? 255 - d : factor(fs, s, d, sa, da, channel(fixed_a, i));
        int c = (fd == 0) ? s : (fd == 1) ? 255 - s : factor(fd, s, d, sa, da, channel(fixed_b, i));
        int r;
        switch (eq) {
        case 0: r = (s * a + d * c) / 255; break;
        case 1: r = (s * a - d * c) / 255; break;
        case 2: r = (d * c - s * a) / 255; break;
        case 3: r = s < d ? s : d; break;
        case 4: r = s > d ? s : d; break;
        default: r = abs(s - d); break;
        }
        out[i] = r;
    }
    out[3] = sa;
    return build(out[0], out[1], out[2], out[3]);
}

static uint32_t blend_px(uint32_t src, uint32_t dst) {
    return blend_px2(src, dst, (R[0x21] & 1) != 0, R[0xDF], R[0xE0], R[0xE1]);
}

static uint32_t apply_texture2(uint32_t vcol, uint32_t tcol, uint32_t f) {
    int modo = f & 7;
    bool alpha = (f >> 8) & 1;
    int dbl = (f >> 16) & 1 ? 2 : 1;
    int r, g, b, a;
    switch (modo) {
    case 0: r = channel(vcol, 0) * channel(tcol, 0) / 255; g = channel(vcol, 1) * channel(tcol, 1) / 255;
            b = channel(vcol, 2) * channel(tcol, 2) / 255; a = alpha ? channel(vcol, 3) * channel(tcol, 3) / 255 : channel(vcol, 3); break;
    case 3: r = channel(tcol, 0); g = channel(tcol, 1); b = channel(tcol, 2); a = alpha ? channel(tcol, 3) : channel(vcol, 3); break;
    case 4: r = channel(vcol, 0) + channel(tcol, 0); g = channel(vcol, 1) + channel(tcol, 1); b = channel(vcol, 2) + channel(tcol, 2);
            a = alpha ? channel(vcol, 3) * channel(tcol, 3) / 255 : channel(vcol, 3); break;
    default: { int ta = channel(tcol, 3);
            r = (channel(vcol, 0) * (255 - ta) + channel(tcol, 0) * ta) / 255; g = (channel(vcol, 1) * (255 - ta) + channel(tcol, 1) * ta) / 255;
            b = (channel(vcol, 2) * (255 - ta) + channel(tcol, 2) * ta) / 255; a = channel(vcol, 3); } break;
    }
    return build(r * dbl, g * dbl, b * dbl, a);
}

static uint32_t apply_texture(uint32_t vcol, uint32_t tcol) { return apply_texture2(vcol, tcol, R[0xC9]); }

static bool in_scissor_e(int x, int y, int ke) {
    int x0 = (int)(R[0xD4] & 0x3FF) * ke, y0 = (int)((R[0xD4] >> 10) & 0x3FF) * ke;
    int x1 = ((int)(R[0xD5] & 0x3FF) + 1) * ke - 1, y1 = ((int)((R[0xD5] >> 10) & 0x3FF) + 1) * ke - 1;
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

static bool in_scissor(int x, int y) {
    int x0 = R[0xD4] & 0x3FF, y0 = (R[0xD4] >> 10) & 0x3FF, x1 = R[0xD5] & 0x3FF, y1 = (R[0xD5] >> 10) & 0x3FF;
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

// depth buffer: 16 bits per pixel
struct ZB { uint32_t dir; int width; };
static ZB zb() {
    ZB z;
    z.dir = VRAM | (R[0x9E] & 0xFFFFFF) | ((R[0x9F] & 0xFF0000) << 8);
    z.width = R[0x9F] & 0x7FF;
    if (z.width == 0) z.width = 512;
    z.dir = high_map(z.dir);
    z.width *= scale_int();
    return z;
}

static bool test_z(const ZB& z, int x, int y, uint32_t depth) {
    uint32_t old = psp_l16(z.dir + (y * z.width + x) * 2);
    switch (R[0xDE] & 7) {
    case 0: return false; case 1: return true; case 2: return depth == old; case 3: return depth != old;
    case 4: return depth < old; case 5: return depth <= old; case 6: return depth > old; default: return depth >= old;
    }
}

static void paint_pixel(const FB& f, int x, int y, uint32_t c, bool cleared, float zf) {
    if (x < 0 || y < 0 || x >= f.width || y >= 512 * scale_int() || !in_scissor_e(x, y, scale_int())) return;
    uint32_t depth = zf < 0 ? 0 : (zf > 65535.0f ? 65535 : (uint32_t)zf);
    if (cleared) {
        uint32_t modo = R[0xD3];
        if (modo & 0x300) clear_pix(f, x, y, c, modo);
        if ((modo >> 10) & 1) { ZB z = zb(); psp_s16(z.dir + (y * z.width + x) * 2, depth); }
        return;
    }
    if (!test_alpha(c)) return;
    uint32_t dst = read_pix(f, x, y);
    uint32_t st = (dst >> 24) & 0xFF;
    int sbits = stencil_bits(f);
    bool stest = sbits && stencil_on();
    if (stest && !test_stencil(st, R[0xDC])) { write_stencil(f, x, y, stencil_op(R[0xDD] & 7, st, sbits, R[0xDC])); return; }
    if (R[0x23] & 1) {
        ZB z = zb();
        uint32_t minz = R[0xD6] & 0xFFFF, maxz = R[0xD7] & 0xFFFF;
        bool passes = depth >= minz && depth <= maxz && test_z(z, x, y, depth);
        if (!passes) { if (stest) write_stencil(f, x, y, stencil_op((R[0xDD] >> 8) & 7, st, sbits, R[0xDC])); return; }
        if (!(R[0xE7] & 1)) psp_s16(z.dir + (y * z.width + x) * 2, depth);
    }
    uint32_t out = blend_px(c, dst);
    if (stest) out = (out & 0x00FFFFFFu) | (stencil_op((R[0xDD] >> 16) & 7, st, sbits, R[0xDC]) << 24);
    else if (sbits && keep_alpha()) out = (out & 0x00FFFFFFu) | (dst & 0xFF000000u);
    write_pix(f, x, y, out);
}

// lighting: global ambient + directional/point lights, diffuse only
static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// TTT_GE_LIGHT_ATT=0: point and spot lights without distance attenuation or cone (old behaviour)
static std::atomic<int> gv_mt_active{0};
static bool ge_light_att() { static int on = -1; if (on < 0) { const char* e = getenv("TTT_GE_LIGHT_ATT"); on = !(e && *e == '0'); } return on; }
static void ge_light_note(int l, int type, float a, float b, float c, float x, float y, float z) {
    if (gv_mt_active.load(std::memory_order_relaxed)) return;
    static uint32_t seen[16]; static int n = 0;
    uint32_t k = (uint32_t)l | ((uint32_t)type << 2) | ((uint32_t)(a * 16) << 4) ^ ((uint32_t)(b * 256) << 12) ^ ((uint32_t)(c * 4096) << 20);
    for (int i = 0; i < n; i++) if (seen[i] == k) return;
    if (n >= 16) return;
    seen[n++] = k;
    printf("ge: luz %d tipo %d atenuacion %.4f %.4f %.4f en (%.2f %.2f %.2f)\n", l, type, a, b, c, x, y, z);
}

// per-primitive constants of the vertex path (GE registers change only between primitives: tc_gen)
struct VtxC {
    uint64_t gen;
    uint32_t matupd, amb_r, dif_r, alpha_r; bool lit, flip, att;
    float amb[3], emi[3];
    struct { bool on, skip; int type; float lx, ly, lz, ka, kb, kc, dx, dy, dz, dl, ex, cut, lac[3], ldc[3], ldcm[3]; bool noted; } L[4];
    float ox, oy, ke, vx, vy, vz, sx, sy, sz, zb; int zf;
    float tsu, tsv, tou, tov; int tmap, tsrc;
};
static VtxC vtx_c = {};
static void vtx_c_update(VtxC& C) {
    C.matupd = R[0x53]; C.amb_r = R[0x55]; C.dif_r = R[0x56]; C.alpha_r = R[0x58] & 0xFF;
    C.lit = R[0x17] & 1; C.flip = R[0x51] & 1; C.att = ge_light_att();
    for (int i = 0; i < 3; i++) { C.amb[i] = channel(R[0x5C], i) / 255.0f; C.emi[i] = channel(R[0x54], i) / 255.0f; }
    for (int l = 0; l < 4; l++) {
        auto& Q = C.L[l];
        Q.on = R[0x18 + l] & 1; Q.skip = false; Q.noted = false;
        if (!Q.on) continue;
        Q.type = (R[0x5F + l] >> 8) & 3;
        Q.lx = f24(R[0x63 + l * 3]); Q.ly = f24(R[0x64 + l * 3]); Q.lz = f24(R[0x65 + l * 3]);
        if (Q.type == 0) {
            float ll = std::sqrt(Q.lx * Q.lx + Q.ly * Q.ly + Q.lz * Q.lz);
            if (ll < 1e-6f) Q.skip = true; else { Q.lx /= ll; Q.ly /= ll; Q.lz /= ll; }
        }
        Q.ka = f24(R[0x7B + l * 3]); Q.kb = f24(R[0x7C + l * 3]); Q.kc = f24(R[0x7D + l * 3]);
        Q.dx = f24(R[0x6F + l * 3]); Q.dy = f24(R[0x70 + l * 3]); Q.dz = f24(R[0x71 + l * 3]);
        Q.dl = std::sqrt(Q.dx * Q.dx + Q.dy * Q.dy + Q.dz * Q.dz);
        Q.ex = f24(R[0x87 + l]); Q.cut = f24(R[0x8B + l]);
        uint32_t lac = R[0x8F + l * 3], ldc = R[0x90 + l * 3];
        for (int i = 0; i < 3; i++) { Q.lac[i] = channel(lac, i) / 255.0f; Q.ldc[i] = (float)channel(ldc, i); Q.ldcm[i] = Q.ldc[i] / 255.0f; }
    }
    C.ox = (R[0x4C] & 0xFFFF) / 16.0f; C.oy = (R[0x4D] & 0xFFFF) / 16.0f; C.ke = (float)scale_int();
    C.vx = f24(R[0x45]); C.vy = f24(R[0x46]); C.vz = f24(R[0x47]); C.sx = f24(R[0x42]); C.sy = f24(R[0x43]); C.sz = f24(R[0x44]);
    C.zb = ((R[0x23] & 1) && (R[0xE7] & 1)) ? zbias() : 0.0f; C.zf = R[0xDE] & 7;
    C.tsu = f24(R[0x48]); C.tsv = f24(R[0x49]); C.tou = f24(R[0x4A]); C.tov = f24(R[0x4B]);
    C.tmap = R[0xC0] & 3; C.tsrc = (R[0xC0] >> 8) & 3;
}

static uint32_t light_vertex(const VtxC& C, const Vert& v, float wx, float wy, float wz, float nx, float ny, float nz) {
    uint32_t mamb = (v.has_color && (C.matupd & 1)) ? v.color : C.amb_r;
    uint32_t mdif = (v.has_color && (C.matupd & 2)) ? v.color : C.dif_r;
    int alpha = (v.has_color && (C.matupd & 1)) ? channel(v.color, 3) : (int)C.alpha_r;
    if (!C.lit) return v.has_color ? v.color : ((mamb & 0xFFFFFF) | ((uint32_t)alpha << 24));
    float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
    bool no_normal = nl < 1e-6f;
    if (!no_normal) { nx /= nl; ny /= nl; nz /= nl; }
    if (C.flip) { nx = -nx; ny = -ny; nz = -nz; }
    float acc[3];
    for (int i = 0; i < 3; i++) acc[i] = C.amb[i] * channel(mamb, i) / 255.0f + C.emi[i];
    for (int l = 0; l < 4; l++) {
        const auto& Q = C.L[l];
        if (!Q.on || Q.skip) continue;
        float lx = Q.lx, ly = Q.ly, lz = Q.lz, ll = 1.0f;
        if (Q.type != 0) {
            lx -= wx; ly -= wy; lz -= wz;
            ll = std::sqrt(lx * lx + ly * ly + lz * lz);
            if (ll < 1e-6f) continue;
            lx /= ll; ly /= ll; lz /= ll;
        }
        float d = no_normal ? 1.0f : clamp01(nx * lx + ny * ly + nz * lz);
        float sc = 1.0f;
        if (Q.type != 0 && C.att) {   // PPSSPP Lighting.cpp: att = 1/(a + b d + c d^2) clamped to 0..1, spot cone dot(dir, L) >= cutoff, pow(dot, exp)
            float den = Q.ka + Q.kb * ll + Q.kc * ll * ll;
            sc = den > 0.0f ? 1.0f / den : 0.0f;
            if (!(sc > 0.0f)) sc = 0.0f; else if (sc > 1.0f) sc = 1.0f;
            if (Q.type == 2) {
                float sp = Q.dl > 0.0f ? (Q.dx * lx + Q.dy * ly + Q.dz * lz) / Q.dl : 0.0f;
                sc *= sp >= Q.cut ? (Q.ex > 0.0f ? std::pow(std::max(sp, 0.0f), Q.ex) : 1.0f) : 0.0f;
            }
            if (!Q.noted && !gv_mt_active.load(std::memory_order_relaxed)) { const_cast<bool&>(Q.noted) = true; ge_light_note(l, Q.type, Q.ka, Q.kb, Q.kc, lx * ll + wx, ly * ll + wy, lz * ll + wz); }
        }
        for (int i = 0; i < 3; i++)
            acc[i] += sc * (Q.lac[i] * channel(mamb, i) / 255.0f + d * Q.ldc[i] / 255.0f * channel(mdif, i) / 255.0f);
    }
    return build((int)(clamp01(acc[0]) * 255), (int)(clamp01(acc[1]) * 255), (int)(clamp01(acc[2]) * 255), alpha);
}

static int lvf_var = -1;
static bool lvf_on() { if (lvf_var < 0) { const char* e = getenv("TTT_GE_LIGHT_FAST"); lvf_var = e && *e ? atoi(e) : 1; } return lvf_var > 0; }
// same lighting with the divisions hoisted (reciprocals once per vertex, /255 constants per light): colours equal or 1/255 apart in ~0.04 % of random states
static uint32_t light_vertex_fast(const VtxC& C, const Vert& v, float wx, float wy, float wz, float nx, float ny, float nz) {
    uint32_t mamb = (v.has_color && (C.matupd & 1)) ? v.color : C.amb_r;
    uint32_t mdif = (v.has_color && (C.matupd & 2)) ? v.color : C.dif_r;
    int alpha = (v.has_color && (C.matupd & 1)) ? channel(v.color, 3) : (int)C.alpha_r;
    if (!C.lit) return v.has_color ? v.color : ((mamb & 0xFFFFFF) | ((uint32_t)alpha << 24));
    float nl2 = nx * nx + ny * ny + nz * nz;
    bool no_normal = nl2 < 1e-12f;
    if (!no_normal) { float inl = 1.0f / std::sqrt(nl2); nx *= inl; ny *= inl; nz *= inl; }
    if (C.flip) { nx = -nx; ny = -ny; nz = -nz; }
    float ma[3], md[3], acc[3];
    for (int i = 0; i < 3; i++) { ma[i] = channel(mamb, i) * (1.0f / 255.0f); md[i] = channel(mdif, i) * (1.0f / 255.0f); acc[i] = C.amb[i] * ma[i] + C.emi[i]; }
    for (int l = 0; l < 4; l++) {
        const auto& Q = C.L[l];
        if (!Q.on || Q.skip) continue;
        float lx = Q.lx, ly = Q.ly, lz = Q.lz, ll = 1.0f;
        if (Q.type != 0) {
            lx -= wx; ly -= wy; lz -= wz;
            float l2 = lx * lx + ly * ly + lz * lz;
            if (l2 < 1e-12f) continue;
            ll = std::sqrt(l2); float il = 1.0f / ll; lx *= il; ly *= il; lz *= il;
        }
        float d = no_normal ? 1.0f : clamp01(nx * lx + ny * ly + nz * lz);
        float sc = 1.0f;
        if (Q.type != 0 && C.att) {
            float den = Q.ka + Q.kb * ll + Q.kc * ll * ll;
            sc = den > 0.0f ? 1.0f / den : 0.0f;
            if (!(sc > 0.0f)) sc = 0.0f; else if (sc > 1.0f) sc = 1.0f;
            if (Q.type == 2) {
                float sp = Q.dl > 0.0f ? (Q.dx * lx + Q.dy * ly + Q.dz * lz) / Q.dl : 0.0f;
                sc *= sp >= Q.cut ? (Q.ex > 0.0f ? std::pow(std::max(sp, 0.0f), Q.ex) : 1.0f) : 0.0f;
            }
            if (!Q.noted && !gv_mt_active.load(std::memory_order_relaxed)) { const_cast<bool&>(Q.noted) = true; ge_light_note(l, Q.type, Q.ka, Q.kb, Q.kc, lx * ll + wx, ly * ll + wy, lz * ll + wz); }
        }
        for (int i = 0; i < 3; i++) acc[i] += sc * (Q.lac[i] * ma[i] + d * Q.ldcm[i] * md[i]);
    }
    return build((int)(clamp01(acc[0]) * 255), (int)(clamp01(acc[1]) * 255), (int)(clamp01(acc[2]) * 255), alpha);
}

// projected vertex: x,y in pixels, z in 0..65535, w = 1/w_clip, u,v in texels multiplied by 1/w
struct VP { float x, y, z, iw, u, v; uint32_t color; float vp[4]; uint32_t fn = 0; float mx = 0, my = 0, mw = 1; uint8_t mk = 0; };   // m*: previous frame screen position (smooth motion)   // fn: the model's own normal in view space (BT3), signed bytes

extern bool psp_stall_on;
void psp_stall_note(int k, uint64_t us, uint32_t size);
static inline uint64_t st_us() { return psp_stall_on ? (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0; }
struct ToonStall { uint64_t t = st_us(); ~ToonStall() { if (t) psp_stall_note(8, st_us() - t, 0); } };
// cel shading of the fighters (TTT_CEL 0 off, 1 on): view-space positions travel with each vertex, the
// normals are rebuilt from the triangles at the GL flush (the models have none) and lit by the map's sun (MAPD slot 1)
static float tc_vp[4];
static bool toon_now = false, fig_now = false;
static uint8_t glow_now = 0;
static float toon_lv[3] = {0, 0, 1};
static uint32_t toon_tint = 0, toon_rg = 0, toon_epoch = 0, toon_grp = 0, toon_tp = 0;
static int toon_who = 0, toon_face = 0, toon_aura = 0;
static bool toon_eyes = false;   // eyes held closed (Character Reference viewer, marker 0xE3 bit 10)
static bool toon_force = false;   // a generated render's capture: toon even with cel off (marker 0xE3 bit 12)
static bool toon_none = false;   // Graphics Cel shading "Off": no cel even on BT3 models (marker 0xE3 bit 13)
static bool toon_only_nrm = false;   // cel shading off: the fighter marker still comes, the cel applies only to models with normals (BT3; marker 0xE3 bit 11)
static bool toon_dmg1 = false;   // the fighter being drawn is at costume damage level 1 (marker 0xE3 bit 9)
// fighter part from its vertex address: pMdl header, part (material +0, flag +0xC, 6 = face), face blocks after it = 254
struct ToonPart { uint32_t va, hdr; int part, mat, flag; uint32_t ep, own; int opart; };
uint32_t psp_fig_mdl[8][9];
static uint32_t toon_st_hint = 0, toon_st_scan = 0; static uint64_t toon_st_us = 0;   // per fighter from cel_wrap: pMdl [ent+0x14] and the extra face pMdF [ent+0x54+4i]
static uint32_t toon_hdrs[8]; static int toon_hn = 0;
static uint32_t toon_last_hdr = 0;
static int toon_part_in(uint32_t hdr, uint32_t va, int* mat, int* flag) {
    if (psp_l32(hdr) != 0x6C644D70u && psp_l32(hdr) != 0x46644D70u) return -1;   // pMdl, pMdF
    uint32_t n = psp_l32(hdr + 0x5C), pt = psp_l32(hdr + 0x60), end = 0;
    if (pt >= 0x08000000u && pt < 0x10000000u) pt -= hdr;   // offsets are relocated to pointers when the model loads
    if (!n || n > 256 || pt > 0x100000u) return -1;   // BT3 models: one part per bone and texture
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = hdr + pt + 0x20 * i, off = psp_l32(e + 4), sz = psp_l32(e + 8);
        if (off >= 0x08000000u && off < 0x10000000u) off -= hdr;
        if (off > 0x200000u || sz > 0x200000u) return -1;
        if (va >= hdr + off && va < hdr + off + sz) {
            uint32_t key = psp_l16(e + 0x10);   // BT3 models: texture key apart from the outline material
            *mat = key & 0x8000 ? (int)(key & 0xFF) : psp_l8(e); *flag = (int)(psp_l32(e + 0xC) & 0xFFFF); return (int)i;
        }
        if (off + sz > end) end = off + sz;
    }
    if (va >= hdr + end && va < hdr + end + 0x60000u) {   // face blocks: the material comes from their own pMdF header
        *mat = -1; *flag = 6;
        PERF_SUB("cel: bloque de caras");
        if (psp_l32(hdr) == 0x6C644D70u)
            for (uint32_t a = va & ~3u; a >= hdr + end; a -= 4)
                if (psp_l32(a) == 0x46644D70u) { int fm = -1, ff = 0; if (toon_part_in(a, va, &fm, &ff) >= 0) *mat = fm; break; }
        return 254;
    }
    return -1;
}
static ToonPart toon_cache[256];
static void toon_forget() { for (auto& c : toon_cache) if (!c.hdr) c.va = 0; }   // a new model loaded where another was: the misses are stale
static int toon_part(uint32_t va, int* mat, int* flag) {
    ToonPart& c = toon_cache[(va >> 4) & 255];
    if (c.va == va && c.hdr && toon_part_in(c.own, va, mat, flag) == c.opart) { toon_last_hdr = c.hdr; return c.part; }
    if (c.va == va && !c.hdr && toon_epoch - c.ep < 600) return -1;
    PERF_SUB("cel: buscar parte");
    if (toon_who >= 1 && toon_who <= 8) {   // the model pointers of the fighter being drawn: no memory scan
        const uint32_t* h = psp_fig_mdl[toon_who - 1];
        for (int i = 0; i < 9; i++) {
            if (h[i] < 0x08000000u || h[i] >= 0x10000000u) continue;
            int p = toon_part_in(h[i], va, mat, flag);
            if (p < 0) continue;
            uint32_t m = i && h[0] >= 0x08000000u && h[0] < 0x10000000u ? h[0] : h[i];
            c = {va, m, i ? 254 : p, *mat, *flag, toon_epoch, h[i], p};
            toon_st_hint++;
            toon_last_hdr = m;
            return c.part;
        }
    }
    for (int i = 0; i < toon_hn; i++) {
        int p = toon_part_in(toon_hdrs[i], va, mat, flag);
        if (p >= 0) { c = {va, toon_hdrs[i], p, *mat, *flag, toon_epoch, toon_hdrs[i], p}; toon_last_hdr = c.hdr; return p; }
    }
    ToonStall ts_;
    struct ScanT { uint64_t t = st_us(); ~ScanT() { toon_st_scan++; toon_st_us += st_us() - t; } } sc_;
    va &= 0x0FFFFFFCu;
    for (uint32_t a = va; a >= 0x08000000u && a + 0x100000u > va; a -= 4) {   // BT3 models are bigger
        if (psp_l32(a) != 0x6C644D70u) continue;
        int p = toon_part_in(a, va, mat, flag);
        if (p < 0) break;
        toon_hdrs[toon_hn < 8 ? toon_hn++ : (int)(toon_epoch & 7)] = a;
        c = {va, a, p, *mat, *flag, toon_epoch, a, p};
        toon_last_hdr = a;
        static int logs = 0;
        if (logs < 6) {
            logs++;
            uint32_t n = psp_l32(a + 0x5C), pt = psp_l32(a + 0x60);
            if (pt >= 0x08000000u && pt < 0x10000000u) pt -= a;
            char fl[160]; int k = 0;
            for (uint32_t i = 0; i < n && k < 150; i++) k += snprintf(fl + k, sizeof(fl) - k, "%u/%u ", psp_l8(a + pt + 0x20 * i), psp_l32(a + pt + 0x20 * i + 0xC) & 0xFFFF);
            uint32_t pak = 0;
            for (uint32_t b = a - 4; b >= 0x08000000u && b + 0x80000u > a && !pak; b -= 4) {
                uint32_t cnt = __builtin_bswap32(psp_l32(b));
                if (cnt >= 3 && cnt < 1024 && (__builtin_bswap32(psp_l32(b + 12)) & ~3u) == a - b) pak = b;
            }
            printf("cel: modelo 0x%08X, %u partes (material/marca): %s| pak %s0x%08X, %u entradas, ultima 0x%08X\n", a, n, fl,
                   pak ? "" : "no hallado ", pak, pak ? __builtin_bswap32(psp_l32(pak)) : 0,
                   pak ? psp_l32(pak + (__builtin_bswap32(psp_l32(pak + 4 * __builtin_bswap32(psp_l32(pak)))) & ~3u)) : 0);
        }
        return p;
    }
    c = {va, 0, -1, -1, 0, toon_epoch, 0, -1};
    static int nlogs = 0;
    if (nlogs < 3) { nlogs++; printf("cel: vertices 0x%08X sin modelo pMdl delante\n", va); }
    return -1;
}
// TPAL: optional last entry of the character pak (Tools > Toon palettes): per material a BT3-style ramp added to the texture.
// The editor can push a live table for a model (same model signature): it wins over the pak until the game restarts.
struct TpalEnt { uint8_t mat, mode, flags, row, glow; };
struct TpalSet { uint32_t hdr, pak, hash, ep, sig; std::vector<TpalEnt> e; int live; std::array<uint8_t, 256> gl; uint32_t src = 0; };
static std::vector<TpalSet> tpal_sets;
static std::vector<std::array<uint8_t, 64>> tpal_rows;
static bool tpal_dirty = false;
static std::mutex tpal_mx;
static std::vector<std::pair<uint32_t, std::vector<uint8_t>>> tpal_live;
void psp_tpal_live(uint32_t sig, const uint8_t* d, int n) {
    std::lock_guard<std::mutex> g(tpal_mx);
    for (auto it = tpal_live.begin(); it != tpal_live.end(); ++it) if (it->first == sig) { tpal_live.erase(it); break; }
    if (d && n >= 8) tpal_live.push_back({sig, std::vector<uint8_t>(d, d + n)});
}
static std::vector<std::pair<uint32_t, std::vector<uint8_t>>> tpal_bt3;   // BT3 characters' own shading ramps (Cel BT3)
static std::map<uint32_t, std::vector<uint8_t>> tpal_src, metal_src;   // BT3 tables by source (P file or opened folder of one colour or damaged)
static std::map<uint32_t, uint32_t> bt3_pak_src;   // guest address of a character pak -> its BT3 source
void psp_bt3_pak(uint32_t pak, uint32_t src) {
    std::lock_guard<std::mutex> g(tpal_mx);
    if (src) bt3_pak_src[pak] = src; else bt3_pak_src.erase(pak);
}
int psp_tpal_bt3(uint32_t sig, uint32_t src, const uint8_t* d, int n) {   // 0 new, 1 same, 2 replaced with other ramps, 3 no ramps (kept), 4 no ramps
    std::lock_guard<std::mutex> g(tpal_mx);
    bool ok = d && n >= 8;
    if (src) { if (ok) tpal_src[src].assign(d, d + n); else tpal_src.erase(src); }
    for (auto it = tpal_bt3.begin(); it != tpal_bt3.end(); ++it) if (it->first == sig) {
        if (!ok) return 3;
        if (it->second.size() == (size_t)n && !memcmp(it->second.data(), d, n)) return 1;
        it->second.assign(d, d + n);
        return 2;
    }
    if (!ok) return 4;
    tpal_bt3.push_back({sig, std::vector<uint8_t>(d, d + n)});
    return 0;
}
// BT3 metal (Cel BT3 / Mixed BT3): per marked material a sphere and a mask, 32 atlas slots of 256x128 (sphere | mask), near/far per slot
static std::vector<std::pair<uint32_t, std::vector<uint8_t>>> metal_bt3;
static std::map<uint64_t, int> metal_slot_of;
static std::vector<std::pair<int, std::vector<uint8_t>>> metal_up;
static uint64_t metal_owner[32];
static float metal_nf[64];
static int metal_next = 0;
static void metal_forget(uint32_t sig) { for (auto s = metal_slot_of.begin(); s != metal_slot_of.end();) if ((uint32_t)(s->first >> 32) == sig) s = metal_slot_of.erase(s); else ++s; }
int psp_metal_bt3(uint32_t sig, uint32_t src, const uint8_t* d, int n) {   // 0 new, 1 same, 2 replaced, 3 none (an earlier table with this signature is kept, like the ramps), 4 none
    std::lock_guard<std::mutex> g(tpal_mx);
    bool ok = d && n >= 16 && !memcmp(d, "MTL1", 4);
    if (src) {
        auto m = metal_src.find(src);
        bool same = ok && m != metal_src.end() && m->second.size() == (size_t)n && !memcmp(m->second.data(), d, n);
        if (!same) { metal_forget(src); if (ok) metal_src[src].assign(d, d + n); else metal_src.erase(src); }
    }
    for (auto it = metal_bt3.begin(); it != metal_bt3.end(); ++it) if (it->first == sig) {
        if (!ok) return 3;
        if (it->second.size() == (size_t)n && !memcmp(it->second.data(), d, n)) return 1;
        metal_forget(sig);
        it->second.assign(d, d + n);
        return 2;
    }
    if (!ok) return 4;
    metal_forget(sig);
    metal_bt3.push_back({sig, std::vector<uint8_t>(d, d + n)});
    return 0;
}
static int metal_slot(uint32_t sig, uint32_t src, int key) {
    std::lock_guard<std::mutex> g(tpal_mx);
    uint64_t id = (uint64_t)(src ? src : sig) << 32 | (uint32_t)key;
    auto f = metal_slot_of.find(id);
    if (f != metal_slot_of.end()) return f->second;
    int slot = -1;
    std::vector<const std::vector<uint8_t>*> ts;
    if (src) { auto m = metal_src.find(src); if (m != metal_src.end()) ts.push_back(&m->second); }
    else for (auto& m : metal_bt3) if (m.first == sig) ts.push_back(&m.second);
    for (auto* tp : ts) {
        const std::vector<uint8_t>& d = *tp;
        uint32_t c; memcpy(&c, &d[12], 4);
        for (uint32_t i = 0, at = 16; i < c && at + 4 + 128 * 128 * 5 <= d.size(); i++, at += 4 + 128 * 128 * 5) {
            int k; memcpy(&k, &d[at], 4);
            if (k != key) continue;
            slot = metal_next++ & 31;
            if (metal_owner[slot]) metal_slot_of.erase(metal_owner[slot]);
            metal_owner[slot] = id;
            memcpy(&metal_nf[2 * slot], &d[4], 8);
            metal_up.push_back({slot, std::vector<uint8_t>(d.begin() + at + 4, d.begin() + at + 4 + 128 * 128 * 5)});
        }
    }
    metal_slot_of[id] = slot;
    return slot;
}
static int toon_mode();
uint32_t psp_tpal_sig(const uint8_t* m, size_t n) {   // pMdl: part count and per part material and geometry size (relocation does not touch them)
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= n) memcpy(&v, m + o, 4); return v; };
    uint32_t c = u32(0x5C), pt = u32(0x60), h = 2166136261u ^ c;
    if (pt >= 0x08000000u) return 0;
    for (uint32_t i = 0; i < c && i < 64; i++) { h = (h ^ (pt + 0x20 * i < n ? m[pt + 0x20 * i] : 0)) * 16777619u; h = (h ^ u32(pt + 0x20 * i + 8)) * 16777619u; }
    return h;
}
static uint32_t toon_sig_mem(uint32_t hdr) {
    uint32_t c = psp_l32(hdr + 0x5C), pt = psp_l32(hdr + 0x60), h = 2166136261u ^ c;
    if (pt >= 0x08000000u && pt < 0x10000000u) pt -= hdr;
    for (uint32_t i = 0; i < c && i < 64; i++) { h = (h ^ psp_l8(hdr + pt + 0x20 * i)) * 16777619u; h = (h ^ psp_l32(hdr + pt + 0x20 * i + 8)) * 16777619u; }
    return h;
}
static uint32_t toon_pak_of(uint32_t hdr) {
    uint32_t pak = 0;
    for (uint32_t b = hdr - 4; b >= 0x08000000u && b + 0x80000u > hdr && !pak; b -= 4) {
        uint32_t cnt = __builtin_bswap32(psp_l32(b));
        if (cnt >= 3 && cnt < 1024 && (__builtin_bswap32(psp_l32(b + 12)) & ~3u) == hdr - b) pak = b;
    }
    return pak;
}
static const std::vector<uint8_t>* bt3_ramps(const TpalSet& s) {   // tpal_mx held
    if (s.src) { auto t = tpal_src.find(s.src); if (t != tpal_src.end()) return &t->second; }
    for (auto& l : tpal_bt3) if (l.first == s.sig) return &l.second;
    return nullptr;
}
static uint32_t toon_tpal(uint32_t hdr, int mat) {
    if (!hdr || mat < 0) return 0;
    TpalSet* set = nullptr;
    for (auto& t : tpal_sets) if (t.hdr == hdr) set = &t;
    if (!set) { ToonStall ts_; tpal_sets.push_back({hdr, toon_pak_of(hdr), 0, toon_epoch - 1, toon_sig_mem(hdr), {}, -1}); set = &tpal_sets.back(); if (tpal_sets.size() > 16) { tpal_sets.erase(tpal_sets.begin()); set = &tpal_sets.back(); } }
    if (set->ep != toon_epoch) {   // once per fighter draw: live table from the editor, else the pak's last entry
        set->ep = toon_epoch;
        uint32_t sg = toon_sig_mem(hdr);
        if (sg != set->sig) { ToonStall ts_; set->sig = sg; set->pak = toon_pak_of(hdr); set->hash = 0; }   // another model loaded at this address
        {
            uint32_t src = 0;
            { std::lock_guard<std::mutex> g(tpal_mx); auto p = bt3_pak_src.find(set->pak); if (p != bt3_pak_src.end() && tpal_src.count(p->second)) src = p->second; }
            if (src != set->src) {
                set->src = src;
                static int logs = 0;
                if (logs++ < 40) printf("cel: tablas BT3 del modelo 0x%08X, pak 0x%08X, %s 0x%08X\n", hdr, set->pak, src ? "por origen" : "por firma", src ? src : set->sig);
            }
        }
        std::vector<uint8_t> d;
        bool live = false;
        {
            std::lock_guard<std::mutex> g(tpal_mx);
            for (auto& l : tpal_live) if (l.first == set->sig) { d = l.second; live = true; }
        }
        if (!live && set->pak) {
            uint32_t pak = set->pak, cnt = __builtin_bswap32(psp_l32(pak)), a = pak + (__builtin_bswap32(psp_l32(pak + 4 * cnt)) & ~3u);
            if (a > pak && a < pak + 0x4000000u && psp_l32(a) == 0x4C415054u) {
                uint32_t n = psp_l16(a + 6);
                if (n <= 64) { d.resize(8 + 72 * n); for (size_t i = 0; i < d.size(); i++) d[i] = psp_l8(a + (uint32_t)i); }
            }
        }
        int src = live ? 1 : d.empty() ? 0 : 2;
        auto gl_fill = [&](const std::vector<uint8_t>& t) {   // BT3 part glow: entry byte 3 per material
            uint32_t n = t.size() >= 8 && !memcmp(t.data(), "TPAL", 4) ? (uint32_t)t[6] | (uint32_t)t[7] << 8 : 0;
            for (uint32_t i = 0; i < n && i < 64 && 8 + 72 * (i + 1) <= t.size(); i++) if (t[8 + 72 * i + 3]) set->gl[t[8 + 72 * i]] = std::min<uint8_t>(t[8 + 72 * i + 3], 0x80);
        };
        set->gl.fill(0);
        {
            std::lock_guard<std::mutex> g(tpal_mx);
            if (auto t = bt3_ramps(*set)) gl_fill(*t);
        }
        if (src) gl_fill(d);
        if (d.empty() && toon_mode() == 2) {   // Cel BT3 without a TPAL of his: the BT3 file's own ramps
            std::lock_guard<std::mutex> g(tpal_mx);
            if (auto t = bt3_ramps(*set)) { d = *t; src = 3; }
        }
        uint32_t h = 2166136261u ^ (uint32_t)src;
        for (uint8_t x : d) h = (h ^ x) * 16777619u;
        if (h != set->hash) {
            set->hash = h;
            uint32_t n = d.size() >= 8 && !memcmp(d.data(), "TPAL", 4) ? (uint32_t)d[6] | (uint32_t)d[7] << 8 : 0;
            if (n > 64 || 8 + 72 * n > d.size()) n = 0;
            std::vector<uint8_t> rows;   // rows already owned by this model are rewritten in place (live edits)
            for (auto& e : set->e) rows.push_back(e.row);
            set->e.clear();
            static std::map<std::array<uint8_t, 64>, uint8_t> shared;   // BT3 ramps: one row per distinct ramp, shared by every model
            if (src == 3 || set->live == 3) rows.clear();
            if (rows.size() < n && tpal_rows.size() + n > 255) { tpal_rows.clear(); rows.clear(); shared.clear(); for (auto& t : tpal_sets) { t.e.clear(); if (&t != set) t.hash = 0; } }
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t* e = &d[8 + 72 * i];
                std::array<uint8_t, 64> r; memcpy(r.data(), e + 8, 64);
                uint8_t row;
                if (src == 3) {
                    auto it = shared.find(r);
                    if (it != shared.end() && it->second <= tpal_rows.size() && tpal_rows[it->second - 1] == r) row = it->second;
                    else { tpal_rows.push_back(r); row = (uint8_t)tpal_rows.size(); shared[r] = row; tpal_dirty = true; }
                }
                else if (i < rows.size()) { row = rows[i]; tpal_rows[row - 1] = r; }
                else { tpal_rows.push_back(r); row = (uint8_t)tpal_rows.size(); }
                tpal_dirty = true;
                set->e.push_back({e[0], e[1], e[2], row, src == 3 ? e[3] : (uint8_t)0});
            }
            static int logs = 0;
            if (logs < 16 && n && set->live != src) { logs++; printf("cel: TPAL %s, modelo 0x%08X, %u materiales\n", src == 1 ? "en vivo del editor" : src == 3 ? "BT3 del personaje" : "del pak", hdr, n); }
            set->live = src;
        }
    }
    uint32_t r = 0;
    for (auto& e : set->e) if (e.mat == mat && e.mode >= 1 && e.mode <= 3) { r = (uint32_t)e.row | (uint32_t)e.flags << 8 | (uint32_t)e.mode << 16 | (uint32_t)e.glow << 24; break; }
    if (toon_mode() == 2) {   // diagnostic: per model signature, which table it got and which materials found no ramp
        struct D { int calls = 0, hits = 0; bool done = false; std::set<int> miss; };
        static std::map<uint32_t, D> dg; static int dn = 0;
        D& q = dg[set->sig];
        if (!q.done && dn < 40) {
            q.calls++; if (r) q.hits++; else q.miss.insert(mat);
            if (q.calls >= 300) {
                q.done = true; dn++;
                bool reg = false;
                { std::lock_guard<std::mutex> g(tpal_mx); reg = bt3_ramps(*set) != nullptr; }
                std::string ms; for (int m : q.miss) { if (ms.size() > 60) { ms += " ..."; break; } ms += " " + std::to_string(m); }
                printf("cel: diagnostico BT3, modelo 0x%08X, firma 0x%08X, fuente %s, rampas BT3 registradas %s, %d materiales, %d de %d dibujos con rampa, sin rampa:%s\n",
                       hdr, set->sig, set->live == 1 ? "editor" : set->live == 2 ? "pak (.tpal)" : set->live == 3 ? "BT3" : "ninguna", reg ? "si" : "no", (int)set->e.size(), q.hits, q.calls, ms.empty() ? " ninguno" : ms.c_str());
            }
        }
    }
    return r;
}
static uint8_t toon_glow(uint32_t hdr, int mat) {
    if (!hdr || mat < 0) return 0;
    toon_tpal(hdr, mat);
    for (auto& t : tpal_sets) if (t.hdr == hdr) return t.gl[mat & 255];
    return 0;
}
static bool glow_opt() {   // Graphics "BT3 glow" (TTT_CEL_GLOW)
    static int n = 0; static bool on = true;
    if ((n++ & 63) == 0) { const char* e = getenv("TTT_CEL_GLOW"); on = !(e && *e && atof(e) <= 0.5); }
    return on;
}
// TTT_CEL: 0 off, 1 Cel shading, 2 Cel BT3, 3 Mixed (BT3 models only, Cel shading), 4 Mixed BT3 (BT3 models only, Cel BT3)
static int toon_raw() {
    static int v = 1, n = 0;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_CEL"); v = e && *e ? atoi(e) : 1; }
    return v;
}
static int toon_mode() { int v = toon_raw(); return v == 3 ? 1 : v == 4 ? 2 : v; }   // the style
static bool toon_all() { int v = toon_raw(); return v == 1 || v == 2; }   // every fighter, TTT models too

static bool tmap_warned = false;
static float tg_s = 0, tg_t = 0, tg_q = 0;
static int tmap_var = -2;

// TTT_TMAP: 0 divide by q, 1 no division, 2 ignore tgen and use vertex uv, 3 division plus scale/offset
static int tmap_variant() {
    if (tmap_var == -2) { const char* e = getenv("TTT_TMAP"); tmap_var = e ? atoi(e) : 0; }
    return tmap_var;
}

// transforms to clip space; returns x,y,z,w in o and the lit colour
// TTT_GE_FAST2=0 restores: texture size read once per primitive (any GE command or primitive invalidates it), strip vertices
// projected to the screen once instead of once per triangle
static uint64_t tc_gen = 1, tc_tex_gen = 0;
// per-prim constants keyed by a generation that moves only when a GE register takes a new value (TTT_GE_GEN: 1 on, 0 every command
// and prim, 2 alternates every 10 s and reports both; TTT_GE_GEN_CHECK=1 rebuilds every 64th reuse and counts differences)
static uint64_t tc_rgen = 1;
static uint64_t vg_st[2][5];   // per mode: lookups, rebuilds, differences, prims, cycles
static int vg_var = -1, vg_m = 0, vg_chk = -1;
static int vg_mode() {
    if (vg_var < 0) { const char* e = getenv("TTT_GE_GEN"); vg_var = e && *e ? atoi(e) : 1; const char* c = getenv("TTT_GE_GEN_CHECK"); vg_chk = c && *c == '1'; }
    if (vg_var != 2) return vg_var ? 1 : 0;
    static uint32_t n = 0; static uint64_t t0 = 0, rep = 0;
    if ((n++ & 255) == 0) {
        uint64_t t = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!t0) t0 = rep = t;
        vg_m = (int)(((t - t0) / 10000) & 1);
        if (t >= rep + 20000) {
            rep = t;
            for (int k = 0; k < 2; k++) {
                double l = vg_st[k][0] ? (double)vg_st[k][0] : 1.0, p = vg_st[k][3] ? (double)vg_st[k][3] : 1.0;
                printf("generacion: %s | %.0f%% de constantes reconstruidas, copia y pantalla %.0f ciclos por prim | diferencias %llu\n", k ? "rapida" : "anterior", 100.0 * vg_st[k][1] / l, vg_st[k][4] / p, (unsigned long long)vg_st[k][2]);
            }
            fflush(stdout);
        }
    }
    return vg_m;
}
static inline uint64_t tc_key() { return vg_mode() ? (tc_rgen | (1ull << 63)) : tc_gen; }
static inline const VtxC& vtx_cur() {
    uint64_t k = tc_key(); int m = (int)(k >> 63);
    vg_st[m][0]++;
    if (vtx_c.gen != k || (m && vtx_c.ke != (float)scale_int())) { vtx_c_update(vtx_c); vtx_c.gen = k; vg_st[m][1]++; }
    else if (m && vg_chk) {
        static uint32_t n = 0;
        if ((++n & 63) == 0) {
            static VtxC t; t = vtx_c; vtx_c_update(t);
            for (int l = 0; l < 4; l++) t.L[l].noted = vtx_c.L[l].noted;
            if (memcmp(&t, &vtx_c, sizeof(VtxC))) { vg_st[1][2]++; static int logs = 0; if (logs++ < 10) { printf("generacion: diferencia en las constantes de vertice\n"); fflush(stdout); } }
        }
    }
    return vtx_c;
}
static uint32_t tc_fn = 0;
static inline uint32_t fn_pack(float x, float y, float z) {
    auto q = [](float v) { float f = std::min(std::max(v, -1.0f), 1.0f) * 127.0f; int i = (int)f; float r = f - (float)i; if (r >= 0.5f) i++; else if (r <= -0.5f) i--; return (uint32_t)(uint8_t)(int8_t)i; };   // = lround, exact in [-127, 127]
    return q(x) | q(y) << 8 | q(z) << 16 | 1u << 24;
}
static inline void fn_unpack(uint32_t f, float* o) { for (int i = 0; i < 3; i++) o[i] = (int8_t)(f >> (8 * i)) / 127.0f; }
static int tc_tw = 0, tc_th = 0;
static inline void tc_tex_upd() {
    uint64_t k = tc_key();
    if (tc_tex_gen != k) { Tex t = tex(); tc_tw = t.w; tc_th = t.h; tc_tex_gen = k; return; }
    if ((k >> 63) && vg_chk > 0) {
        static uint32_t n = 0;
        if ((++n & 63) == 0) { Tex t = tex(); if (t.w != tc_tw || t.h != tc_th) { vg_st[1][2]++; static int logs = 0; if (logs++ < 10) { printf("generacion: diferencia en la textura %dx%d contra %dx%d\n", t.w, t.h, tc_tw, tc_th); fflush(stdout); } } }
    }
}
// smooth motion (item 70): previous clip position of each vertex, keyed by its guest address and the draw's occurrence in the frame
struct SmEnt { uint32_t src, occ, stamp; float c[4]; };
static const uint32_t SM_N = 1u << 17;
static SmEnt* sm_tab[2] = {nullptr, nullptr};
static uint32_t sm_fno = 2, sm_occ = 0;
static bool sm_on = false, sm_rec = false;
static float tc_pc[4]; static uint8_t tc_pk = 0;
static uint64_t sm_look = 0, sm_hit = 0;
struct SmOcc { uint32_t a, b, stamp, n; };
static SmOcc sm_occ_tab[4096];
static inline uint32_t sm_h(uint32_t a, uint32_t o) { uint32_t h = a * 0x9E3779B1u ^ (o + 1u) * 0x85EBCA77u; h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; return h; }
static void sm_note(uint32_t src, const float* o) {
    memcpy(tc_pc, o, sizeof(tc_pc)); tc_pk = 0;
    if (!sm_rec || !src) return;
    if (!sm_tab[0]) { sm_tab[0] = (SmEnt*)calloc(SM_N, sizeof(SmEnt)); sm_tab[1] = (SmEnt*)calloc(SM_N, sizeof(SmEnt)); if (!sm_tab[0] || !sm_tab[1]) { sm_on = sm_rec = false; return; } }
    uint32_t h = sm_h(src, sm_occ) & (SM_N - 1);
    const SmEnt* pt = sm_tab[(sm_fno - 1) & 1];
    sm_look++;
    for (uint32_t i = h, k = 0; k < 64; i = (i + 1) & (SM_N - 1), k++) {
        const SmEnt& e = pt[i];
        if (e.stamp != sm_fno - 1) break;
        if (e.src == src && e.occ == sm_occ) { memcpy(tc_pc, e.c, sizeof(tc_pc)); tc_pk = 1; sm_hit++; break; }
    }
    SmEnt* ct = sm_tab[sm_fno & 1];
    for (uint32_t i = h, k = 0; k < 64; i = (i + 1) & (SM_N - 1), k++) {
        SmEnt& e = ct[i];
        if (e.stamp != sm_fno) { e.src = src; e.occ = sm_occ; e.stamp = sm_fno; memcpy(e.c, o, sizeof(e.c)); break; }
        if (e.src == src && e.occ == sm_occ) { memcpy(e.c, o, sizeof(e.c)); break; }
    }
}

struct GvXf { float W[12], V[12], P[16], T[12]; float ax, ay, ssx, ssy, sox, soy; int split; bool toon, pnl; float ps, pox, pdy, pys; };
static void to_clip_core(const VtxC& C, const Vert& v, float* o, uint32_t* color, float* uv, float* vp, uint32_t* fnp, int tw, int th, const GvXf* X = nullptr) {
    const float* MW = X ? X->W : m_world; const float* MV = X ? X->V : m_view; const float* MP = X ? X->P : m_proj;
    const bool TN = X ? X->toon : toon_now; const int SPL = X ? X->split : split_now;
    const float AX = X ? X->ax : asp_x, AY = X ? X->ay : asp_y;
    float w[3], vi[3];
    mat43(MW, v.x, v.y, v.z, w, true);
    float n[3] = {0, 0, 0};
    if (v.nx != 0 || v.ny != 0 || v.nz != 0) mat43(MW, v.nx, v.ny, v.nz, n, false);
    *color = lvf_on() ? light_vertex_fast(C, v, w[0], w[1], w[2], n[0], n[1], n[2]) : light_vertex(C, v, w[0], w[1], w[2], n[0], n[1], n[2]);
    *fnp = 0;
    if (TN && (n[0] != 0 || n[1] != 0 || n[2] != 0)) {
        float q[3]; mat43(MV, n[0], n[1], n[2], q, false);
        float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        if (l > 1e-9f) *fnp = fn_pack(q[0] / l, q[1] / l, q[2] / l);
    }
    mat43(MV, w[0], w[1], w[2], vi, true);
    vp[0] = vi[0]; vp[1] = vi[1]; vp[2] = vi[2]; vp[3] = 0.0f;
    mat44(MP, vi[0], vi[1], vi[2], o);
    if (AX != 1.0f) o[0] *= AX;     // field of view widened to the window
    if (AY != 1.0f) o[1] *= AY;
    if (SPL) { if (X) { o[0] = o[0] * X->ssx + X->sox * o[3]; o[1] = o[1] * X->ssy + X->soy * o[3]; } else { o[0] = o[0] * sp_sx + sp_ox * o[3]; o[1] = o[1] * sp_sy + sp_oy * o[3]; } }
    if (X ? X->pnl : pnl_on) { if (X) { o[0] = o[0] * X->ps + X->pox * o[3]; o[1] = o[1] * X->ps + X->pdy * X->pys * o[3]; } else pnl_apply(o); }
    if (C.tmap == 1) {                                  // coordinates generated with the texture matrix
        int source = C.tsrc;
        float in[3], g[3];
        if (source == 0) { in[0] = v.x; in[1] = v.y; in[2] = v.z; }
        else if (source == 1) { in[0] = v.u; in[1] = v.v; in[2] = 0.0f; }
        else {
            in[0] = v.nx; in[1] = v.ny; in[2] = v.nz;
            if (source == 2) {
                float l = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
                if (l > 1e-8f) { in[0] /= l; in[1] /= l; in[2] /= l; }
            }
        }
        mat43(X ? X->T : m_tgen, in[0], in[1], in[2], g, true);
        if (vp == tc_vp) { tg_s = g[0]; tg_t = g[1]; tg_q = g[2]; }
        int md = tmap_variant();
        float s = g[0], r = g[1];
        if (md != 1 && (g[2] > 1e-8f || g[2] < -1e-8f)) { s /= g[2]; r /= g[2]; }
        if (md == 2) { s = v.u; r = v.v; }
        if (md == 3) { s = s * C.tsu + C.tou; r = r * C.tsv + C.tov; }
        uv[0] = s * tw;
        uv[1] = r * th;
        return;
    }
    uv[0] = (v.u * C.tsu + C.tou) * tw;
    uv[1] = (v.v * C.tsv + C.tov) * th;
}

static void to_clip(const Vert& v, float* o, uint32_t* color, float* uv) {
    SProf sp_(1);
    gp_n[3]++;
    int tw, th;
    if (ge_fast2()) {
        tc_tex_upd();
        tw = tc_tw; th = tc_th;
    } else { Tex t = tex(); tw = t.w; th = t.h; }
    if ((R[0xC0] & 3) == 2 && !tmap_warned) { tmap_warned = true; warning("TMAP de entorno sin cubrir", R[0xC0] & 3); }
    to_clip_core(vtx_cur(), v, o, color, uv, tc_vp, &tc_fn, tw, th);
    sm_note(v.src, o);
}

static VP to_screen_c(const VtxC& C, const float* c, uint32_t color, const float* uv, bool plain = false) {
    VP p;
    float iw = 1.0f / c[3];
    float ox = C.ox, oy = C.oy;
    float ke = C.ke;
    p.x = (C.vx + C.sx * c[0] * iw - ox) * ke;
    p.y = (C.vy + C.sy * c[1] * iw - oy) * ke;
    if (!plain) {
    if (pnl_on && cap_face_prim && !cap_zoom) { float x = p.x / ke, y = p.y / ke; pnl_face[0] = std::min(pnl_face[0], x); pnl_face[1] = std::min(pnl_face[1], y); pnl_face[2] = std::max(pnl_face[2], x); pnl_face[3] = std::max(pnl_face[3], y); }
    if (cap_zoom && cap_face_prim) { float x = p.x / ke, y = p.y / ke; cap_face[0] = std::min(cap_face[0], x); cap_face[1] = std::min(cap_face[1], y); cap_face[2] = std::max(cap_face[2], x); cap_face[3] = std::max(cap_face[3], y); }
    if (cap_zoom) { p.x = ((p.x / ke - cap_rx) * cap_kz + cap_sx0) * ke; p.y = ((p.y / ke - cap_ry) * cap_kz + cap_sy0) * ke; }
    if (rm_zoom) { p.x = ((p.x / ke - rm_bx) * rm_kz + rm_full[0]) * ke; p.y = ((p.y / ke - rm_by) * rm_kz + rm_full[1]) * ke; }
    }
    p.z = C.vz + C.sz * c[2] * iw;
    if (C.zb > 0.0f) {
        if (C.zf == 6 || C.zf == 7) p.z += C.zb; else if (C.zf == 4 || C.zf == 5) p.z -= C.zb;
    }
    p.iw = iw;
    p.u = uv[0] * iw; p.v = uv[1] * iw;
    p.color = color;
    return p;
}
static VP to_screen(const float* c, uint32_t color, const float* uv) { return to_screen_c(vtx_cur(), c, color, uv); }

// head and waist of the captured model on the screen (the skinned path of a vertex: world, view, projection, panel, viewport),
// then the zoom box and the points in pixels of the rect read back
static void cap_points() {
    cap_pt_done = true;
    float sp[3][2];
    for (int k = 0; k < 3; k++) {
        float w[3], vi[3], o[4];
        mat43(m_world, cap_pt[k][0], cap_pt[k][1], cap_pt[k][2], w, true);
        mat43(m_view, w[0], w[1], w[2], vi, true);
        mat44(m_proj, vi[0], vi[1], vi[2], o);
        if (asp_x != 1.0f) o[0] *= asp_x;
        if (asp_y != 1.0f) o[1] *= asp_y;
        if (pnl_on) pnl_apply(o);
        float iw = o[3] != 0.0f ? 1.0f / o[3] : 1.0f, ox = (R[0x4C] & 0xFFFF) / 16.0f, oy = (R[0x4D] & 0xFFFF) / 16.0f;
        sp[k][0] = f24(R[0x45]) + f24(R[0x42]) * o[0] * iw - ox; sp[k][1] = f24(R[0x46]) + f24(R[0x43]) * o[1] * iw - oy;
    }
    const float A = 272.0f / 176.0f * rf::MARGIN / (rf::MARGIN + rf::MARGIN_BOTTOM);
    float sw = std::min(480.0f, 272.0f * A), sh = sw / A;
    cap_sx0 = (480.0f - sw) * 0.5f; cap_sy0 = (272.0f - sh) * 0.5f; cap_sx1 = cap_sx0 + sw; cap_sy1 = cap_sy0 + sh;
    float span = std::max(4.0f, sp[1][1] - sp[0][1]), bx, by, W, H;   // the capture is the big render box plus a margin: the whole internal resolution goes into it
    float ey = rf::eye_y(sp[0][1], cap_pt[2][0] < 1e8f ? sp[2][1] : -1.0f, span, cap_bk);
    rf::big(sp[0][0], ey, span, cap_bk, cap_tw, &bx, &by, &W, &H);
    float m = rf::MARGIN;
    cap_rx = bx - W * (m - 1.0f) * 0.5f; cap_ry = by - H * (m - 1.0f) * 0.5f; cap_kz = sh / (H * (m + rf::MARGIN_BOTTOM));
    cap_out_u = cap_bk;
    cap_zoom = true;
    float ke = (float)scale_int();
    for (int k = 0; k < 3; k++) { cap_px[k][0] = (sp[k][0] - cap_rx) * cap_kz * ke; cap_px[k][1] = (sp[k][1] - cap_ry) * cap_kz * ke; }
    static int logs = 0;
    if (logs++ < 8) printf("render: cabeza en pantalla (%.1f, %.1f), cintura (%.1f, %.1f), cuello (%.1f, %.1f), ojos y %.1f, distancia relativa %.2f, caja %.1fx%.1f, zoom %.2f\n", sp[0][0], sp[0][1], sp[1][0], sp[1][1], sp[2][0], sp[2][1], ey, cap_bk, W, H, cap_kz);
    cap_face[0] = cap_face[1] = 1e9f; cap_face[2] = cap_face[3] = -1e9f;
}
// game room: the model's big render box (render_frame.h, as the generated big) onto the whole render of its row
static void room_points() {
    RoomS r;
    { std::lock_guard<std::mutex> g(rm_mx); r = rm_s[rm_k & 3]; }
    float sp[3][2];
    for (int k = 0; k < 3; k++) {
        float w[3], vi[3], o[4];
        mat43(m_world, r.pt[k][0], r.pt[k][1], r.pt[k][2], w, true);
        mat43(m_view, w[0], w[1], w[2], vi, true);
        mat44(m_proj, vi[0], vi[1], vi[2], o);
        if (asp_x != 1.0f) o[0] *= asp_x;
        if (asp_y != 1.0f) o[1] *= asp_y;
        float iw = o[3] != 0.0f ? 1.0f / o[3] : 1.0f, ox = (R[0x4C] & 0xFFFF) / 16.0f, oy = (R[0x4D] & 0xFFFF) / 16.0f;
        sp[k][0] = f24(R[0x45]) + f24(R[0x42]) * o[0] * iw - ox; sp[k][1] = f24(R[0x46]) + f24(R[0x43]) * o[1] * iw - oy;
    }
    float span = std::max(4.0f, sp[1][1] - sp[0][1]), bx, by, W, H;
    float ey = rf::eye_y(sp[0][1], r.pt[2][0] < 1e8f ? sp[2][1] : -1.0f, span, r.rw);
    rf::big(sp[0][0], ey, span, r.rw, r.tw, &bx, &by, &W, &H);
    float fx, fy, fw, fh;   // the face as the character's mid render frames it (his per character values): its centre on the row's point
    rf::face(rf::MID, sp[0][0], ey, span, r.rw, 68, 48, &fx, &fy, &fw, &fh);
    rf::tweak_box(r.tw, &fx, &fy, &fw, &fh, 68, 48);
    rm_done = true; rm_zoom = rm_show;
    rm_bx = fx + fw * 0.5f; rm_by = fy + fh * 0.5f; rm_kz = rm_full[2] / std::max(1.0f, W);
    rm_full[0] = rm_tgt[0]; rm_full[1] = rm_tgt[1] - r.tw.rup;
    static int logs = 0;
    if (rm_show && logs++ < 8) printf("gpu: sala: modelo de la fila %d, cabeza (%.1f, %.1f), cintura (%.1f, %.1f), caja %.1fx%.1f, zoom %.2f, cara (%.1f, %.1f) al punto (%.1f, %.1f)\n", rm_k, sp[0][0], sp[0][1], sp[1][0], sp[1][1], W, H, rm_kz, rm_bx, rm_by, rm_full[0], rm_full[1]);
}

// mipmap chain generated by us: the game uploads a single level and the sky
// repeats 48 times, so without mipmaps each pixel lands on a random texel
struct Chain { uint64_t key; uint32_t dir; int nn; int aw[12], ah[12]; std::vector<uint32_t> px[12]; unsigned gl; int glw, glh; uint32_t avg; bool avg_ok; uint64_t stamp; size_t bytes; bool pin; };
#define N_CACHE 1024
#define N_CACHE_OLD 192
static Chain cache[N_CACHE];
static int cache_sig = 0;
static std::atomic<uint32_t> cache_gen[N_CACHE];
static std::unordered_map<uint64_t, int> cache_map;
static uint64_t cache_clock = 0;
static size_t cache_bytes = 0;
std::atomic<int> psp_tex_pin_req{0};
static int cache_pins = 0;
static int tex_pin_max() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_TEX_PIN"); v = e && *e ? atoi(e) : 96; } return v; }   // render big textures kept out of the LRU (0 off)
static void glr_tex_free(unsigned t);
static void glr_upload_async(Chain& c, int slot);

// when minifying, averaged alpha sinks foliage below the test: each level is
// rescaled so the fraction of texels passing the test stays the same
static void keep_coverage(std::vector<uint32_t>& px, const std::vector<uint32_t>& base) {
    size_t nb = 0;
    for (size_t i = 0; i < base.size(); i++) if ((base[i] >> 24) >= 128) nb++;
    if (!nb || nb == base.size() || px.empty()) return;
    double target_v = (double)nb / (double)base.size();
    float lo = 0.0f, hi = 8.0f;
    for (int it = 0; it < 12; it++) {
        float k = (lo + hi) * 0.5f;
        size_t n = 0;
        for (size_t i = 0; i < px.size(); i++) if ((float)(px[i] >> 24) * k >= 128.0f) n++;
        if ((double)n / (double)px.size() < target_v) lo = k; else hi = k;
    }
    float k = (lo + hi) * 0.5f;
    for (size_t i = 0; i < px.size(); i++) {
        int a = (int)((float)(px[i] >> 24) * k);
        if (a > 255) a = 255;
        px[i] = (px[i] & 0x00FFFFFFu) | ((uint32_t)a << 24);
    }
}

// texture exposed as png: disc/disc.h installs the lookup by the marker it left in the data
bool (*psp_tex_png)(const uint8_t*, uint32_t, uint32_t, int, int, int, int, int, int*, int*, const uint32_t**) = nullptr;
uint32_t (*psp_tex_variant)(const uint8_t*) = nullptr;
uint32_t (*psp_tex_matid)(const uint8_t*, int) = nullptr;
uint32_t (*psp_tex_dmg1)(const uint8_t*, uint32_t) = nullptr;
uint32_t psp_tex_force = 0;
static int prim_mat = -1;   // material of the fighter part being drawn (item 28), -1 elsewhere
static bool prim_blink = false;   // face part of a fighter with its eyes closed right now (item 29.2)
// blinking: every fighter closes its eyes at random moments (TTT_BLINK); the face textures swap to the blink texture of the character
static bool blink_now(int k) {
    static int on = 1, n = 0;
    if ((n++ & 1023) == 0) { const char* e = getenv("TTT_BLINK"); on = e && *e ? atoi(e) : 1; }
    if (on <= 0 || k < 0 || k >= 8) return false;
    static uint64_t next[8], until[8], seed = 0x9E3779B97F4A7C15ull;
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto rnd = [&](uint64_t a, uint64_t b) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return a + seed % (b - a + 1); };
    if (!next[k] || now > next[k] + 10000) next[k] = now + rnd(800, 4000);
    if (now >= next[k]) { until[k] = now + rnd(90, 140); next[k] = rnd(0, 99) < 15 ? until[k] + rnd(120, 200) : now + rnd(2000, 6000); }
    return now < until[k];
}
// Material editor: a texture per (model signature, material) pushed live; "off" hides a material texture until restart
struct MatLive { uint32_t sig; int mat; bool off; int w, h; std::vector<uint32_t> px; uint32_t gen; };
static std::vector<MatLive> mat_live;
static std::mutex mat_live_mx;
static uint32_t mat_live_gen = 0;
void psp_mattex_live(uint32_t sig, int mat, const uint32_t* px, int w, int h) {
    std::lock_guard<std::mutex> g(mat_live_mx);
    MatLive* e = nullptr;
    for (auto& x : mat_live) if (x.sig == sig && x.mat == mat) e = &x;
    if (!e) { mat_live.push_back({sig, mat, true, 0, 0, {}, 0}); e = &mat_live.back(); }
    e->off = !px || w <= 0 || h <= 0; e->w = w; e->h = h; e->gen = ++mat_live_gen;
    if (e->off) e->px.clear(); else e->px.assign(px, px + (size_t)w * h);
}


static uint64_t tex_key(const Tex& t) {
    uint64_t k = t.dir;
    k = k * 1000003u + (uint64_t)t.fmt * 131 + (uint64_t)t.w * 17 + (uint64_t)t.h * 7 + t.stride * 3 + t.swz;
    if (t.fmt >= 4) { k = k * 1000003u + clut_bytes + (uint64_t)(R[0xC5] & 0xFFFFFF) * 7;
                      for (int i = 0; i < 32; i++) k = k * 1000003u + clut_raw[i * 31]; }
    int bpp = bpp_tex(t.fmt);
    uint32_t bytes = (uint32_t)t.stride * t.h * bpp / 8;
    for (int i = 0; i < 24; i++) k = k * 1000003u + psp_l32(t.dir + (bytes / 24) * i);   // content signature
    if (psp_tex_variant) k = k * 1000003u + psp_tex_variant(psp_p(t.dir));
    return k;
}

static int cache_viva[N_CACHE];

// average colour of a decoded texture (alpha weighted), for the light that effects cast on the fighters
static uint32_t chain_avg(const Chain* cc) {
    Chain* c = const_cast<Chain*>(cc);
    if (c->avg_ok) return c->avg;
    size_t n = c->px[0].size(), st = n > 4096 ? n / 4096 : 1;
    double r = 0, g = 0, b = 0, a = 0; size_t k = 0;
    for (size_t i = 0; i < n; i += st, k++) { uint32_t p = c->px[0][i]; double w = (p >> 24) / 255.0; r += (p & 255) * w; g += ((p >> 8) & 255) * w; b += ((p >> 16) & 255) * w; a += w; }
    uint32_t A = k ? (uint32_t)(a / k * 255.0) : 0;
    c->avg = a > 0 ? (uint32_t)(r / a) | (uint32_t)(g / a) << 8 | (uint32_t)(b / a) << 16 | A << 24 : 0;
    c->avg_ok = true;
    return c->avg;
}

static uint32_t tex_avg(const Tex& t) {   // average colour sampled on an 8x8 grid, cached by content signature
    static std::unordered_map<uint64_t, uint32_t> m;
    uint64_t k = tex_key(t);
    auto it = m.find(k);
    if (it != m.end()) return it->second;
    if (m.size() > 4096) m.clear();
    double r = 0, g = 0, b = 0, a = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            uint32_t p = texel_n(t, 0, (2 * x + 1) * t.w / 16, (2 * y + 1) * t.h / 16);
            double w = (p >> 24) / 255.0; r += (p & 255) * w; g += ((p >> 8) & 255) * w; b += ((p >> 16) & 255) * w; a += w;
        }
    uint32_t v = a > 0 ? (uint32_t)(r / a) | (uint32_t)(g / a) << 8 | (uint32_t)(b / a) << 16 | (uint32_t)(a / 64.0 * 255.0) << 24 : 1u;
    m[k] = v;
    return v;
}

static int chain_mips(std::vector<uint32_t>* px, int* aw, int* ah, int mm) {
    int n = 1;
    while (n < 12 && (aw[n - 1] > 1 || ah[n - 1] > 1)) {
        int pw = aw[n - 1], ph = ah[n - 1];
        int w = pw > 1 ? pw / 2 : 1, h = ph > 1 ? ph / 2 : 1;
        aw[n] = w; ah[n] = h;
        px[n].resize((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int x0 = pw > 1 ? x * 2 : 0, y0 = ph > 1 ? y * 2 : 0;
                int x1 = pw > 1 ? x0 + 1 : 0, y1 = ph > 1 ? y0 + 1 : 0;
                uint32_t a = px[n - 1][(size_t)y0 * pw + x0], b = px[n - 1][(size_t)y0 * pw + x1];
                uint32_t d = px[n - 1][(size_t)y1 * pw + x0], e = px[n - 1][(size_t)y1 * pw + x1];
                int o[4];
                for (int i = 0; i < 4; i++) o[i] = (channel(a, i) + channel(b, i) + channel(d, i) + channel(e, i) + 2) / 4;
                px[n][(size_t)y * w + x] = build(o[0], o[1], o[2], o[3]);
            }
        n++;
    }
    if (mm == 2) for (int i = 1; i < n; i++) keep_coverage(px[i], px[0]);
    return n;
}

static size_t cache_budget() {
    static size_t b = 0;
    if (!b) { const char* e = getenv("TTT_TEXCACHE_MB"); int mb = e && atoi(e) > 0 ? atoi(e) : 1536; b = (size_t)mb << 20; }
    return b;
}

static void cache_drop(int i, bool free_gl) {
    Chain& c = cache[i];
    if (!c.nn) return;
    cache_map.erase(c.key);
    cache_bytes -= std::min(cache_bytes, c.bytes);
    c.bytes = 0; c.nn = 0;
    if (c.pin) { c.pin = false; cache_pins--; }
    cache_gen[i].fetch_add(1);
    if (free_gl) {
        if (c.gl) glr_tex_free(c.gl);
        c.gl = 0;
        for (auto& v : c.px) std::vector<uint32_t>().swap(v);
    }
}

// least recently used entry the pending queue does not reference
static int cache_victim() {
    if (!patch_tex_lru.val) { int i = cache_sig; cache_sig = (cache_sig + 1) % N_CACHE_OLD; return i; }
    int best = -1; uint64_t bs = ~0ull;
    for (int i = 0; i < N_CACHE; i++) {
        if (!cache[i].nn) return i;
        if (!cache_viva[i] && !cache[i].pin && cache[i].stamp < bs) { bs = cache[i].stamp; best = i; }
    }
    if (best < 0) for (int i = 0; i < N_CACHE; i++) if (!cache_viva[i] && cache[i].stamp < bs) { bs = cache[i].stamp; best = i; }
    if (best < 0) { best = cache_sig; cache_sig = (cache_sig + 1) % N_CACHE; }
    return best;
}

static void cache_trim(int keep) {
    if (!patch_tex_lru.val) return;
    while (cache_bytes > cache_budget()) {
        int best = -1; uint64_t bs = ~0ull;
        for (int i = 0; i < N_CACHE; i++) if (i != keep && cache[i].nn && !cache_viva[i] && !cache[i].pin && cache[i].stamp < bs) { bs = cache[i].stamp; best = i; }
        if (best < 0) for (int i = 0; i < N_CACHE; i++) if (i != keep && cache[i].nn && !cache_viva[i] && cache[i].stamp < bs) { bs = cache[i].stamp; best = i; }
        if (best < 0) break;
        cache_drop(best, true);
    }
}

static size_t chain_bytes(const Chain& c, int up) {
    size_t b = 0;
    for (int i = 0; i < c.nn; i++) b += (size_t)c.aw[i] * c.ah[i] * 4;
    size_t p0 = (size_t)c.aw[0] * c.ah[0] * 4;
    return b + (up == 2 ? p0 * 20 : up == 1 ? p0 * 4 : 0);
}

extern uint32_t ly_tex_force;
// port memory handed back (HUD textures and buffers of a bar mode): the cached textures read from it are dropped, so a new
// picture in the same addresses is never taken for the old one (the key only samples 24 words)
static std::mutex forget_mx;
static std::vector<std::pair<uint32_t, uint32_t>> forget_q;
static std::atomic<int> forget_n{0};
void psp_gpu_tex_forget(uint32_t a, uint32_t n) { std::lock_guard<std::mutex> g(forget_mx); forget_q.push_back({a, n}); forget_n.store(1); }
static void forget_run() {
    std::vector<std::pair<uint32_t, uint32_t>> q;
    { std::lock_guard<std::mutex> g(forget_mx); q.swap(forget_q); forget_n.store(0); }
    bool flushed = false;
    int dropped = 0;
    for (int i = 0; i < N_CACHE; i++) {
        if (!cache[i].nn) continue;
        for (auto& r : q) if (cache[i].dir >= r.first && cache[i].dir - r.first < r.second) {
            if (!flushed) { psp_gpu_flush_by(PC_VTEX); flushed = true; }
            cache_drop(i, false); dropped++;
            break;
        }
    }
    static int logs = 0;
    if (dropped && logs++ < 10) printf("tex: %d texturas de memoria liberada del port fuera de la cache\n", dropped);
}
static const Chain* chain_of(const Tex& t) {
    if (forget_n.load(std::memory_order_relaxed)) forget_run();
    uint64_t k = tex_key(t);
    uint32_t force = prim_mat >= 0 && psp_tex_matid ? psp_tex_matid(psp_p(t.dir), prim_blink ? prim_mat | 0x1000 : prim_mat) : 0;
    if (toon_dmg1 && toon_who && psp_tex_dmg1) {
        uint32_t d1 = psp_tex_dmg1(psp_p(t.dir), force);
        if (d1) {
            force = d1;
            static int logs = 0;
            if (logs < 8) { logs++; printf("texture: rotura nivel 1, luchador %d con textura %u\n", toon_who - 1, d1); }
        }
    }
    if (!force && ly_tex_force && prim_mat < 0) force = ly_tex_force;   // BT3 stage texture animation
    if (force) k = k * 1000003u + 0x5A5A000000ull + force;
    const MatLive* live = nullptr;
    std::unique_lock<std::mutex> live_lk(mat_live_mx, std::defer_lock);
    if (prim_mat >= 0 && !mat_live.empty() && toon_last_hdr) {
        static uint32_t lh = 0, ls = 0;
        if (lh != toon_last_hdr) { lh = toon_last_hdr; ls = toon_sig_mem(lh); }
        live_lk.lock();
        for (auto& x : mat_live) if (x.sig == ls && x.mat == prim_mat) live = &x;
        if (live) { force = 0; k = k * 1000003u + 0x3C3C000000ull + live->gen; if (live->off) live = nullptr; }
        else live_lk.unlock();
    }
    bool tint = !force && !live && prim_mat >= 0 && patch_char_textures.val == 2;   // TTT_CHAR_TEXTURES=2: each material tinted to show its number
    if (tint) k = k * 1000003u + 0x7B7B000000ull + (uint64_t)prim_mat;
    // png textures: the same served image under another address or content signature reuses its entry (alias)
    static std::unordered_map<uint64_t, std::pair<int, uint64_t>> cache_alias;
    static std::unordered_map<const uint32_t*, std::pair<int, uint64_t>> cache_by_px;
    if (patch_tex_lru.val) {
        auto it = cache_map.find(k);
        if (it != cache_map.end() && cache[it->second].nn && cache[it->second].key == k) {
            Chain& h = cache[it->second];
            cache_viva[it->second] = 1; h.stamp = ++cache_clock;
            return &h;
        }
        auto ia = cache_alias.find(k);
        if (ia != cache_alias.end() && cache[ia->second.first].nn && cache[ia->second.first].key == ia->second.second) {
            Chain& h = cache[ia->second.first];
            cache_viva[ia->second.first] = 1; h.stamp = ++cache_clock;
            return &h;
        }
    } else
        for (int i = 0; i < N_CACHE_OLD; i++) if (cache[i].nn && cache[i].key == k) { cache_viva[i] = 1; return &cache[i]; }
    int pw = 0, ph = 0; const uint32_t* ppx = nullptr;
    uint64_t t0 = st_us();
    psp_tex_force = force;
    bool from_png = false;
    if (live) { pw = live->w; ph = live->h; ppx = live->px.data(); from_png = true; }
    else { psp_tex_pin_req.store(0); from_png = psp_tex_png && psp_tex_png(psp_p(t.dir), t.dir, clut_dir, t.w, t.h, t.stride, t.swz, t.fmt, &pw, &ph, &ppx); }
    const bool pin_now = psp_tex_pin_req.exchange(0) != 0 && from_png && !live && !tint;
    psp_tex_force = 0;
    if (from_png && !live && !tint && patch_tex_lru.val && ppx && pw > 0 && ph > 0) {
        auto ib = cache_by_px.find(ppx);
        if (ib != cache_by_px.end()) {
            int i = ib->second.first;
            Chain& h = cache[i];
            bool same = h.nn && h.key == ib->second.second && h.aw[0] == pw && h.ah[0] == ph && h.px[0].size() == (size_t)pw * ph;
            same = same && memcmp(h.px[0].data(), ppx, (size_t)pw * ph * 4) == 0;   // the same pointer can hold another image (shared buffers)
            if (same) {
                if (cache_alias.size() > 65536) cache_alias.clear();
                cache_alias[k] = ib->second;
                cache_viva[i] = 1; h.stamp = ++cache_clock;
                return &h;
            }
        }
    }
    int mine = cache_victim();
    if (cache[mine].nn) {
        if (!patch_tex_lru.val || cache_viva[mine]) psp_gpu_flush_by(PC_VTEX);   // an entry the queue may still use cannot be recycled
        cache_drop(mine, false);
    }
    Chain& c = cache[mine];
    cache_viva[mine] = 1;
    c.key = k; c.dir = t.dir; c.avg_ok = false; c.stamp = ++cache_clock;
    if (pin_now && cache_pins < tex_pin_max()) {
        c.pin = true; cache_pins++;
        static int pl = 0; if (pl < 3) { pl++; printf("tex: render big fijado en la cache (%d de %d)\n", cache_pins, tex_pin_max()); }
    }
    c.aw[0] = t.w; c.ah[0] = t.h;
    if (from_png) {
        c.aw[0] = pw; c.ah[0] = ph;
        c.px[0].assign(ppx, ppx + (size_t)pw * ph);
        if (t0) psp_stall_note(0, st_us() - t0, (uint32_t)pw * ph);
    } else {
        c.px[0].resize((size_t)t.w * t.h);
        PERF(PZ_TEX);
        for (int y = 0; y < t.h; y++)
            for (int x = 0; x < t.w; x++) c.px[0][(size_t)y * t.w + x] = texel_n(t, 0, x, y);
        if (t0) psp_stall_note(1, st_us() - t0, (uint32_t)t.w * t.h);
    }
    if (tint) {
        static const uint32_t TC[8] = {0x4040FF, 0x40FF40, 0xFF4040, 0x40FFFF, 0xFF40FF, 0xFFFF40, 0x4080FF, 0xFFFFFF};
        uint32_t tc = TC[prim_mat & 7];
        float kk = prim_mat >= 8 ? 0.45f : 0.75f;
        for (auto& p : c.px[0]) {
            uint32_t o = p & 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8) { float v = (float)((p >> sh) & 255) * (1.0f - kk) + (float)((tc >> sh) & 255) * kk; o |= (uint32_t)std::min(255.0f, v) << sh; }
            p = o;
        }
        static uint64_t seen = 0;
        if (!(seen >> (prim_mat & 63) & 1)) { seen |= 1ull << (prim_mat & 63); static const char* TN[8] = {"rojo", "verde", "azul", "amarillo", "magenta", "cian", "naranja", "blanco"}; printf("tex: material %d de un luchador, tinte %s%s\n", prim_mat, TN[prim_mat & 7], prim_mat >= 8 ? " suave" : ""); }
    }
    bool in_gl = glr_enabled();
    uint32_t px0 = (uint32_t)c.aw[0] * c.ah[0];
    if (patch_tex_lru.val) {
        cache_map[k] = mine;
        if (from_png && !live && !tint && ppx) { if (cache_by_px.size() > 65536) cache_by_px.clear(); cache_by_px[ppx] = {mine, k}; }
    }
    if (in_gl && patch_tex_async.val) {
        t0 = st_us();
        c.nn = 1;
        glr_upload_async(c, mine);
        if (t0) psp_stall_note(3, st_us() - t0, px0);
        cache_bytes += c.bytes;
        cache_trim(mine);
        return &c;
    }
    if (in_gl && glr_mips() == 0) {
        t0 = st_us();
        c.nn = 1; c.gl = glr_upload_chain(c, c.gl, &c.glw, &c.glh);
        if (t0) psp_stall_note(3, st_us() - t0, px0);
        c.bytes = chain_bytes(c, glr_up_of(c.aw[0], c.ah[0]));
        cache_bytes += c.bytes;
        cache_trim(mine);
        return &c;
    }
    t0 = st_us();
    c.nn = chain_mips(c.px, c.aw, c.ah, in_gl ? glr_mips() : 0);
    if (in_gl) {
        if (t0) psp_stall_note(2, st_us() - t0, px0);
        t0 = st_us();
        c.gl = glr_upload_chain(c, c.gl, &c.glw, &c.glh);
        if (t0) psp_stall_note(3, st_us() - t0, px0);
    }
    c.bytes = chain_bytes(c, in_gl ? glr_up_of(c.aw[0], c.ah[0]) : 0);
    cache_bytes += c.bytes;
    cache_trim(mine);
    return &c;
}

static inline uint32_t texel_cache(const Chain& c, int lvl, int x, int y) {
    int w = c.aw[lvl], h = c.ah[lvl];
    if (tl_wrap & 1) { if (x < 0) x = 0; if (x >= w) x = w - 1; } else x &= w - 1;
    if (tl_wrap & 0x100) { if (y < 0) y = 0; if (y >= h) y = h - 1; } else y &= h - 1;
    return c.px[lvl][(size_t)y * w + x];
}

static uint32_t sample_cache(const Chain& c, int lvl, float u, float v, bool bilinear) {
    if (lvl >= c.nn) lvl = c.nn - 1;
    float k = 1.0f / (float)(1 << lvl);
    u *= k; v *= k;
    if (!bilinear) return texel_cache(c, lvl, (int)std::floor(u), (int)std::floor(v));
    float fu = u - 0.5f, fv = v - 0.5f;
    int u0 = (int)std::floor(fu), v0 = (int)std::floor(fv);
    int du = (int)((fu - u0) * 256.0f), dv = (int)((fv - v0) * 256.0f);
    uint32_t c00 = texel_cache(c, lvl, u0, v0), c10 = texel_cache(c, lvl, u0 + 1, v0);
    uint32_t c01 = texel_cache(c, lvl, u0, v0 + 1), c11 = texel_cache(c, lvl, u0 + 1, v0 + 1);
    int o[4];
    for (int i = 0; i < 4; i++) {
        int t0 = channel(c00, i) + ((channel(c10, i) - channel(c00, i)) * du >> 8);
        int t1 = channel(c01, i) + ((channel(c11, i) - channel(c01, i)) * du >> 8);
        o[i] = t0 + ((t1 - t0) * dv >> 8);
    }
    return build(o[0], o[1], o[2], o[3]);
}

// bilinear sampling with mipmap level selection
static uint32_t sample_tex(const Tex& t, int lvl, float u, float v, bool bilinear) {
    if (!bilinear) return texel_n(t, lvl, (int)std::floor(u), (int)std::floor(v));
    float fu = u - 0.5f, fv = v - 0.5f;
    int u0 = (int)std::floor(fu), v0 = (int)std::floor(fv);
    int du = (int)((fu - u0) * 256.0f), dv = (int)((fv - v0) * 256.0f);
    uint32_t c00 = texel_n(t, lvl, u0, v0), c10 = texel_n(t, lvl, u0 + 1, v0);
    uint32_t c01 = texel_n(t, lvl, u0, v0 + 1), c11 = texel_n(t, lvl, u0 + 1, v0 + 1);
    int o[4];
    for (int i = 0; i < 4; i++) {
        int t0 = channel(c00, i) + ((channel(c10, i) - channel(c00, i)) * du >> 8);
        int t1 = channel(c01, i) + ((channel(c11, i) - channel(c01, i)) * du >> 8);
        o[i] = t0 + ((t1 - t0) * dv >> 8);
    }
    return build(o[0], o[1], o[2], o[3]);
}

// hoisted state: computed once per triangle instead of per pixel
struct TexState {
    FB f; Tex t; ZB z;
    int sx0, sy0, sx1, sy1;
    bool ztest, zwrite, cleared, texture, gouraud, bilinear, stest, mip, atest, blend;
    int sfail, szfail, szpass, sbits;
    uint32_t tf, atst, bmode, fixed_a, fixed_b;
    const Chain* chain;
    uint32_t avg;
    int zfun, lvl;
    uint32_t minz, maxz, modo;
    uint8_t* fbp; uint8_t* zp; int fbpp;
    uint32_t mrgb, ma; bool no_mask, early_z;
    uint32_t wrap, clutf, stst; int clut_n, pal; uint32_t palv;
    unsigned gltex, glpal; int gltex_clut, gltex_rtfmt, gltex_w, gltex_h;
};

// per triangle or sprite state: avoids rereading GE registers on every pixel
static int palette_index() {
    if (pal_current >= 0) return pal_current;
    if (pal_live[pal_next]) psp_gpu_flush_by(PC_VPAL);
    memcpy(palettes[pal_next], clut_raw, 1024);
    pal_seq[pal_next] = ++pal_seq_global;
    pal_live[pal_next] = 1;
    pal_current = pal_next;
    pal_next = (pal_next + 1) & 255;
    return pal_current;
}

static void apply_sampling(const TexState& e) {
    tl_wrap = e.wrap; tl_clutf = e.clutf; tl_clut_bytes = e.clut_n;
    if (e.pal >= 0 && tl_pal_seq != e.palv) { memcpy(tl_clut, palettes[e.pal], 1024); tl_pal_seq = e.palv; }
}

static void prepare_tex_state(TexState& e, bool texture, bool cleared, bool gouraud) {
    memset(&e, 0, sizeof(e));
    e.f = fb(); e.t = tex(); e.z = zb();
    int ke = scale_int();
    e.sx0 = (int)(R[0xD4] & 0x3FF) * ke; e.sy0 = (int)((R[0xD4] >> 10) & 0x3FF) * ke;
    e.sx1 = ((int)(R[0xD5] & 0x3FF) + 1) * ke - 1; e.sy1 = ((int)((R[0xD5] >> 10) & 0x3FF) + 1) * ke - 1;
    if (split_clip) { e.sx0 = std::max(e.sx0, sp_x0 * ke); e.sx1 = std::min(e.sx1, sp_x1 * ke - 1); e.sy0 = std::max(e.sy0, sp_y0 * ke); e.sy1 = std::min(e.sy1, sp_y1 * ke - 1); }
    if (pnl_on && !cap_on) { e.sx0 = std::max(e.sx0, pnl_x0 * ke); e.sx1 = std::min(e.sx1, pnl_x1 * ke - 1); e.sy0 = std::max(e.sy0, pnl_y0 * ke); e.sy1 = std::min(e.sy1, pnl_y1 * ke - 1); if (pnl_hide) { e.sx1 = e.sx0; e.sy1 = e.sy0; } }
    e.cleared = cleared; e.texture = texture; e.gouraud = gouraud;
    e.modo = R[0xD3];
    e.ztest = (R[0x23] & 1) != 0;
    e.zwrite = (R[0xE7] & 1) == 0;
    e.zfun = R[0xDE] & 7;
    e.minz = R[0xD6] & 0xFFFF; e.maxz = R[0xD7] & 0xFFFF;
    e.bilinear = texture && ((R[0xC6] >> 8) & 1);
    e.sbits = stencil_bits(e.f);
    e.stest = !cleared && e.sbits && stencil_on();
    e.sfail = R[0xDD] & 7; e.szfail = (R[0xDD] >> 8) & 7; e.szpass = (R[0xDD] >> 16) & 7;
    e.chain = nullptr; e.avg = 0;
    e.tf = R[0xC9];
    e.atest = (R[0x22] & 1) != 0; e.atst = R[0xDB];
    e.blend = (R[0x21] & 1) != 0; e.bmode = R[0xDF]; e.fixed_a = R[0xE0]; e.fixed_b = R[0xE1];
    e.mip = (R[0xC6] & 4) != 0 || mip_forced();
    e.lvl = 0;
    e.fbp = psp_p(e.f.dir);
    e.fbpp = e.f.fmt == 3 ? 4 : 2;
    if (dual_side && glr_enabled() && (is_disp(e.f.dir) || is_twinned(e.f.dir))) {
        if (texture && (is_disp(e.t.dir) || is_twinned(e.t.dir))) e.t.dir = twin_dir(e.t.dir);
        if (is_disp(e.f.dir)) twin_drawn = alt_frame;
        e.f.dir = twin_dir(e.f.dir);
    }
    e.zp = psp_p(e.z.dir);
    e.mrgb = R[0xE8] & 0xFFFFFF; e.ma = R[0xE9] & 0xFF;
    e.no_mask = (e.mrgb | e.ma) == 0;
    e.early_z = e.ztest && !e.atest && !e.stest && !e.cleared;
    e.wrap = R[0xC7]; e.clutf = R[0xC5]; e.clut_n = clut_bytes; e.stst = R[0xDC];
    e.pal = (texture && e.t.fmt >= 4) ? palette_index() : -1;
    e.palv = e.pal >= 0 ? pal_seq[e.pal] : 0;
    apply_sampling(e);
    if (glr_enabled() && texture && !cleared) {
        if (is_vram(e.t.dir) || is_vram_high(e.t.dir) || is_twin(e.t.dir)) {
            int rf = 3;
            int rw = e.t.w, rh = e.t.h;
            e.gltex = glr_target_tex(e.t.dir, e.t.stride, e.t.w, e.t.h, e.t.fmt, &rf, &rw, &rh);
            e.gltex_rtfmt = rf;
            e.gltex_w = rw; e.gltex_h = rh;
            e.gltex_clut = e.t.fmt >= 4 ? 1 : 0;
            if (e.gltex_clut) e.glpal = glr_palette();
            if (!is_twin(e.t.dir)) e.avg = tex_avg(e.t);
        } else {
            e.chain = chain_of(e.t);
            e.gltex = e.chain ? e.chain->gl : 0;
            e.gltex_w = e.t.w; e.gltex_h = e.t.h;
            e.avg = tex_avg(e.t);
        }
    }
}

static inline uint32_t read_fb(const TexState& e, int x, int y) {
    const uint8_t* p = e.fbp + (size_t)(y * e.f.width + x) * e.fbpp;
    if (e.fbpp == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
    uint16_t v; memcpy(&v, p, 2); return convert(v, e.f.fmt);
}

static inline void set_fb(const TexState& e, int x, int y, uint32_t c) {
    uint8_t* p = e.fbp + (size_t)(y * e.f.width + x) * e.fbpp;
    if (!e.no_mask) {
        uint32_t m = e.mrgb | (e.ma << 24);
        c = (c & ~m) | (read_fb(e, x, y) & m);
    }
    if (e.fbpp == 4) { memcpy(p, &c, 4); return; }
    uint16_t q = e.f.fmt == 0 ? a5650(c) : e.f.fmt == 1 ? a5551(c) : a4444(c);
    memcpy(p, &q, 2);
}

static inline void set_stencil(const TexState& e, int x, int y, uint32_t sv) {
    if (!e.sbits) return;
    uint32_t old = read_fb(e, x, y);
    uint32_t a = ((sv & ~e.ma) | (((old >> 24) & 0xFF) & e.ma)) & 0xFF;
    uint32_t c = (old & 0x00FFFFFFu) | (a << 24);
    uint8_t* q = e.fbp + (size_t)(y * e.f.width + x) * e.fbpp;
    if (e.fbpp == 4) { memcpy(q, &c, 4); return; }
    uint16_t p = a5551(c);
    memcpy(q, &p, 2);
}

static inline bool z_early(const TexState& e, int x, int y, float zf) {
    uint32_t depth = zf < 0 ? 0 : (zf > 65535.0f ? 65535 : (uint32_t)zf);
    if (depth < e.minz || depth > e.maxz) return false;
    uint16_t* dz = (uint16_t*)e.zp + (y * e.z.width + x);
    uint32_t old = *dz;
    bool passes;
    switch (e.zfun) {
    case 0: passes = false; break; case 1: passes = true; break;
    case 2: passes = depth == old; break; case 3: passes = depth != old; break;
    case 4: passes = depth < old; break; case 5: passes = depth <= old; break;
    case 6: passes = depth > old; break; default: passes = depth >= old; break;
    }
    if (!passes) return false;
    if (e.zwrite) *dz = (uint16_t)depth;
    return true;
}

static void save_png(const char* nombre, const uint32_t* px, int w, int h);
struct Use { Tex t; uint64_t px; int d2; uint32_t blend_mode, prim; };
static int dump_tex = 0;
static uint32_t goal[4];      // textures marked for dumping as soon as they are drawn again
static int goal_n = 0;
static uint32_t skip[8]; static int skip_n = -1;

// TTT_SKIP: comma-separated hex list of textures to skip
static bool skip_draw(uint32_t dir) {
    if (skip_n < 0) {
        skip_n = 0;
        const char* e = getenv("TTT_SKIP");
        while (e && *e && skip_n < 8) {
            char* end_v;
            unsigned long v = strtoul(e, &end_v, 0);
            if (end_v == e) break;
            skip[skip_n++] = (uint32_t)v;
            e = end_v;
            while (*e == ',' || *e == ' ') e++;
        }
        if (skip_n) { printf("TTT_SKIP activo:"); for (int i = 0; i < skip_n; i++) printf(" 0x%08X", skip[i]); printf("\n"); }
    }
    for (int i = 0; i < skip_n; i++) if (skip[i] == dir) return true;
    return false;
}
static Use uses[32];
static int uses_n = 0;
static int in_2d = 0;             // 1 while a through-mode triangle is being rasterized
static const uint32_t RAMPA_DIR = 0x08D987C0u;

static void dump_live(const Tex& t, int d2) {
    static uint32_t tpx[1024 * 1024];
    if (t.w > 1024 || t.h > 1024) return;
    for (int y = 0; y < t.h; y++) for (int x = 0; x < t.w; x++) tpx[y * t.w + x] = texel_n(t, 0, x, y) | 0xFF000000u;
    char name[96];
    snprintf(name, sizeof(name), "viva_%s_%08X_%dx%d_f%d_s%d_sw%d.png", d2 ? "2D" : "3D", t.dir, t.w, t.h, t.fmt, t.stride, t.swz);
    save_png(name, tpx, t.w, t.h);
    printf("  viva %s 0x%08X %dx%d fmt %d stride %d swz %d clut %d fmtclut %u\n",
           d2 ? "2D" : "3D", t.dir, t.w, t.h, t.fmt, t.stride, t.swz, clut_bytes, R[0xC5] & 3);
}

static void note_use(const Tex& t, uint64_t px, int d2) {
    for (int i = 0; i < goal_n; i++)
        if (goal[i] == t.dir) { goal[i] = 0; dump_live(t, d2); }
    for (int i = 0; i < uses_n; i++) if (uses[i].t.dir == t.dir && uses[i].t.w == t.w && uses[i].d2 == d2) { uses[i].px += px; return; }
    if (uses_n < 32) {
        uses[uses_n].t = t; uses[uses_n].px = px; uses[uses_n].d2 = d2;
        uses[uses_n].blend_mode = (R[0x21] & 1) | (R[0xDF] << 1); uses[uses_n].prim = R[0x22] & 1 ? R[0xDB] : 0;
        uses_n++;
        if (dump_tex > 0) { dump_tex--; dump_live(t, d2); }
    }
}

static int spy_x = -1, spy_y = -1, spy_n = 0, spy_init = 0;
static uint32_t spy_texel = 0, spy_vcol = 0; static float spy_u = 0, spy_v = 0;
static uint32_t spy_dumped[8]; static int spy_vn = 0;

// TTT_SPY=x,y dumps the colour chain of that pixel
static void spy(const TexState& e, uint32_t col, uint32_t dst, uint32_t out) {
    int sa = (col >> 24) & 0xFF, da = (dst >> 24) & 0xFF;
    int fs = e.bmode & 15, fd = (e.bmode >> 4) & 15;
    printf("spy: tex 0x%08X %dx%d fmt %d | tfunc 0x%06X | col 0x%08X (sa %d) dst 0x%08X (da %d) -> out 0x%08X | mezcla %u fs %d fd %d eq %u | factores %d/%d | atest %u/0x%06X\n",
           e.t.dir, e.t.w, e.t.h, e.t.fmt, e.tf, col, sa, dst, da, out,
           (unsigned)e.blend, fs, fd, (e.bmode >> 8) & 7,
           (fs == 0) ? (int)((dst >> 8) & 0xFF) : (fs == 1) ? 255 - (int)((dst >> 8) & 0xFF) : factor(fs, 0, 0, sa, da, channel(e.fixed_a, 1)),
           (fd == 0) ? (int)((col >> 8) & 0xFF) : (fd == 1) ? 255 - (int)((col >> 8) & 0xFF) : factor(fd, 0, 0, sa, da, channel(e.fixed_b, 1)),
           (unsigned)e.atest, e.atst);
    uint32_t cf = R[0xC5];
    printf("   vcol 0x%08X texel 0x%08X uv (%.2f %.2f) | paleta fmt %u shift %u mask 0x%02X off %u bytes %d | wrap %u/%u filtro %u/%u | tfunc modo %u alfa %u doble %u\n",
           spy_vcol, spy_texel, spy_u, spy_v, cf & 3, (cf >> 2) & 31, (cf >> 8) & 0xFF, (cf >> 16) & 31, clut_bytes,
           R[0xC7] & 1, (R[0xC7] >> 8) & 1, R[0xC6] & 7, (R[0xC6] >> 8) & 7,
           e.tf & 7, (e.tf >> 8) & 1, (e.tf >> 16) & 1);
    if (e.t.dir >= 0x04000000u && e.t.dir < 0x04200000u) {
        bool ya = false;
        for (int i = 0; i < spy_vn; i++) if (spy_dumped[i] == e.t.dir) ya = true;
        if (!ya && spy_vn < 8) { spy_dumped[spy_vn++] = e.t.dir; dump_live(e.t, 0); }
    }
}

static void blend_pixel(const TexState& e, int x, int y, uint32_t col, float zf, bool z_done) {
#if TTT_DIAG
    if (!spy_init) {
        spy_init = 1;
        const char* v = getenv("TTT_SPY");
        if (v) { spy_x = atoi(v); const char* c = strchr(v, ','); if (c) spy_y = atoi(c + 1); }
    }
#endif
    uint32_t depth = zf < 0 ? 0 : (zf > 65535.0f ? 65535 : (uint32_t)zf);
    uint16_t* dz = (uint16_t*)e.zp + (y * e.z.width + x);
    if (e.cleared) {
        if (e.modo & 0x300) {
            uint32_t out = col;
            if ((e.modo & 0x300) != 0x300) {
                uint32_t old = read_fb(e, x, y);
                if (!(e.modo & 0x100)) out = (out & 0xFF000000u) | (old & 0x00FFFFFFu);
                if (!(e.modo & 0x200)) out = (out & 0x00FFFFFFu) | (old & 0xFF000000u);
            }
            set_fb(e, x, y, out);
        }
        if ((e.modo >> 10) & 1) *dz = (uint16_t)depth;
        return;
    }
    if (!test_alpha2(col, e.atest, e.atst)) return;
    uint32_t dst = read_fb(e, x, y);
    uint32_t st = (dst >> 24) & 0xFF;
    if (e.stest && !test_stencil(st, e.stst)) { set_stencil(e, x, y, stencil_op(e.sfail, st, e.sbits, e.stst)); return; }
    if (e.ztest && !z_done) {
        bool passes = depth >= e.minz && depth <= e.maxz;
        if (passes) {
            uint32_t old = *dz;
            switch (e.zfun) {
            case 0: passes = false; break; case 1: passes = true; break;
            case 2: passes = depth == old; break; case 3: passes = depth != old; break;
            case 4: passes = depth < old; break; case 5: passes = depth <= old; break;
            case 6: passes = depth > old; break; default: passes = depth >= old; break;
            }
        }
        if (!passes) { if (e.stest) set_stencil(e, x, y, stencil_op(e.szfail, st, e.sbits, e.stst)); return; }
        if (e.zwrite) *dz = (uint16_t)depth;
    }
    uint32_t out = blend_px2(col, dst, e.blend, e.bmode, e.fixed_a, e.fixed_b);
#if TTT_DIAG
    if (x == spy_x && y == spy_y && spy_n < 30) { spy_n++; spy(e, col, dst, out); }
#endif
    if (e.stest) out = (out & 0x00FFFFFFu) | (stencil_op(e.szpass, st, e.sbits, e.stst) << 24);
    else if (e.sbits && keep_alpha()) out = (out & 0x00FFFFFFu) | (dst & 0xFF000000u);
    set_fb(e, x, y, out);
}

// fast log2 from the float exponent
static inline int lod_of(float rho2) {
    if (rho2 <= 1.0f) return 0;
    uint32_t b; memcpy(&b, &rho2, 4);
    int e = (int)((b >> 23) & 0xFF) - 127;
    return (e + ((b >> 22) & 1)) >> 1;
}

static uint32_t v3_tex[6]; static int v3_x0[6], v3_y0[6], v3_x1[6], v3_y1[6]; static uint64_t v3_px[6], v3_cnt[6]; static float v3_u[6][2], v3_v[6][2]; static int v3_n = 0;

static void note_vram3d(const Tex& t, int x0, int y0, int x1, int y1, const VP& a, const VP& b, const VP& c) {
    int i = 0;
    for (; i < v3_n; i++) if (v3_tex[i] == t.dir) break;
    if (i == v3_n) {
        if (v3_n >= 6) return;
        v3_n++;
        v3_tex[i] = t.dir; v3_x0[i] = x0; v3_y0[i] = y0; v3_x1[i] = x1; v3_y1[i] = y1;
        v3_px[i] = 0; v3_cnt[i] = 0;
        v3_u[i][0] = v3_u[i][1] = a.u / a.iw; v3_v[i][0] = v3_v[i][1] = a.v / a.iw;
    }
    if (x0 < v3_x0[i]) v3_x0[i] = x0;
    if (y0 < v3_y0[i]) v3_y0[i] = y0;
    if (x1 > v3_x1[i]) v3_x1[i] = x1;
    if (y1 > v3_y1[i]) v3_y1[i] = y1;
    const VP* vs[3] = {&a, &b, &c};
    for (int k = 0; k < 3; k++) {
        float u = vs[k]->u / vs[k]->iw, v = vs[k]->v / vs[k]->iw;
        if (u < v3_u[i][0]) v3_u[i][0] = u;
        if (u > v3_u[i][1]) v3_u[i][1] = u;
        if (v < v3_v[i][0]) v3_v[i][0] = v;
        if (v > v3_v[i][1]) v3_v[i][1] = v;
    }
    v3_px[i] += (uint64_t)(x1 - x0 + 1) * (y1 - y0 + 1);
    v3_cnt[i]++;
}

// parallel rasterization in horizontal bands: primitives are queued with their state
// and on flush each thread paints its strip of rows. Per-pixel order matches the serial path.
struct Job {
    uint32_t st_tex;
    VP v[3];
    uint32_t col_flat;
    int x0, x1, y0, y1;
    uint8_t back = 0;   // drawn inside the bars-behind packet
    uint8_t cls, toon, fig;   // fig: a fighter draw (marker 0xE3), toon or not: the absolute outline needs only this
    uint32_t signature;
    float lv[3];
    uint32_t tint, grp, vh, tp;
    uint8_t face, d3, who, aura, glow;
    float up[3];
    float vi[12], eye[3]; uint8_t sv, wsf;   // wsf: a BT3 water surface triangle (surf_draw), for the water mask
    int ag = -1;   // per-prim sums computed by the vertex workers (gv_agv)
};
static int surf_now = 0;

static int watchtex_var = -1, batch_var = -1;

// TTT_WATCH_TEX=1 signs the texture on enqueue and checks it on paint
static bool watch_tex() {
    if (watchtex_var < 0) { const char* e = getenv("TTT_WATCH_TEX"); watchtex_var = e ? atoi(e) : 0; }
    return watchtex_var != 0;
}

// TTT_BATCH=N flushes the queue every N primitives
static int batch_cap() {
    if (batch_var < 0) { const char* e = getenv("TTT_BATCH"); batch_var = e ? atoi(e) : 0; }
    return batch_var;
}

static void apply_sampling(const TexState& e);

// signs the sampled colour: covers texture, palette, palette format and wrap
static uint32_t sampling_signature(const TexState& e) {
    apply_sampling(e);
    uint32_t k = 2166136261u;
    k = (k ^ e.wrap) * 16777619u;
    k = (k ^ e.clutf) * 16777619u;
    k = (k ^ (uint32_t)e.clut_n) * 16777619u;
    int w = e.t.w, h = e.t.h;
    const int px[6] = {0, w / 3, w / 2, 2 * w / 3, w - 1, w / 5};
    const int py[6] = {0, h / 2, h / 3, h - 1, 2 * h / 3, h / 7};
    for (int i = 0; i < 6; i++) k = (k ^ texel(e.t, px[i], py[i])) * 16777619u;
    return k;
}

static std::vector<TexState> states;
static std::vector<TexState> st_copy;   // only with TTT_WATCH_TEX: the state exactly as it was on enqueue
static std::vector<Job> queue;
static int threads_var = -1;
static int threads_n() {
    if (threads_var < 0) {
        const char* e = getenv("TTT_THREADS");
        int n = e ? atoi(e) : 0;
        if (n <= 0) { unsigned h = std::thread::hardware_concurrency(); n = h > 2 ? (int)h - 2 : 1; }
        if (n < 1) n = 1;
        if (n > 32) n = 32;
        if (n > 12 && !e) n = 12;
        threads_var = n;
        if (n > 1) printf("rasterizado en %d hilos (TTT_THREADS)\n", n);
    }
    return threads_var;
}

static int diff_var = -1;
static bool defer() {
    if (diff_var < 0) { const char* e = getenv("TTT_DEFER"); diff_var = e ? atoi(e) : 1; }
    return diff_var != 0;
}

static int cload_var = -1;
static bool cload_flush() {
    if (cload_var < 0) { const char* e = getenv("TTT_CLOAD"); cload_var = e ? atoi(e) : 1; }
    return cload_var != 0;
}

static bool in_vram_addr(uint32_t d) { return d >= 0x04000000u && d < 0x04200000u; }

static int overlap_var = -1;
static bool use_overlap() {
    if (overlap_var < 0) { const char* e = getenv("TTT_OVERLAP"); overlap_var = e ? atoi(e) : 1; }
    return overlap_var != 0;
}

static int strips_var = -1;
static bool use_strips() {
    if (strips_var < 0) { const char* e = getenv("TTT_STRIPS"); strips_var = e ? atoi(e) : 1; }
    return strips_var != 0;
}

// real range written by the queued work, not the whole buffer
static uint32_t sc_fb0 = 0xFFFFFFFFu, sc_fb1 = 0, sc_z0 = 0xFFFFFFFFu, sc_z1 = 0;

static void note_target(const TexState& e, int y0, int y1) {
    if (is_twin(e.f.dir)) return;
    uint32_t bpp = e.f.fmt == 3 ? 4 : 2;
    uint32_t a = e.f.dir + (uint32_t)y0 * e.f.width * bpp;
    uint32_t b = e.f.dir + (uint32_t)(y1 + 1) * e.f.width * bpp;
    if (a < sc_fb0) sc_fb0 = a;
    if (b > sc_fb1) sc_fb1 = b;
    if (e.ztest || e.cleared) {
        uint32_t za = e.z.dir + (uint32_t)y0 * e.z.width * 2;
        uint32_t zb = e.z.dir + (uint32_t)(y1 + 1) * e.z.width * 2;
        if (za < sc_z0) sc_z0 = za;
        if (zb > sc_z1) sc_z1 = zb;
    }
}

static bool hits_target(const Tex& t) {
    if (!use_overlap()) return true;
    if (queue.empty()) return false;
    uint32_t tb = t.dir, tf_ = t.dir + (uint32_t)t.stride * t.h * bpp_tex(t.fmt) / 8;
    return (tb < sc_fb1 && tf_ > sc_fb0) || (tb < sc_z1 && tf_ > sc_z0);
}

static uint32_t tex_state_index(const TexState& e) {
    if (e.pal >= 0) pal_live[e.pal] = 1;
    if (e.chain) cache_viva[e.chain - cache] = 1;
    if (!states.empty() && memcmp(&states.back(), &e, sizeof(TexState)) == 0) return (uint32_t)states.size() - 1;
    states.push_back(e);
    return (uint32_t)states.size() - 1;
}

static void rasterize_band(const TexState& e, const Job& t, int by0, int by1, uint64_t* cnt);
static void sprite_band(const TexState& e, const Job& t, int by0, int by1, uint64_t* cnt);

static void run_band(int k);
static std::mutex& m_pool = *(new std::mutex());          // no destructor: the workers live until exit
static std::condition_variable& cv_begin = *(new std::condition_variable());
static std::condition_variable& cv_end = *(new std::condition_variable());
static int pool_n = 0;
static int gen_round = 0, n_end = 0, n_active = 0;
static int band_y0[32], band_y1[32];
static uint64_t counter_h[32][5];

static void worker(int k) {
    int seen = 0;
    for (;;) {
        std::unique_lock<std::mutex> lk(m_pool);
        cv_begin.wait(lk, [&] { return gen_round != seen; });
        seen = gen_round;
        lk.unlock();
        if (k < n_active) run_band(k);
        lk.lock();
        n_end++;
        cv_end.notify_one();
    }
}

static void start_pool() {
    if (pool_n) return;
    stencil_on(); keep_alpha(); mip_forced(); clut_variant(); tmap_variant();
    pool_n = threads_n();
    for (int k = 0; k < pool_n; k++) std::thread(worker, k).detach();
}

static void run_band(int k) {
    uint64_t* c = counter_h[k];
    int by0 = band_y0[k], by1 = band_y1[k];
    for (size_t i = 0; i < queue.size(); i++) {
        const Job& t = queue[i];
        if (t.y1 < by0 || t.y0 > by1) continue;
        const TexState& e = states[t.st_tex];
        apply_sampling(e);
        if (t.cls == 2) continue;
        if (t.cls) sprite_band(e, t, by0, by1, c);
        else rasterize_band(e, t, by0, by1, c);
    }
}

static void check_signatures() {
    static int tex_warnings = 0;
    for (size_t i = 0; i < queue.size() && tex_warnings < 60; i++) {
        const TexState& e = states[queue[i].st_tex];
        if (!e.texture) continue;
        if (i < st_copy.size() && memcmp(&st_copy[i], &e, sizeof(TexState)) != 0) {
            const uint8_t* a = (const uint8_t*)&st_copy[i];
            const uint8_t* b = (const uint8_t*)&e;
            size_t off = 0;
            while (off < sizeof(TexState) && a[off] == b[off]) off++;
            tex_warnings++;
            printf("estado mal asociado: pos %d de %d, indice %u de %u estados, primer byte distinto en +%d | tex encolada 0x%08X, tex al pintar 0x%08X\n",
                   (int)i, (int)queue.size(), queue[i].st_tex, (unsigned)states.size(), (int)off,
                   st_copy[i].t.dir, e.t.dir);
            continue;
        }
        uint32_t ahora = sampling_signature(e);
        if (ahora != queue[i].signature) {
            tex_warnings++;
            printf("muestreo cambiado: tex 0x%08X %dx%d fmt %d stride %d swz %d | paleta ranura %d serie %u clutf 0x%06X bytes %d wrap 0x%03X | clase %d pos %d de %d | encolada %08X, al pintar %08X\n",
                   e.t.dir, e.t.w, e.t.h, e.t.fmt, e.t.stride, e.t.swz,
                   e.pal, e.palv, e.clutf, e.clut_n, e.wrap,
                   queue[i].cls, (int)i, (int)queue.size(), queue[i].signature, ahora);
        }
    }
}

static uint64_t cp_box[3], cp_job[4][2], cp_op[256][2], cp_em[2][5], cp_st[4], cp_gj[12], cp_vq[12], cp_tj[8], cp_lj[12], cp_lg[8], cp_db[5], cp_at[4], cp_ap[9], cp_pr[9], cp_mk[256][2]; static int cp_var = -1;
static bool cp_on() { if (cp_var < 0) { const char* e = getenv("TTT_CPU_PROBE"); cp_var = e && *e ? atoi(e) : 0; } return cp_var > 0; }
struct LjT { uint64_t t0, ex = 0; LjT(bool on) : t0(on ? perf_clock() : 0) {} ~LjT() { if (t0) { cp_lj[0] += perf_clock() - t0 - ex; cp_lj[1]++; } } };
struct CpJob { uint64_t t0; int k; CpJob(int c) : t0(cp_on() ? perf_clock() : 0), k(c < 0 || c > 3 ? 3 : c) {} ~CpJob() { if (t0) { cp_job[k][0] += perf_clock() - t0; cp_job[k][1]++; } } };
static uint64_t gv_t[8];   // cycles: skinned 3D prims, other 3D prims, cel scene, cel normals, whole GL flush, upload, job loop, last batch
struct GvTime { uint64_t t0; int k; GvTime(int i) : t0(perf_clock()), k(i) {} ~GvTime() { if (k >= 0) gv_t[k] += perf_clock() - t0; } };
#include "gpu/gpu_gl.h"

static void gv_defer_run();
void psp_gpu_flush_by(int reason) {
    gv_defer_run();
    SProf sp_(3);
    if (queue.empty()) { states.clear(); st_copy.clear(); return; }
    // painting installs each primitive's sampling: it must be restored
    uint32_t g_wrap = tl_wrap, g_clutf = tl_clutf, g_seq = tl_pal_seq;
    int g_bytes = tl_clut_bytes;
    static uint8_t g_clut[1024];
    memcpy(g_clut, tl_clut, 1024);
    if (watch_tex()) check_signatures();
    if (reason >= 0) PERF_N(reason, 1);
    PERF(PZ_RAST);
    if (glr_enabled()) {
        PERF_N(PC_FLUSH, 1);
        glr_flush_queue();
        tl_wrap = g_wrap; tl_clutf = g_clutf; tl_pal_seq = g_seq; tl_clut_bytes = g_bytes;
        memcpy(tl_clut, g_clut, 1024);
        queue.clear(); states.clear(); st_copy.clear();
        sc_fb0 = sc_z0 = 0xFFFFFFFFu; sc_fb1 = sc_z1 = 0;
        memset(cache_viva, 0, sizeof(cache_viva));
        memset(pal_live, 0, sizeof(pal_live));
        pal_current = -1;
        return;
    }
    int n = threads_n();
    PERF_N(PC_FLUSH, 1);
    int ymin = 1 << 30, ymax = -1;
    uint64_t area = 0;
    for (size_t i = 0; i < queue.size(); i++) {
        if (queue[i].y0 < ymin) ymin = queue[i].y0;
        if (queue[i].y1 > ymax) ymax = queue[i].y1;
        area += (uint64_t)(queue[i].x1 - queue[i].x0 + 1) * (queue[i].y1 - queue[i].y0 + 1);
    }
    int height = ymax - ymin + 1;
    if (height < 1) n = 1;
    else if (n > height) n = height;
    int ntope = (int)(area / 8000);   // each band needs enough work to pay for the barrier
    if (n > ntope) n = ntope;
    if (n < 1) n = 1;
    if (n <= 1) {
        band_y0[0] = 0; band_y1[0] = 1 << 30;
        memset(counter_h[0], 0, sizeof(counter_h[0]));
        run_band(0);
    } else {
        start_pool();
        for (int k = 0; k < n; k++) {
            band_y0[k] = ymin + (int)((int64_t)height * k / n);
            band_y1[k] = ymin + (int)((int64_t)height * (k + 1) / n) - 1;
            memset(counter_h[k], 0, sizeof(counter_h[k]));
        }
        {
            std::unique_lock<std::mutex> lk(m_pool);
            n_active = n; n_end = 0; gen_round++;
            cv_begin.notify_all();
            cv_end.wait(lk, [&] { return n_end >= pool_n; });
        }
    }
    for (int k = 0; k < (n <= 1 ? 1 : n); k++) {
        PERF_N(PC_PIX, counter_h[k][0]);
        PERF_N(PC_ZFAIL, counter_h[k][1]);
        PERF_N(PC_PIXA, counter_h[k][2]);
        PERF_N(PC_PIXNZ, counter_h[k][3]);
        PERF_N(PC_PIXST, counter_h[k][4]);
    }
    tl_wrap = g_wrap; tl_clutf = g_clutf; tl_pal_seq = g_seq; tl_clut_bytes = g_bytes;
    memcpy(tl_clut, g_clut, 1024);
    queue.clear();
    states.clear();
    st_copy.clear();
    sc_fb0 = sc_z0 = 0xFFFFFFFFu;
    sc_fb1 = sc_z1 = 0;
    memset(cache_viva, 0, sizeof(cache_viva));
    memset(pal_live, 0, sizeof(pal_live));
    pal_current = -1;
}

void psp_gpu_flush() { psp_gpu_flush_by(-1); }

// TTT_TINT=1: paints each 3D texture group in a flat colour, no texture or blending.
// Shows where each model really ends up when nothing is visible on screen.
static int tint_var = -1;
static int tint() {
    if (tint_var < 0) {
        const char* e = getenv("TTT_TINT");
        tint_var = e ? atoi(e) : 0;
        if (tint_var) { printf("AVISO: TTT_TINT activo, las texturas 3D salen de colores planos\n"); fflush(stdout); }
    }
    return tint_var;
}
static const uint32_t TINTES[12] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFFFF00FFu,
    0xFF00FFFFu, 0xFFFF8000u, 0xFF8000FFu, 0xFF00FF80u, 0xFF80FF00u, 0xFFFFFFFFu, 0xFF808080u};

// ge_fast: the state of a primitive is built once for all its triangles (render targets excluded)
static uint64_t prim_serial = 0, memo_serial = ~0ull;
static int memo_flags = -1;
static TexState memo_e;

static void clip_box(int& x0, int& y0, int& x1, int& y1) {
    int ke = scale_int();
    x0 = (int)(R[0xD4] & 0x3FF) * ke; y0 = (int)((R[0xD4] >> 10) & 0x3FF) * ke;
    x1 = ((int)(R[0xD5] & 0x3FF) + 1) * ke - 1; y1 = ((int)((R[0xD5] >> 10) & 0x3FF) + 1) * ke - 1;
    if (split_clip) { x0 = std::max(x0, sp_x0 * ke); x1 = std::min(x1, sp_x1 * ke - 1); y0 = std::max(y0, sp_y0 * ke); y1 = std::min(y1, sp_y1 * ke - 1); }
    if (pnl_on && !cap_on) { x0 = std::max(x0, pnl_x0 * ke); x1 = std::min(x1, pnl_x1 * ke - 1); y0 = std::max(y0, pnl_y0 * ke); y1 = std::min(y1, pnl_y1 * ke - 1); if (pnl_hide) { x1 = x0; y1 = y0; } }
}

static void rasterize(const VP& a0_, const VP& b0_, const VP& c0_, bool texture, bool cleared, bool gouraud, bool inv) {
    SProf sp_(2);
    gp_n[4]++;
    VP a0 = a0_, b0 = b0_, c0 = c0_;
    if (tint() && texture && !cleared && sc_act >= 0) {
        uint32_t t = TINTES[sc_act % 12];
        a0.color = b0.color = c0.color = t;
        texture = false;
        gouraud = false;
    }
    float area = (b0.x - a0.x) * (c0.y - a0.y) - (b0.y - a0.y) * (c0.x - a0.x);
    if (area == 0.0f) return;
    if (!cleared && (R[0x1D] & 1) && !(in_2d && hud_flip)) {
        bool front_positive = (R[0x9B] & 1) == 0;
        if ((area > 0) != (front_positive != inv)) { tris_cull++; sc_cull++; if (sc_act >= 0) sc_cul[sc_act]++; PERF_N(PC_TRICULL, 1); return; }
    }
    if (sc_act >= 0) { sc_pri[sc_act]++; sc_vertex(a0.x, a0.y, a0.z, a0.color); sc_vertex(b0.x, b0.y, b0.z, b0.color); sc_vertex(c0.x, c0.y, c0.z, c0.color); }
    uint32_t col_flat = c0.color;            // in flat shading the last original vertex wins
    // single winding: the fill rule is the same for both faces and no cracks remain
    const VP& a = a0;
    const VP& b = area < 0 ? c0 : b0;
    const VP& c = area < 0 ? b0 : c0;
    if (area < 0) area = -area;

    int x0 = (int)std::floor(std::fmin(a.x, std::fmin(b.x, c.x)));
    int x1 = (int)std::ceil(std::fmax(a.x, std::fmax(b.x, c.x)));
    int y0 = (int)std::floor(std::fmin(a.y, std::fmin(b.y, c.y)));
    int y1 = (int)std::ceil(std::fmax(a.y, std::fmax(b.y, c.y)));
    if (patch_ge_fast.val) {
        int bx0, by0, bx1, by1;
        clip_box(bx0, by0, bx1, by1);
        if (std::max(x0, bx0) > std::min(x1, bx1) || std::max(y0, by0) > std::min(y1, by1)) return;
    }
    int fl = (texture ? 1 : 0) | (cleared ? 2 : 0) | (gouraud ? 4 : 0);
    TexState e_own;
    bool reuse = patch_ge_fast.val && memo_serial == prim_serial && memo_flags == fl;
    TexState& e = reuse ? memo_e : e_own;
    bool in_vram = false;
    if (reuse) apply_sampling(e);
    else {
        prepare_tex_state(e, texture, cleared, gouraud);
        in_vram = (e.t.dir >= 0x04000000u && e.t.dir < 0x04200000u) || (patch_rt_texture.val && is_vram_high(e.t.dir)) || is_twin(e.t.dir);   // render target, changes every frame
        if (texture && !in_vram && e.t.levels <= 1 && e.t.w >= 8 && e.t.h >= 8 && !(patch_ge_fast.val && e.chain)) e.chain = chain_of(e.t);
        if (patch_ge_fast.val && !in_vram) { memo_e = e; memo_serial = prim_serial; memo_flags = fl; }
    }
    split_tris[split_now]++;
    if (x0 < e.sx0) x0 = e.sx0;
    if (y0 < e.sy0) y0 = e.sy0;
    if (x1 > e.sx1) x1 = e.sx1;
    if (y1 > e.sy1) y1 = e.sy1;
    if (x1 > e.f.width - 1) x1 = e.f.width - 1;
    if (y1 > 512 * scale_int() - 1) y1 = 512 * scale_int() - 1;
    if (x0 > x1 || y0 > y1) return;
    if (texture) {
        if (skip_draw(e.t.dir)) return;
#if TTT_DIAG
        note_use(e.t, (uint64_t)(x1 - x0 + 1) * (y1 - y0 + 1), in_2d ? 2 : 0);
        if (!in_2d && e.t.dir >= 0x04000000u && e.t.dir < 0x04200000u)
            note_vram3d(e.t, x0, y0, x1, y1, a, b, c);
#endif
    }
    if (texture && in_vram && hits_target(e.t)) psp_gpu_flush_by(PC_VVRAM);
    Job t;
    t.cls = 0; t.v[0] = a; t.v[1] = b; t.v[2] = c; t.col_flat = col_flat;
    t.toon = toon_now && texture && !cleared;
    t.fig = fig_now && texture && !cleared;
    t.glow = t.fig ? glow_now : 0;
    memcpy(t.lv, toon_lv, sizeof(t.lv));
    t.tint = t.toon ? toon_tint : 0;
    t.grp = t.toon ? toon_grp : 0; t.tp = t.toon ? toon_tp : 0; t.who = (uint8_t)(t.toon || t.fig ? toon_who : 0); t.aura = (uint8_t)toon_aura; t.face = (uint8_t)(t.toon ? toon_face : 0);
    t.d3 = !in_2d; t.vh = view_hash; memcpy(t.up, view_up, sizeof(t.up));
    memcpy(t.vi, view_inv, sizeof(t.vi)); memcpy(t.eye, proj_eye, sizeof(t.eye)); t.sv = (uint8_t)split_now; t.wsf = (uint8_t)surf_now; t.back = (uint8_t)ef_now;
    t.x0 = x0; t.x1 = x1; t.y0 = y0; t.y1 = y1;
    if (!defer()) {
        uint64_t cn[5] = {0, 0, 0, 0, 0};
        rasterize_band(e, t, 0, 1 << 29, cn);
        PERF_N(PC_PIX, cn[0]); PERF_N(PC_ZFAIL, cn[1]);
        PERF_N(PC_PIXA, cn[2]); PERF_N(PC_PIXNZ, cn[3]); PERF_N(PC_PIXST, cn[4]);
        return;
    }
    t.st_tex = tex_state_index(e);
    t.signature = watch_tex() && texture ? sampling_signature(e) : 0;
    queue.push_back(t);
    note_target(e, t.y0, t.y1);
    if (watch_tex()) st_copy.push_back(e);
    if (batch_cap() && (int)queue.size() >= batch_cap()) psp_gpu_flush();
}

static void rasterize_band(const TexState& e, const Job& t, int by0, int by1, uint64_t* cnt) {
    const VP& a = t.v[0]; const VP& b = t.v[1]; const VP& c = t.v[2];
    uint32_t col_flat = t.col_flat;
    bool texture = e.texture, gouraud = e.gouraud;
    int x0 = t.x0, x1 = t.x1;
    int y0 = t.y0 < by0 ? by0 : t.y0;
    int y1 = t.y1 > by1 ? by1 : t.y1;
    if (y0 > y1) return;
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area < 0) area = -area;

    // incremental edge functions with a top-left rule so edges are not painted twice
    float A01 = a.y - b.y, B01 = b.x - a.x;
    float A12 = b.y - c.y, B12 = c.x - b.x;
    float A20 = c.y - a.y, B20 = a.x - c.x;
    float px = x0 + 0.5f, py = t.y0 + 0.5f;
    float w0f = (c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x);
    float w1f = (a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x);
    float w2f = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
    float s12 = (B12 != 0 ? (B12 > 0 ? 1e-6f : 0.0f) : (A12 > 0 ? 1e-6f : 0.0f));
    float s20 = (B20 != 0 ? (B20 > 0 ? 1e-6f : 0.0f) : (A20 > 0 ? 1e-6f : 0.0f));
    float s01 = (B01 != 0 ? (B01 > 0 ? 1e-6f : 0.0f) : (A01 > 0 ? 1e-6f : 0.0f));
    float inv_area = 1.0f / area;

    // screen gradients of u/w, v/w and 1/w: they allow per-pixel LOD
    float g0x = A12 * inv_area, g1x = A20 * inv_area, g2x = -(g0x + g1x);
    float g0y = B12 * inv_area, g1y = B20 * inv_area, g2y = -(g0y + g1y);
    float dUx = a.u * g0x + b.u * g1x + c.u * g2x, dVx = a.v * g0x + b.v * g1x + c.v * g2x;
    float dWx = a.iw * g0x + b.iw * g1x + c.iw * g2x;
    float dUy = a.u * g0y + b.u * g1y + c.u * g2y, dVy = a.v * g0y + b.v * g1y + c.v * g2y;
    float dWy = a.iw * g0y + b.iw * g1y + c.iw * g2y;
    int nmax = 0;
    if (e.mip && e.texture) nmax = e.chain ? e.chain->nn - 1 : e.t.levels - 1;
    if (nmax < 0) nmax = 0;

    // row stepping accumulates from the triangle's first row, not the band's:
    // so rounding is identical however the work is split
    for (int y = t.y0; y < y0; y++) { w0f += B12; w1f += B20; w2f += B01; }
    uint64_t np = 0, nz = 0;
    bool gradient = gouraud && !(a.color == b.color && b.color == c.color);
    (void)e;
    for (int y = y0; y <= y1; y++) {
        float e0 = w0f, e1 = w1f, e2 = w2f;
        for (int x = x0; x <= x1; x++, e0 += A12, e1 += A20, e2 += A01) {
            if (e0 + s12 < 0 || e1 + s20 < 0 || e2 + s01 < 0) continue;
            np++;
            float l0 = e0 * inv_area, l1 = e1 * inv_area, l2 = 1.0f - l0 - l1;
            float z = a.z * l0 + b.z * l1 + c.z * l2;
            if (e.early_z && !z_early(e, x, y, z)) { nz++; continue; }
            uint32_t col = gradient ? 0u : (gouraud ? a.color : col_flat);
            if (gradient) {
                int cc[4];
                for (int i = 0; i < 4; i++) cc[i] = (int)(channel(a.color, i) * l0 + channel(b.color, i) * l1 + channel(c.color, i) * l2);
                col = build(cc[0], cc[1], cc[2], cc[3]);
            }
            if (e.texture) {
                float riw = 1.0f / (a.iw * l0 + b.iw * l1 + c.iw * l2);
                float uu = (a.u * l0 + b.u * l1 + c.u * l2) * riw;
                float vv = (a.v * l0 + b.v * l1 + c.v * l2) * riw;
                int lvl = 0;
                if (nmax) {
                    float dux = (dUx - uu * dWx) * riw, dvx = (dVx - vv * dWx) * riw;
                    float duy = (dUy - uu * dWy) * riw, dvy = (dVy - vv * dWy) * riw;
                    float r2 = std::fmax(dux * dux + dvx * dvx, duy * duy + dvy * dvy);
                    lvl = lod_of(r2);
                    if (lvl > nmax) lvl = nmax;
                }
                uint32_t tex_c = e.chain ? sample_cache(*e.chain, lvl, uu, vv, e.bilinear)
                                       : sample_tex(e.t, lvl, uu, vv, e.bilinear);
#if TTT_DIAG
                if (x == spy_x && y == spy_y) { spy_texel = tex_c; spy_vcol = col; spy_u = uu; spy_v = vv; }
#endif
                col = apply_texture2(col, tex_c, e.tf);
            }
            blend_pixel(e, x, y, col, z, e.early_z);
        }
        w0f += B12; w1f += B20; w2f += B01;
    }
    cnt[0] += np; cnt[1] += nz;
    if (e.atest) cnt[2] += np;
    if (!e.ztest) cnt[3] += np;
    if (e.stest) cnt[4] += np;
}

// clipping against w > eps (near plane) in clip space, then rasterization
struct VC { float c[4]; float uv[2]; uint32_t color; float vp[4]; uint32_t fn = 0; float pc[4]; uint8_t pk = 0; };

static VC interp(const VC& p, const VC& q, float t) {
    VC r;
    for (int i = 0; i < 4; i++) r.c[i] = p.c[i] + (q.c[i] - p.c[i]) * t;
    r.uv[0] = p.uv[0] + (q.uv[0] - p.uv[0]) * t; r.uv[1] = p.uv[1] + (q.uv[1] - p.uv[1]) * t;
    for (int i = 0; i < 3; i++) r.vp[i] = p.vp[i] + (q.vp[i] - p.vp[i]) * t;
    r.vp[3] = 0.0f;
    r.fn = 0;
    for (int i = 0; i < 4; i++) r.pc[i] = p.pc[i] + (q.pc[i] - p.pc[i]) * t;
    r.pk = p.pk & q.pk;
    if (p.fn && q.fn) { float a[3], b[3]; fn_unpack(p.fn, a); fn_unpack(q.fn, b); float x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t, z = a[2] + (b[2] - a[2]) * t, l = std::sqrt(x * x + y * y + z * z); if (l > 1e-6f) r.fn = fn_pack(x / l, y / l, z / l); }
    int cc[4];
    for (int i = 0; i < 4; i++) cc[i] = (int)(channel(p.color, i) + (channel(q.color, i) - channel(p.color, i)) * t);
    r.color = build(cc[0], cc[1], cc[2], cc[3]);
    return r;
}

static void read_clip(const Fmt& fm, uint32_t d, VC& o) {
    Vert v = read_vert(fm, d);
    to_clip(v, o.c, &o.color, o.uv);
    memcpy(o.vp, tc_vp, sizeof(o.vp));
    o.fn = tc_fn;
    memcpy(o.pc, tc_pc, sizeof(o.pc)); o.pk = tc_pk;
    if (toon_now && fm.pos) {   // identity of the vertex for normal smoothing: its raw model position, per fighter draw
        int nb = 3 * (fm.pos == 3 ? 4 : fm.pos);
        uint32_t h = 2166136261u ^ (toon_epoch * 0x9E3779B9u);
        for (int i = 0; i < nb; i++) h = (h ^ psp_l8(d + fm.o_pos + i)) * 16777619u;
        if (!h) h = 1;
        memcpy(&o.vp[3], &h, 4);
    }
}

// previous frame screen position of a clipped vertex (same viewport as to_screen); far jumps stay unpaired
static float SM_JUMP = 0.25f;
static void sm_vp(VP& p, const VC& c) {
    p.mk = 0; p.mx = p.x; p.my = p.y; p.mw = c.c[3];
    if (!c.pk || c.pc[3] <= 1e-4f || cap_zoom || rm_zoom) return;
    float iw = 1.0f / c.pc[3];
    float ox = (R[0x4C] & 0xFFFF) / 16.0f, oy = (R[0x4D] & 0xFFFF) / 16.0f;
    float ke = (float)scale_int();
    float x = (f24(R[0x45]) + f24(R[0x42]) * c.pc[0] * iw - ox) * ke;
    float y = (f24(R[0x46]) + f24(R[0x43]) * c.pc[1] * iw - oy) * ke;
    float lim = SM_JUMP * 480.0f * ke;
    if (std::fabs(x - p.x) > lim || std::fabs(y - p.y) > lim) return;
    p.mx = x; p.my = y; p.mw = c.pc[3]; p.mk = 1;
}

static void triangle_3d_vc(const VC* in, bool texture, bool cleared, bool inv) {
    if (sc_act >= 0) sc_in[sc_act]++;
    VC out[16];
    const float eps = 1e-4f;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const VC& p = in[i]; const VC& q = in[(i + 1) % 3];
        bool pin = p.c[3] > eps, qin = q.c[3] > eps;
        if (pin) out[n++] = p;
        if (pin != qin) out[n++] = interp(p, q, (eps - p.c[3]) / (q.c[3] - p.c[3]));
    }
    if (n >= 3) {
        const float G = 8.0f;
        bool big = false;
        for (int i = 0; i < n && !big; i++) big = std::fabs(out[i].c[0]) > G * out[i].c[3] || std::fabs(out[i].c[1]) > G * out[i].c[3];
        if (big && guard_clip()) {
            VC tmp[16];
            for (int pl = 0; pl < 4 && n >= 3; pl++) {
                int ax = pl >> 1; float sg = (pl & 1) ? 1.0f : -1.0f;
                int m = 0;
                for (int i = 0; i < n; i++) {
                    const VC& p = out[i]; const VC& q = out[(i + 1) % n];
                    float dp = G * p.c[3] + sg * p.c[ax], dq = G * q.c[3] + sg * q.c[ax];
                    if (dp >= 0) tmp[m++] = p;
                    if ((dp >= 0) != (dq >= 0) && m < 16) tmp[m++] = interp(p, q, dp / (dp - dq));
                    if (m >= 15) break;
                }
                memcpy(out, tmp, sizeof(VC) * m); n = m;
            }
        }
    }
    if (n < 3) { tris_out++; sc_close++; if (sc_act >= 0) sc_near[sc_act]++; PERF_N(PC_TRIOUT, 1); return; }
    bool gouraud = R[0x50] & 1;
    VP p0 = to_screen(out[0].c, out[0].color, out[0].uv);
    memcpy(p0.vp, out[0].vp, sizeof(p0.vp)); p0.fn = out[0].fn;
    if (sm_rec) sm_vp(p0, out[0]);
    for (int i = 1; i + 1 < n; i++) {
        VP p1 = to_screen(out[i].c, out[i].color, out[i].uv);
        VP p2 = to_screen(out[i + 1].c, out[i + 1].color, out[i + 1].uv);
        memcpy(p1.vp, out[i].vp, sizeof(p1.vp)); memcpy(p2.vp, out[i + 1].vp, sizeof(p2.vp)); p1.fn = out[i].fn; p2.fn = out[i + 1].fn;
        if (sm_rec) { sm_vp(p1, out[i]); sm_vp(p2, out[i + 1]); }
        rasterize(p0, p1, p2, texture, cleared, gouraud, inv);
    }
    prims_3d_ok++;
    sc_draw++;
}

static void triangle_3d(const Vert& va, const Vert& vb, const Vert& vc, bool texture, bool cleared, bool inv) {
    VC in[3];
    const Vert* vs[3] = {&va, &vb, &vc};
    for (int i = 0; i < 3; i++) { to_clip(*vs[i], in[i].c, &in[i].color, in[i].uv); memcpy(in[i].vp, tc_vp, sizeof(tc_vp)); in[i].fn = tc_fn; memcpy(in[i].pc, tc_pc, sizeof(tc_pc)); in[i].pk = tc_pk; }
    triangle_3d_vc(in, texture, cleared, inv);
}

static int traces = 0;

static int trace_vram = 0;
static int alpha_dump = 0;
static uint32_t vram_dir[6]; static int vram_cnt[6], vram_n = 0;

static bool vram_ok(uint32_t dir) {
    for (int i = 0; i < vram_n; i++) if (vram_dir[i] == dir) return vram_cnt[i]++ < 4;
    if (vram_n < 6) { vram_dir[vram_n] = dir; vram_cnt[vram_n++] = 1; return true; }
    return false;
}

static uint32_t raw_index(const Tex& t, int u, int v) {
    if (t.fmt == 7) return psp_l32(texel_addr(t, u, v, 32));
    if (t.fmt == 6) return psp_l16(texel_addr(t, u, v, 16));
    if (t.fmt == 5) return psp_l8(texel_addr(t, u, v, 8));
    return 0;
}

static void trace_vram_on(const Vert& a, const Vert& b, const Tex& t) {
    FB f = fb();
    uint32_t cf = R[0xC5];
    printf("2D vram %d: destino (%.0f,%.0f)-(%.0f,%.0f) uv (%.1f,%.1f)-(%.1f,%.1f) | tex 0x%08X %dx%d fmt %d swz %d stride %d wrap %u/%u filtro %u/%u | tfunc 0x%06X tenv 0x%06X color vert 0x%08X | mezcla %u eq/fac 0x%06X fijo 0x%06X/0x%06X | atest %u/0x%06X | fb 0x%08X fmt %d ancho %d | scissor (%u,%u)-(%u,%u)\n",
           trace_vram, a.x, a.y, b.x, b.y, a.u, a.v, b.u, b.v,
           t.dir, t.w, t.h, t.fmt, t.swz, t.stride, R[0xC7] & 1, (R[0xC7] >> 8) & 1, R[0xC6] & 7, (R[0xC6] >> 8) & 7,
           R[0xC9], R[0xCA], b.color, R[0x21] & 1, R[0xDF], R[0xE0], R[0xE1], R[0x22] & 1, R[0xDB],
           f.dir, f.fmt, f.width, R[0xD4] & 0x3FF, (R[0xD4] >> 10) & 0x3FF, R[0xD5] & 0x3FF, (R[0xD5] >> 10) & 0x3FF);
    printf("   paleta fmt %u shift %u mask 0x%02X off %u bytes %d entradas %d |", cf & 3, (cf >> 2) & 31, (cf >> 8) & 0xFF, (cf >> 16) & 31, clut_bytes, clut_bytes / ((cf & 3) == 3 ? 4 : 2));
    for (int i = 0; i < 16; i++) printf(" %08X", clut_entry(i));
    printf("\n");
    if (t.fmt == 7) {
        static int hist[256];
        memset(hist, 0, sizeof(hist));
        for (int y = 0; y < 272; y++) for (int x = 0; x < 480; x++) hist[(psp_l32(t.dir + (y * t.stride + x) * 4) >> 24) & 0xFF]++;
        printf("   alfa fuente:");
        for (int k = 0; k < 10; k++) {
            int best_i = 0;
            for (int i = 1; i < 256; i++) if (hist[i] > hist[best_i]) best_i = i;
            if (!hist[best_i]) break;
            printf(" %02X x%d (idx %02X);", best_i, hist[best_i], best_i & ((cf >> 8) & 0xFF));
            hist[best_i] = 0;
        }
        printf("\n");
        if (!alpha_dump) {
            alpha_dump = 1;
            static uint32_t apx[480 * 272];
            for (int y = 0; y < 272; y++) for (int x = 0; x < 480; x++) {
                uint32_t a = (psp_l32(t.dir + (y * t.stride + x) * 4) >> 24) & 0xFF;
                apx[y * 480 + x] = 0xFF000000u | a | (a << 8) | (a << 16);
            }
            char name[64];
            snprintf(name, sizeof(name), "alfa_%08X.png", t.dir);
            save_png(name, apx, 480, 272);
        }
    }
    printf("   muestra:");
    int pu[3] = {(int)a.u + 8, (int)((a.u + b.u) / 2), (int)b.u - 8};
    int pv[3] = {(int)a.v + 8, (int)((a.v + b.v) / 2), (int)b.v - 8};
    for (int i = 0; i < 3; i++) printf(" (%d,%d) crudo %08X -> %08X;", pu[i], pv[i], raw_index(t, pu[i], pv[i]), texel(t, pu[i], pv[i]));
    printf("\n");
}

// TTT_OUTLINE: outline width in window pixels (2 = one per native pixel); TTT_OUTLINE_WINDOW=0 measures it in internal pixels
static float outline_var = -1.0f;
static int outline_win = -1;
static float outline_width() {
    if (outline_var < 0.0f) { const char* e = getenv("TTT_OUTLINE"); outline_var = e ? (float)atof(e) : 2.0f; }
    return outline_var;
}
void psp_window_target(int* w, int* h);

static void sprite(const Vert& a, const Vert& b, bool texture, bool cleared) {
    FB f = fb();
    Tex t = tex();
#if TTT_DIAG
    if (texture && t.dir >= 0x04000000u && t.dir < 0x04200000u && vram_ok(t.dir)) { trace_vram++; trace_vram_on(a, b, t); }
#endif
    if (traces < 6 && (traces < 2 || texture)) {
        traces++;
        uint32_t tx = texture ? texel(t, (int)((a.u + b.u) / 2), (int)((a.v + b.v) / 2)) : 0;
        printf("GE sprite %d: (%.0f,%.0f)-(%.0f,%.0f) z %.0f color 0x%08X %s%s | fb 0x%08X ancho %d fmt %d | tex 0x%08X %dx%d fmt %d swz %d stride %d uv (%.0f,%.0f)-(%.0f,%.0f) texel 0x%08X clut fmt %d n %d [%08X %08X %08X %08X] | tfunc 0x%06X blend %d/0x%06X atest %d/0x%06X mask 0x%06X/%02X scissor (%d,%d)-(%d,%d)\n",
               traces, a.x, a.y, b.x, b.y, b.z, b.color, texture ? "tex" : "plano", cleared ? " BORRADO" : "",
               f.dir, f.width, f.fmt, t.dir, t.w, t.h, t.fmt, t.swz, t.stride, a.u, a.v, b.u, b.v, tx,
               R[0xC5] & 3, clut_bytes, clut_color(0), clut_color(1), clut_color(2), clut_color(3), R[0xC9], R[0x21] & 1, R[0xDF], R[0x22] & 1, R[0xDB], R[0xE8], R[0xE9] & 0xFF,
               R[0xD4] & 0x3FF, (R[0xD4] >> 10) & 0x3FF, R[0xD5] & 0x3FF, (R[0xD5] >> 10) & 0x3FF);
    }
    float kr = (float)t.rt;
    float ua = a.u * kr, va_ = a.v * kr, ub = b.u * kr, vb_ = b.v * kr;
    float ax = a.x, ay = a.y, bx = b.x, by = b.y;
    int ke = scale_int();
    if (outline_win < 0) { const char* e = getenv("TTT_OUTLINE_WINDOW"); outline_win = e ? atoi(e) : 1; }
    if (texture && outline_win && outline_width() > 0.0f) {
        int dw = 0, dh = 0;
        psp_window_target(&dw, &dh);
        float g = outline_width() * 0.5f, lim = 1.5f * ke;
        float kx = dw > 0 ? std::min(1.0f, g * 480.0f / dw) : 1.0f, ky = dh > 0 ? std::min(1.0f, g * 272.0f / dh) : 1.0f;
        float d0 = ax - ua, d1 = bx - ub;
        if (d0 != 0.0f && d0 > -lim && d0 < lim && d1 - d0 < 0.01f && d0 - d1 < 0.01f) { ua = std::floor(ax) - d0 * kx; ub = std::floor(bx) - d1 * kx; }
        d0 = ay - va_; d1 = by - vb_;
        if (d0 != 0.0f && d0 > -lim && d0 < lim && d1 - d0 < 0.01f && d0 - d1 < 0.01f) { va_ = std::floor(ay) - d0 * ky; vb_ = std::floor(by) - d1 * ky; }
    } else if (texture && ke > 1 && outline_width() > 0.0f && outline_width() < ke) {
        float lim = 1.5f * ke, k = outline_width() / ke;
        float d0 = ax - ua, d1 = bx - ub;
        if (d0 != 0.0f && d0 > -lim && d0 < lim && d1 - d0 < 0.01f && d0 - d1 < 0.01f) { ax = ua + d0 * k; bx = ub + d1 * k; }
        d0 = ay - va_; d1 = by - vb_;
        if (d0 != 0.0f && d0 > -lim && d0 < lim && d1 - d0 < 0.01f && d0 - d1 < 0.01f) { ay = va_ + d0 * k; by = vb_ + d1 * k; }
    }
    int x0 = (int)ax, y0 = (int)ay, x1 = (int)bx, y1 = (int)by;
    if (x1 < x0) { int s = x0; x0 = x1; x1 = s; float su = ua; ua = ub; ub = su; }
    if (y1 < y0) { int s = y0; y0 = y1; y1 = s; float sv = va_; va_ = vb_; vb_ = sv; }
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    if (texture) {
        if (skip_draw(t.dir)) return;
#if TTT_DIAG
        note_use(t, (uint64_t)w * h, 1);
#endif
    }
    TexState e;
    prepare_tex_state(e, texture, cleared, false);
    int cx0 = x0 < e.sx0 ? e.sx0 : x0, cy0 = y0 < e.sy0 ? e.sy0 : y0;
    int cx1 = x1, cy1 = y1;
    if (cx1 > e.sx1 + 1) cx1 = e.sx1 + 1;
    if (cy1 > e.sy1 + 1) cy1 = e.sy1 + 1;
    if (cx1 > e.f.width) cx1 = e.f.width;
    if (cy1 > 512 * scale_int()) cy1 = 512 * scale_int();
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    if (texture && t.dir >= 0x04000000u && t.dir < 0x04200000u && hits_target(t)) psp_gpu_flush_by(PC_VVRAM);
    Job tr;
    tr.fig = 0; tr.glow = 0;
    tr.cls = 1; tr.back = (uint8_t)ef_now;
    tr.v[0].x = (float)x0; tr.v[0].y = (float)y0; tr.v[0].u = ua; tr.v[0].v = va_;
    tr.v[1].x = (float)x1; tr.v[1].y = (float)y1; tr.v[1].u = ub; tr.v[1].v = vb_;
    tr.v[2].z = b.z; tr.col_flat = b.color;
    tr.x0 = cx0; tr.x1 = cx1 - 1; tr.y0 = cy0; tr.y1 = cy1 - 1;
    if (!defer()) {
        uint64_t cn[5] = {0, 0, 0, 0, 0};
        sprite_band(e, tr, 0, 1 << 29, cn);
        PERF_N(PC_PIX, cn[0]); PERF_N(PC_ZFAIL, cn[1]);
        PERF_N(PC_PIXA, cn[2]); PERF_N(PC_PIXNZ, cn[3]); PERF_N(PC_PIXST, cn[4]);
        return;
    }
    tr.toon = 0; tr.tint = 0; tr.grp = 0; tr.who = 0; tr.aura = 0; tr.tp = 0; tr.face = 0; tr.d3 = 0; tr.vh = 0; tr.sv = (uint8_t)split_now;
    tr.st_tex = tex_state_index(e);
    tr.signature = watch_tex() && texture ? sampling_signature(e) : 0;
    queue.push_back(tr);
    note_target(e, tr.y0, tr.y1);
    if (watch_tex()) st_copy.push_back(e);
    if (batch_cap() && (int)queue.size() >= batch_cap()) psp_gpu_flush();
}

static void sprite_band(const TexState& e, const Job& tr, int by0, int by1, uint64_t* cnt) {
    const Tex& t = e.t;
    int x0 = (int)tr.v[0].x, y0 = (int)tr.v[0].y, x1 = (int)tr.v[1].x, y1 = (int)tr.v[1].y;
    float ua = tr.v[0].u, va_ = tr.v[0].v, ub = tr.v[1].u, vb_ = tr.v[1].v;
    int w = x1 - x0, h = y1 - y0;
    float zf = tr.v[2].z;
    uint32_t bcol = tr.col_flat;
    int cx0 = tr.x0, cx1 = tr.x1 + 1;
    int cy0 = tr.y0 < by0 ? by0 : tr.y0;
    int cy1 = (tr.y1 > by1 ? by1 : tr.y1) + 1;
    if (cy0 >= cy1) return;
    uint64_t np = (uint64_t)(cx1 - cx0) * (cy1 - cy0), nz = 0;
    for (int y = cy0; y < cy1; y++) {
        float fv = h > 1 ? (float)(y - y0) / (float)(y1 - y0) : 0.0f;
        float v = va_ + (vb_ - va_) * fv;
        for (int x = cx0; x < cx1; x++) {
            float fu = w > 1 ? (float)(x - x0) / (float)(x1 - x0) : 0.0f;
            float u = ua + (ub - ua) * fu;
            if (e.early_z && !z_early(e, x, y, zf)) { nz++; continue; }
            uint32_t c = bcol;
            if (e.texture) c = apply_texture2(bcol, texel(t, (int)u, (int)v), e.tf);
            blend_pixel(e, x, y, c, zf, e.early_z);
        }
    }
    cnt[0] += np; cnt[1] += nz;
    if (e.atest) cnt[2] += np;
    if (!e.ztest) cnt[3] += np;
    if (e.stest) cnt[4] += np;
}

// 3D sprite: both corners are projected and the screen rectangle is painted
static void sprite_3d(const Vert& a, const Vert& b, bool texture, bool cleared) {
    float ca[4], cb[4], uva[2], uvb[2];
    uint32_t queue, colb;
    to_clip(a, ca, &queue, uva); to_clip(b, cb, &colb, uvb);
    if (ca[3] <= 1e-4f || cb[3] <= 1e-4f) return;
    VP pa = to_screen(ca, queue, uva), pb = to_screen(cb, colb, uvb);
    Vert sa, sb; memset(&sa, 0, sizeof(sa)); memset(&sb, 0, sizeof(sb));
    sa.x = pa.x; sa.y = pa.y; sa.z = pa.z; sa.u = pa.u / pa.iw; sa.v = pa.v / pa.iw; sa.color = queue;
    sb.x = pb.x; sb.y = pb.y; sb.z = pb.z; sb.u = pb.u / pb.iw; sb.v = pb.v / pb.iw; sb.color = colb;
    sprite(sa, sb, texture, cleared);
    prims_3d_ok++;
}

static bool is_ramp(const Tex& t) { return t.w == 32 && t.h == 256 && t.fmt == 4; }
static int trace_ramp2d = 0;

static void trace_2d(const Vert* const* vs, const Tex& t) {
    FB f = fb();
    uint32_t cf = R[0xC5];
    printf("2D rampa %d: vtype 0x%06X |", trace_ramp2d, R[0x12]);
    for (int i = 0; i < 3; i++) printf(" v%d (%.1f %.1f z %.0f uv %.2f %.2f c %08X)", i, vs[i]->x, vs[i]->y, vs[i]->z, vs[i]->u, vs[i]->v, vs[i]->color);
    printf(" | tex 0x%08X %dx%d fmt %d swz %d stride %d wrap %u/%u filtro %u/%u tfunc 0x%06X tenv 0x%06X | clut fmt %u shift %u mask 0x%02X off %u bytes %d | atest 0x%06X blend %u/0x%06X fix 0x%06X/0x%06X | ztest %u fn %u zw %s | fb 0x%08X fmt %d | scissor (%u,%u)-(%u,%u)\n",
           t.dir, t.w, t.h, t.fmt, t.swz, t.stride, R[0xC7] & 1, (R[0xC7] >> 8) & 1, R[0xC6] & 7, (R[0xC6] >> 8) & 7, R[0xC9], R[0xCA],
           cf & 3, (cf >> 2) & 31, (cf >> 8) & 0xFF, (cf >> 16) & 31, clut_bytes, R[0xDB], R[0x21] & 1, R[0xDF], R[0xE0], R[0xE1],
           R[0x23] & 1, R[0xDE] & 7, (R[0xE7] & 1) ? "no" : "si", f.dir, f.fmt,
           R[0xD4] & 0x3FF, (R[0xD4] >> 10) & 0x3FF, R[0xD5] & 0x3FF, (R[0xD5] >> 10) & 0x3FF);
    printf("   clut:");
    for (int i = 0; i < 16; i++) printf(" %08X", clut_color(i));
    printf("\n   texel en vertices: %08X %08X %08X | fila 0 cruda:", texel(t, (int)vs[0]->u, (int)vs[0]->v), texel(t, (int)vs[1]->u, (int)vs[1]->v), texel(t, (int)vs[2]->u, (int)vs[2]->v));
    for (int i = 0; i < 16; i++) printf(" %02X", psp_l8(texel_addr(t, i * 2, 0, 4)));
    printf(" | columna 0: ");
    for (int y = 0; y < 256; y += 16) printf("%08X ", texel(t, 0, y));
    printf("\n");
}

static void triangle(const Vert& a, const Vert& b, const Vert& c, bool texture, bool cleared) {
    VP p[3];
    float krt = texture ? (float)tex().rt : 1.0f;
    const Vert* vs[3] = {&a, &b, &c};
    for (int i = 0; i < 3; i++) {
        p[i].x = vs[i]->x; p[i].y = vs[i]->y; p[i].z = vs[i]->z; p[i].iw = 1.0f;
        p[i].u = vs[i]->u * krt; p[i].v = vs[i]->v * krt; p[i].color = vs[i]->color;
    }
#if TTT_DIAG
    if (texture && trace_ramp2d < 12) {
        Tex t = tex();
        if (t.dir == RAMPA_DIR || is_ramp(t)) { trace_ramp2d++; trace_2d(vs, t); }
    }
#endif
    in_2d = 1;
    rasterize(p[0], p[1], p[2], texture, cleared, R[0x50] & 1, false);
    in_2d = 0;
}

static void save_png(const char* nombre, const uint32_t* px, int w, int h);
static int trace3d = 0;
static uint32_t trace_vt[16]; static int trace_cnt[16], trace_n = 0;

static uint64_t skin_flat = 0, skin_full = 0;

static bool flat_bones(const Fmt& fm) {
    // flat matrix: Y row zero in the rotation (shadow projection or broken matrix)
    for (int b = 0; b < fm.nw && b < 8; b++)
        if (m_bone[b][1] != 0 || m_bone[b][4] != 0 || m_bone[b][7] != 0) return false;
    return true;
}

static int trace_ramp = 0;

static bool trace_ok(uint32_t vt, const Fmt& fm) {
    if (is_ramp(tex()) && trace_ramp < 8) { trace_ramp++; return true; }
    // with weights: up to 4 flat and 4 full per format; without weights: 6 per format
    bool flat = fm.wt ? flat_bones(fm) : false;
    uint32_t key = vt | (flat ? 0x80000000u : 0);
    for (int i = 0; i < trace_n; i++) if (trace_vt[i] == key) return trace_cnt[i]++ < (fm.wt ? 4 : 6);
    if (trace_n < 16) { trace_vt[trace_n] = key; trace_cnt[trace_n++] = 1; return true; }
    return false;
}

static void trace_3d(const Fmt& fm, uint32_t vt, int type, int n, uint32_t d) {
    if (!trace_ok(vt, fm)) return;
    trace3d--;
    {
        Tex t = tex();
        printf("tex vtype 0x%06X: base 0x%08X %dx%d fmt %d swz %d stride %d | niveles %d de %d | filtro min %u mag %u wrap %u/%u | TEXLEVEL 0x%06X escala (%.2f %.2f)",
               vt, t.dir, t.w, t.h, t.fmt, t.swz, t.stride, t.levels, ((R[0xC2] >> 16) & 7) + 1,
               R[0xC6] & 7, (R[0xC6] >> 8) & 7, R[0xC7] & 1, (R[0xC7] >> 8) & 1, R[0xC8],
               f24(R[0x48]), f24(R[0x49]));
        for (int i = 1; i < 8; i++) printf(" | n%d 0x%08X %dx%d sw %d", i, t.ndir[i], t.nw[i], t.nh[i], t.nstride[i]);
        printf("\n");
    }
    Vert raw_data; memset(&raw_data, 0, sizeof(raw_data));
    if (fm.pos) {
        int t = fm.pos == 3 ? 4 : fm.pos;
        raw_data.x = read_num(d + fm.o_pos, fm.pos, false); raw_data.y = read_num(d + fm.o_pos + t, fm.pos, false); raw_data.z = read_num(d + fm.o_pos + 2 * t, fm.pos, false);
    }
    Vert v = read_vert(fm, d);
    float c[4], uv[2]; uint32_t col;
    to_clip(v, c, &col, uv);
    printf("3D%s vtype 0x%06X tipo %d n %d v 0x%08X stride %d | modelo (%.3f %.3f %.3f)", fm.wt ? (flat_bones(fm) ? " PLANO" : " LLENO") : "", vt, type, n, d, fm.stride, raw_data.x, raw_data.y, raw_data.z);
    if (fm.wt) {
        int t = fm.wt == 3 ? 4 : fm.wt;
        printf(" pesos [");
        for (int i = 0; i < fm.nw; i++) printf("%s%.2f", i ? " " : "", read_weight(d + fm.o_wt + i * t, fm.wt));
        printf("] skin (%.3f %.3f %.3f)", v.x, v.y, v.z);
    }
    printf(" | clip (%.2f %.2f %.2f w %.2f)", c[0], c[1], c[2], c[3]);
    if (c[3] > 1e-4f) { VP p = to_screen(c, col, uv); printf(" pantalla (%.1f %.1f z %.0f)", p.x, p.y, p.z); }
    printf(" uv vertice (%.3f %.3f) uv final (%.1f %.1f) tmap %u/%u color %08X tex %dx%d fmt %d\n",
           v.u, v.v, uv[0], uv[1], R[0xC0] & 3, (R[0xC0] >> 8) & 3, col, tex().w, tex().h, tex().fmt);
    {
        uint32_t vcru = fm.col ? (fm.col == 7 ? psp_l32(d + fm.o_col) : convert(psp_l16(d + fm.o_col), fm.col - 4)) : 0xFFFFFFFFu;
        Tex t2 = tex();
        printf("   color crudo 0x%08X fmtcol %d | tfunc 0x%06X modo %u alfa %u doble %u | luz %u ambiente 0x%08X | mezcla %u/0x%06X | texel(0,0) 0x%08X texel(c) 0x%08X | clutfmt %u bytes %d\n",
               vcru, fm.col, R[0xC9], R[0xC9] & 7, (R[0xC9] >> 8) & 1, (R[0xC9] >> 16) & 1,
               R[0x17] & 1, R[0x5B], R[0x21] & 1, R[0xDF],
               texel(t2, 0, 0), texel(t2, t2.w / 2, t2.h / 2), R[0xC5] & 3, clut_bytes);
    }
    if ((R[0xC0] & 3) == 1) {
        printf("   tgen stq (%.5f %.5f %.6f) variante %d | matriz tgen:", tg_s, tg_t, tg_q, tmap_variant());
        for (int i = 0; i < 12; i++) printf("%s%.4f", (i % 3) ? " " : "  ", m_tgen[i]);
        printf("\n");
    }
    if (fm.wt) {
        for (int b = 0; b < fm.nw && b < 8; b++)
            printf("   hueso %d: [%.3f %.3f %.3f] [%.3f %.3f %.3f] [%.3f %.3f %.3f] t (%.3f %.3f %.3f)\n", b,
                   m_bone[b][0], m_bone[b][1], m_bone[b][2], m_bone[b][3], m_bone[b][4], m_bone[b][5],
                   m_bone[b][6], m_bone[b][7], m_bone[b][8], m_bone[b][9], m_bone[b][10], m_bone[b][11]);
    }
}

static uint32_t vert_addr(const Fmt& fm, int i) {
    if (!fm.idx) return v_addr + i * fm.stride;
    uint32_t k = fm.idx == 1 ? psp_l8(i_addr + i) : psp_l16(i_addr + i * 2);
    return v_addr + k * fm.stride;
}

static uint32_t st_en[8], st_test[8], st_op[8]; static uint64_t st_cnt[8]; static int st_n = 0;

static void note_stencil() {
    uint32_t en = R[0x24] & 1, t = R[0xDC], o = R[0xDD];
    for (int i = 0; i < st_n; i++) if (st_en[i] == en && st_test[i] == t && st_op[i] == o) { st_cnt[i]++; return; }
    if (st_n < 8) { st_en[st_n] = en; st_test[st_n] = t; st_op[st_n] = o; st_cnt[st_n++] = 1; }
}

static uint32_t tm_v[8], tm_tex[8], tm_col[8], tm_bl[8], tm_wh[8], tm_at[8]; static uint64_t tm_cnt[8]; static int tm_n = 0;
static uint32_t tm_dumped[8]; static int tm_vn = 0;

static void note_tmap(const Fmt& fm, uint32_t d) {
    Tex t = tex();
    uint32_t v = R[0xC0] & 0x303;
    uint32_t col = fm.col ? (fm.col == 7 ? psp_l32(d + fm.o_col) : convert(psp_l16(d + fm.o_col), fm.col - 4)) : 0xFFFFFFFFu;
    for (int i = 0; i < tm_n; i++) if (tm_v[i] == v && tm_tex[i] == t.dir) { tm_cnt[i]++; return; }
    if (tm_n < 8) {
        tm_v[tm_n] = v; tm_tex[tm_n] = t.dir; tm_col[tm_n] = col;
        tm_bl[tm_n] = (R[0x21] & 1) | (R[0xDF] << 1); tm_wh[tm_n] = (t.w << 16) | (t.h << 4) | t.fmt;
        tm_at[tm_n] = (R[0x22] & 1) | (R[0xDB] << 1);
        tm_cnt[tm_n++] = 1;
    }
    bool ya = false;
    for (int i = 0; i < tm_vn; i++) if (tm_dumped[i] == t.dir) ya = true;
    if (!ya && tm_vn < 8) { tm_dumped[tm_vn++] = t.dir; dump_live(t, 0); }
}

static uint32_t lz_en[8], lz_tipo[8], lz_mat[8]; static float lz_d[8][3]; static uint64_t lz_cnt[8]; static int lz_n = 0;

static void note_light() {
    uint32_t en = (R[0x17] & 1) | ((R[0x18] & 1) << 1) | ((R[0x19] & 1) << 2) | ((R[0x1A] & 1) << 3) | ((R[0x1B] & 1) << 4);
    uint32_t tp = (R[0x5F] & 0x30F) | ((R[0x60] & 0x30F) << 12);
    for (int i = 0; i < lz_n; i++) if (lz_en[i] == en && lz_tipo[i] == tp && lz_mat[i] == R[0x53]) { lz_cnt[i]++; return; }
    if (lz_n < 8) {
        lz_en[lz_n] = en; lz_tipo[lz_n] = tp; lz_mat[lz_n] = R[0x53];
        lz_d[lz_n][0] = f24(R[0x63]); lz_d[lz_n][1] = f24(R[0x64]); lz_d[lz_n][2] = f24(R[0x65]);
        lz_cnt[lz_n++] = 1;
    }
}

static uint32_t mz_v[12]; static uint64_t mz_cnt[12]; static int mz_n = 0;

static void note_blend() {
    uint32_t v = (R[0x21] & 1) ? (R[0xDF] & 0x7FF) | 0x10000u : 0;
    for (int i = 0; i < mz_n; i++) if (mz_v[i] == v) { mz_cnt[i]++; return; }
    if (mz_n < 12) { mz_v[mz_n] = v; mz_cnt[mz_n++] = 1; }
}

static uint32_t fi_f[8], fi_lvl[8]; static uint64_t fi_cnt[8]; static int fi_n = 0;

static void note_filter() {
    uint32_t f = R[0xC6] & 0x707, nv = (uint32_t)tex().levels;
    for (int i = 0; i < fi_n; i++) if (fi_f[i] == f && fi_lvl[i] == nv) { fi_cnt[i]++; return; }
    if (fi_n < 8) { fi_f[fi_n] = f; fi_lvl[fi_n] = nv; fi_cnt[fi_n++] = 1; }
}

static uint32_t bo_mode[8], bo_col[8]; static uint64_t bo_cnt[8]; static int bo_n = 0;

static void note_clear(uint32_t col) {
    for (int i = 0; i < bo_n; i++) if (bo_mode[i] == R[0xD3] && bo_col[i] == col) { bo_cnt[i]++; return; }
    if (bo_n < 8) { bo_mode[bo_n] = R[0xD3]; bo_col[bo_n] = col; bo_cnt[bo_n++] = 1; }
}

static uint64_t prims_frame = 0, prims_last = 0;
int psp_gpu_load() { return (int)prims_last; }
static int pk_tex = 0, pk_flat = 0, pk_clr = 0, pk_l[3];
static uint64_t pk_n = 0;
static float pk_lum = -1.0f;
void psp_gpu_kinds(int* tex, int* flat, int* clr, uint64_t* n, float* lum) { *tex = pk_l[0]; *flat = pk_l[1]; *clr = pk_l[2]; *n = pk_n; *lum = pk_lum; }

static int fit_var = -1;
static bool fit_2d_on() {
    if (fit_var < 0) { const char* e = getenv("TTT_2D"); fit_var = e ? atoi(e) : 1; }
    return fit_var != 0;
}

// a 2D overlay covering the whole 480x272 screen (fades, attack darkening) keeps covering the widened window
static bool full_screen_2d(const Fmt& fm, int n) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) {
        Vert v = read_vert(fm, vert_addr(fm, i));
        x0 = std::fmin(x0, v.x); x1 = std::fmax(x1, v.x); y0 = std::fmin(y0, v.y); y1 = std::fmax(y1, v.y);
    }
    return x0 <= 2.0f && y0 <= 2.0f && x1 >= 478.0f && y1 >= 270.0f;
}

// 2D uses 480x272 coordinates: it is scaled around the centre by the same
// factor as 3D so it does not deform and markers keep lining up
static inline void hud_map(Vert& p) {
    int v = hud_code & 7;
    bool ring = (hud_code >> 3) & 1, right = ((v - 1) & 1) != 0;
    float X0 = right ? 240.0f : 0.0f, Y0 = split_views == 4 ? (float)(((v - 1) >> 1) * 136) : 0.0f;
    if (ring && split_views == 2) { if (dual_full()) return; p.x = dual2() ? X0 + p.x * 0.5f : p.x + X0 - sp_crop[v <= 2 ? v : 1]; return; }
    float px = hud_flip ? 2.0f * hud_cx - p.x : p.x;
    float x = hud_cx + (px - hud_cx) * hud_sc + hud_dx, y = hud_cy + (p.y - hud_cy) * hud_sc;
    if (split_views == 4) { p.x = X0 + x * 0.5f; p.y = Y0 + y * 0.5f; }
    else if (dual_full()) { p.x = x; p.y = y; }
    else if (dual2()) { p.x = X0 + x * 0.5f; p.y = y; }
    else { p.x = X0 + x; p.y = y; }
}

// 2D drawn inside a view pass (projected effects such as the charged blast flash) follows the view like the rings
static bool view2d_on = false;
static inline void view2d_map(Vert& p) {
    int v = split_now;
    bool right = ((v - 1) & 1) != 0;
    float X0 = right ? 240.0f : 0.0f, Y0 = (float)(((v - 1) >> 1) * 136);
    if (split_views == 4) { p.x = X0 + p.x * 0.5f; p.y = Y0 + p.y * 0.5f; }
    else if (dual_full()) return;
    else p.x = dual2() ? X0 + p.x * 0.5f : p.x + X0 - sp_crop[v <= 2 ? v : 1];
}
static inline void fit_2d(Vert& v, bool corr, float ke) {
    if (hud_code && split_now) hud_map(v);
    else if (view2d_on) view2d_map(v);
    else if (dual_dup == 1 || dual_dup == 2) v.x = (dual_dup == 1 ? 120.0f : 360.0f) + (v.x - 240.0f) * 0.5f;
    else if (dual_dup == 3) v.x *= 0.5f;
    else if (dual_dup == 4) v.x = 480.0f - (480.0f - v.x) * 0.5f;
    if (corr) {
        v.x = 240.0f + (v.x - 240.0f) * asp_x;
        v.y = 136.0f + (v.y - 136.0f) * asp_y;
    }
    v.x *= ke; v.y *= ke;
}

// TTT_POINT: point and line width in native pixels. The game draws
// far characters as a point cloud; at scale N a 1-pixel point is invisible.
// TTT_LOD: the game draws far characters as a point cloud, with the same
// buffer and commands as the full mesh, changing only the primitive type.
// 1 restores the mesh. It only affects vertices with bone weights, which are the
// characters, never particles. 0 keeps the game's behaviour.
static int lod_var = -1;
static int lod_type() {
    if (lod_var < 0) {
        const char* e = getenv("TTT_LOD");
        lod_var = e ? atoi(e) : 0;
        if (lod_var == 1 || lod_var == 2) lod_var = 4;
        if (lod_var < 3 || lod_var > 5) lod_var = 0;
        if (lod_var) { printf("LOD: los personajes lejanos se dibujan como malla, tipo %d\n", lod_var); fflush(stdout); }
    }
    return lod_var;
}

static float point_var = -1.0f;
static float point_size() {
    if (point_var < 0.0f) { const char* e = getenv("TTT_POINT"); point_var = e ? (float)atof(e) : 1.0f; }
    return point_var;
}

static void frame_vp(const VP& p, float side, bool texture, bool cleared) {
    float h = side * 0.5f;
    VP a = p, b = p, c = p, d = p;
    a.x = p.x - h; a.y = p.y - h;
    b.x = p.x + h; b.y = p.y - h;
    c.x = p.x + h; c.y = p.y + h;
    d.x = p.x - h; d.y = p.y + h;
    rasterize(a, b, c, texture, cleared, false, false);
    rasterize(a, c, d, texture, cleared, false, false);
}

static void line_vp(const VP& p, const VP& q, float thickness, bool texture, bool cleared) {
    float dx = q.x - p.x, dy = q.y - p.y;
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-6f) { frame_vp(p, thickness, texture, cleared); return; }
    float nx = -dy / l * thickness * 0.5f, ny = dx / l * thickness * 0.5f;
    VP a = p, b = p, c = q, d = q;
    a.x = p.x + nx; a.y = p.y + ny;
    b.x = p.x - nx; b.y = p.y - ny;
    c.x = q.x - nx; c.y = q.y - ny;
    d.x = q.x + nx; d.y = q.y + ny;
    rasterize(a, b, c, texture, cleared, false, false);
    rasterize(a, c, d, texture, cleared, false, false);
}

static bool vp_of(const Vert& v, VP& o) {
    float c[4], uv[2]; uint32_t col;
    to_clip(v, c, &col, uv);
    if (c[3] <= 1e-4f) return false;
    o = to_screen(c, col, uv);
    return true;
}

// view_cull: a 3D primitive whose bounding box falls entirely outside the view (or the scissor) is skipped before
// transforming its vertices. 1 = only inside split views, 2 = always
// box cull, fast form (TTT_BOX_FAST: 1 on, 0 the old one, 2 alternates every 10 s and reports both for A/B in one sitting; TTT_BOX_CHECK=1
// also runs the old test in fast mode and counts different answers): positions read through a host pointer, and the 8 corners replaced
// by 5 planes (behind, left, right, top, bottom) folded once per matrix and viewport change
static uint64_t box_st[2][6];   // per mode: prims, read cycles, corner cycles, different answers, outside, milliseconds
static uint64_t box_ms() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static int box_mode() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("TTT_BOX_FAST"); v = e && *e ? atoi(e) : 1; }
    if (v != 2) return v ? 1 : 0;
    static uint32_t n = 0; static int m = 0; static uint64_t t0 = 0, last = 0, rep = 0;
    if ((n++ & 255) == 0) {
        uint64_t t = box_ms();
        if (!t0) t0 = last = rep = t;
        box_st[m][5] += t - last;
        last = t;
        m = (int)(((t - t0) / 10000) & 1);
        if (t >= rep + 20000) {
            rep = t;
            for (int k = 0; k < 2; k++) {
                double sec = box_st[k][5] ? box_st[k][5] / 1000.0 : 1.0, p = box_st[k][0] ? (double)box_st[k][0] : 1.0;
                printf("caja: %s | %.0f prims por segundo, %.0f%% fuera | lectura %.0f ciclos, esquinas %.0f ciclos por prim | respuestas distintas %llu\n", k ? "rapida" : "anterior",
                       box_st[k][0] / sec, 100.0 * box_st[k][4] / p, box_st[k][1] / p, box_st[k][2] / p, (unsigned long long)box_st[k][3]);
            }
            fflush(stdout);
        }
    }
    return m;
}
static bool box_corners(const float* lo, const float* hi);
static bool box_planes(const float* lo, const float* hi) {
    struct In { float w[12], v[12], p[16], ax, ay, sx, sy, ox, oy, X0, X1, Y0, Y1, cx, cy, gx, gy; int split; };
    static In last; static bool have = false;
    static float pl[5][4];
    In in;
    memset(&in, 0, sizeof(in));
    memcpy(in.w, m_world, 48); memcpy(in.v, m_view, 48); memcpy(in.p, m_proj, 64);
    in.ax = asp_x; in.ay = asp_y; in.split = split_now ? 1 : 0;
    if (split_now) { in.sx = sp_sx; in.sy = sp_sy; in.ox = sp_ox; in.oy = sp_oy; }
    int bx0, by0, bx1, by1;
    clip_box(bx0, by0, bx1, by1);
    float ke = (float)scale_int();
    in.X0 = bx0 / ke - 2; in.X1 = (bx1 + 1) / ke + 2; in.Y0 = by0 / ke - 2; in.Y1 = (by1 + 1) / ke + 2;
    float ox = (R[0x4C] & 0xFFFF) / 16.0f, oy = (R[0x4D] & 0xFFFF) / 16.0f;
    in.cx = f24(R[0x45]) - ox; in.cy = f24(R[0x46]) - oy; in.gx = f24(R[0x42]); in.gy = f24(R[0x43]);
    if (!have || memcmp(&in, &last, sizeof(in))) {
        have = true; last = in;
        float r[4][4];   // r[i] = (x, y, z, 1) coefficients of clip output i
        for (int c = 0; c < 4; c++) {
            float w3[3], vi[3], o[4];
            if (c < 3) {
                float d[3] = {0, 0, 0}; d[c] = 1;
                mat43(m_world, d[0], d[1], d[2], w3, false);
                mat43(m_view, w3[0], w3[1], w3[2], vi, false);
                for (int i = 0; i < 4; i++) o[i] = vi[0] * m_proj[i] + vi[1] * m_proj[4 + i] + vi[2] * m_proj[8 + i];
            } else {
                mat43(m_world, 0, 0, 0, w3, true);
                mat43(m_view, w3[0], w3[1], w3[2], vi, true);
                mat44(m_proj, vi[0], vi[1], vi[2], o);
            }
            for (int i = 0; i < 4; i++) r[i][c] = o[i];
        }
        for (int c = 0; c < 4; c++) {
            r[0][c] *= in.ax; r[1][c] *= in.ay;
            if (in.split) { r[0][c] = r[0][c] * in.sx + in.ox * r[3][c]; r[1][c] = r[1][c] * in.sy + in.oy * r[3][c]; }
            float gx = in.cx * r[3][c] + in.gx * r[0][c], gy = in.cy * r[3][c] + in.gy * r[1][c], w = r[3][c];
            pl[0][c] = w;
            pl[1][c] = gx - in.X0 * w;
            pl[2][c] = gx - in.X1 * w;
            pl[3][c] = gy - in.Y0 * w;
            pl[4][c] = gy - in.Y1 * w;
        }
    }
    float ce[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f}, he[3] = {(hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f};
    float mx[5], mn[5];
    for (int k = 0; k < 5; k++) {
        float d = pl[k][0] * ce[0] + pl[k][1] * ce[1] + pl[k][2] * ce[2] + pl[k][3];
        float m = std::fabs(pl[k][0]) * he[0] + std::fabs(pl[k][1]) * he[1] + std::fabs(pl[k][2]) * he[2];
        mx[k] = d + m; mn[k] = d - m;
    }
    return mx[0] <= 1e-4f || mx[1] < 0 || mn[2] > 0 || mx[3] < 0 || mn[4] > 0;
}
static bool prim_outside(const Fmt& fm, int n) {
    if (fm.wt || !fm.pos || n < 3) return false;
    uint64_t cb0 = perf_clock(); cp_box[2] += (uint64_t)n;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    int bm = box_mode();
    box_st[bm][0]++;
    static int lin = -1;
    if (lin < 0) { const char* e = getenv("TTT_BOX_LIN"); lin = e && *e ? atoi(e) : 1; }
    bool fast = bm && lin && !pnl_on;
    if (fast) {   // host pointer reads: the vertex block is contiguous in guest memory
        const uint8_t* vb = psp_p(v_addr + fm.o_pos);
        const uint8_t* ib = fm.idx ? psp_p(i_addr) : nullptr;
        if (fm.pos == 3) {
            for (int i = 0; i < n; i++) {
                uint32_t k = !ib ? (uint32_t)i : fm.idx == 1 ? ib[i] : (uint32_t)((const uint16_t*)ib)[i];
                float v[3]; memcpy(v, vb + (size_t)k * fm.stride, 12);
                for (int c = 0; c < 3; c++) { lo[c] = std::min(lo[c], v[c]); hi[c] = std::max(hi[c], v[c]); }
            }
        } else {
            int ilo[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, ihi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
            for (int i = 0; i < n; i++) {
                uint32_t k = !ib ? (uint32_t)i : fm.idx == 1 ? ib[i] : (uint32_t)((const uint16_t*)ib)[i];
                const uint8_t* q = vb + (size_t)k * fm.stride;
                for (int c = 0; c < 3; c++) {
                    int v; if (fm.pos == 1) v = (int8_t)q[c]; else { int16_t s16; memcpy(&s16, q + c * 2, 2); v = s16; }
                    ilo[c] = std::min(ilo[c], v); ihi[c] = std::max(ihi[c], v);
                }
            }
            float div = fm.pos == 1 ? 128.0f : 32768.0f;
            for (int c = 0; c < 3; c++) { lo[c] = (float)ilo[c] / div; hi[c] = (float)ihi[c] / div; }
        }
        uint64_t cb1 = perf_clock(); cp_box[0] += cb1 - cb0; box_st[1][1] += cb1 - cb0;
        bool out = box_planes(lo, hi);
        uint64_t cb2 = perf_clock(); cp_box[1] += cb2 - cb1; box_st[1][2] += cb2 - cb1;
        static int chk = -1;
        if (chk < 0) { const char* e = getenv("TTT_BOX_CHECK"); chk = e && *e == '1'; }
        if (chk && box_corners(lo, hi) != out) {
            box_st[1][3]++;
            static int logs = 0;
            if (logs++ < 10) { printf("caja: respuesta distinta (rapida %d), caja (%.2f %.2f %.2f)-(%.2f %.2f %.2f), %d vertices\n", out ? 1 : 0, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], n); fflush(stdout); }
        }
        if (out) box_st[1][4]++;
        return out;
    }
    if (fm.pos == 3) {
        for (int i = 0; i < n; i++) {
            uint32_t d = vert_addr(fm, i) + fm.o_pos;
            for (int k = 0; k < 3; k++) {
                float v = f32(psp_l32(d + k * 4));
                if (v < lo[k]) lo[k] = v;
                if (v > hi[k]) hi[k] = v;
            }
        }
    } else {   // integer formats: min/max on the raw values, converted once (the conversion is monotonic: same result)
        int ilo[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, ihi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
        uint32_t d0 = v_addr + fm.o_pos;
        for (int i = 0; i < n; i++) {
            uint32_t d = fm.idx ? vert_addr(fm, i) + fm.o_pos : d0 + (uint32_t)i * fm.stride;
            for (int k = 0; k < 3; k++) {
                int v = fm.pos == 1 ? (int)(int8_t)psp_l8(d + k) : (int)(int16_t)psp_l16(d + k * 2);
                if (v < ilo[k]) ilo[k] = v;
                if (v > ihi[k]) ihi[k] = v;
            }
        }
        float div = fm.pos == 1 ? 128.0f : 32768.0f;
        for (int k = 0; k < 3; k++) { lo[k] = (float)ilo[k] / div; hi[k] = (float)ihi[k] / div; }
    }
    uint64_t cb1 = perf_clock(); cp_box[0] += cb1 - cb0; box_st[0][1] += cb1 - cb0;
    bool out = box_corners(lo, hi);
    uint64_t cb2 = perf_clock(); cp_box[1] += cb2 - cb1; box_st[0][2] += cb2 - cb1;
    if (out) box_st[0][4]++;
    return out;
}
static bool box_corners(const float* lo, const float* hi) {
    static int lin = -1;
    if (lin < 0) { const char* e = getenv("TTT_BOX_LIN"); lin = e && *e ? atoi(e) : 1; }
    int bx0, by0, bx1, by1;
    clip_box(bx0, by0, bx1, by1);
    float ke = (float)scale_int();
    float X0 = bx0 / ke - 2, X1 = (bx1 + 1) / ke + 2, Y0 = by0 / ke - 2, Y1 = (by1 + 1) / ke + 2;
    float ox = (R[0x4C] & 0xFFFF) / 16.0f, oy = (R[0x4D] & 0xFFFF) / 16.0f;
    float cx = f24(R[0x45]) - ox, cy = f24(R[0x46]) - oy, sx = f24(R[0x42]), sy = f24(R[0x43]);
    int out[5] = {0, 0, 0, 0, 0};
    float b0[4], ed[3][4];
    if (lin) {   // the chain is affine: corner = low corner + the transformed box edges (4 transforms instead of 8)
        float w3[3], vi[3];
        mat43(m_world, lo[0], lo[1], lo[2], w3, true);
        mat43(m_view, w3[0], w3[1], w3[2], vi, true);
        mat44(m_proj, vi[0], vi[1], vi[2], b0);
        for (int k = 0; k < 3; k++) {
            float dd[3] = {0, 0, 0}; dd[k] = hi[k] - lo[k];
            mat43(m_world, dd[0], dd[1], dd[2], w3, false);
            mat43(m_view, w3[0], w3[1], w3[2], vi, false);
            for (int i = 0; i < 4; i++) ed[k][i] = vi[0] * m_proj[i] + vi[1] * m_proj[4 + i] + vi[2] * m_proj[8 + i];
        }
    }
    for (int c = 0; c < 8; c++) {
        float w3[3], vi[3], o[4];
        if (lin) {
            for (int i = 0; i < 4; i++) o[i] = b0[i] + ((c & 1) ? ed[0][i] : 0.0f) + ((c & 2) ? ed[1][i] : 0.0f) + ((c & 4) ? ed[2][i] : 0.0f);
        } else {
            mat43(m_world, (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2], w3, true);
            mat43(m_view, w3[0], w3[1], w3[2], vi, true);
            mat44(m_proj, vi[0], vi[1], vi[2], o);
        }
        if (asp_x != 1.0f) o[0] *= asp_x;
        if (asp_y != 1.0f) o[1] *= asp_y;
        if (split_now) { o[0] = o[0] * sp_sx + sp_ox * o[3]; o[1] = o[1] * sp_sy + sp_oy * o[3]; }
        if (pnl_on) pnl_apply(o);
        float gx = cx * o[3] + sx * o[0], gy = cy * o[3] + sy * o[1];
        out[0] += o[3] <= 1e-4f;
        out[1] += gx < X0 * o[3];
        out[2] += gx > X1 * o[3];
        out[3] += gy < Y0 * o[3];
        out[4] += gy > Y1 * o[3];
    }
    for (int k = 0; k < 5; k++) if (out[k] == 8) return true;
    return false;
}

// ge_fast: transformed vertices of an indexed triangle list are reused inside the primitive
struct VCSlot { uint32_t addr; uint64_t serial; VC v; };
static VCSlot vc_cache[1024];

static const VC& vc_get(const Fmt& fm, uint32_t d) {
    VCSlot& s = vc_cache[(d / (uint32_t)(fm.stride ? fm.stride : 4)) & 1023];
    if (s.serial != prim_serial || s.addr != d) { read_clip(fm, d, s.v); s.addr = d; s.serial = prim_serial; }
    return s.v;
}

void psp_gpu_split_time(int on) {
    static uint64_t t0 = 0;
    uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (t0 && split_pass >= 0) split_us[split_pass < 5 ? split_pass : 0] += now - t0;
    t0 = on ? now : 0;
}

static void primitive_one(uint32_t arg);
int psp_local_pause_view();
static void prim_sides(uint32_t arg, int sides) {
    uint32_t va = v_addr, ia = i_addr;
    if (sides & 1) { dual_side = 0; primitive_one(arg); }
    if (sides & 2) { if (sides & 1) { v_addr = va; i_addr = ia; } dual_side = 1; primitive_one(arg); }
    dual_side = 0;
}

// battle frames (split or shared camera): 3D outside the views and full-height 2D in both screens, bars zone (top 80
// lines) to the screen of its side, the timer and everything below in both (pause: its owner's)
static bool port_hud = false, port_both = false;   // between the port HUD markers (0xF8); both: every sprite in both screens
static void primitive_dual(uint32_t arg) {
    int n = arg & 0xFFFF;
    Fmt fm = format(R[0x12]);
    uint32_t fd = fb().dir;
    bool disp = is_disp(fd), tw = is_twinned(fd);
    // full-screen pass that reads the screen (outline post-process): in both screens, whatever pass it comes in
    if (fm.through && (R[0x1E] & 1) && !(R[0xD3] & 1) && n >= 2 && n <= 4096 && full_screen_2d(fm, n)) {
        Tex t = tex();
        if (is_disp(t.dir) || is_twinned(t.dir)) {
            static int logs = 0;
            if (logs < 24) { logs++; printf("dual: posproceso cuadro %llu pasada %d hud %d destino 0x%08X ancho %d textura 0x%08X n %d\n", (unsigned long long)alt_frame, split_pass, split_hud, norm_dir(fd), fb().width, norm_dir(t.dir), n); fflush(stdout); }
            if (!disp) twin_add(fd);
            prim_sides(arg, 3);
            return;
        }
    }
    if (split_pass) { dual_side = split_pass == 2 ? 1 : 0; primitive_one(arg); dual_side = 0; return; }
    bool battle = alt_frame - battle_seen <= 2 || split2_frame == alt_frame;
    if (!disp && !tw) {
        if (battle && fm.through && (R[0x1E] & 1) && !(R[0xD3] & 1)) {
            Tex t = tex();
            if (is_disp(t.dir) || is_twinned(t.dir)) { twin_add(fd); prim_sides(arg, 3); return; }
        }
        primitive_one(arg);
        return;
    }
    if (!battle && !tw) { primitive_one(arg); return; }
    if (!fm.through || tw || (R[0xD3] & 1) || n < 2 || n > 2048) { prim_sides(arg, 3); return; }
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) { Vert v = read_vert(fm, vert_addr(fm, i)); x0 = std::fmin(x0, v.x); x1 = std::fmax(x1, v.x); y0 = std::fmin(y0, v.y); y1 = std::fmax(y1, v.y); }
    bool straddle = x0 < 236.0f && x1 > 244.0f;
    int pv = psp_local_pause_view(), sides;
    if (port_hud) { prim_sides(arg, port_both || straddle || x1 - x0 > 470.0f ? 3 : (x0 + x1) * 0.5f < 240.0f ? 1 : 2); return; }   // port-drawn HUD: the timer and centred pieces in both screens, the rest by side
    if (x1 - x0 > 470.0f || (y0 <= 2.0f && y1 >= 270.0f)) sides = 3;
    else if (y1 > 80.0f || (straddle && x1 - x0 <= 240.0f)) sides = y1 > 80.0f && pv >= 0 ? (pv == 0 ? 1 : 2) : 3;
    else sides = (x0 + x1) * 0.5f < 240.0f ? 1 : 2;
    prim_sides(arg, sides);
}

static void primitive(uint32_t arg) {
    GvTime gvt_(((R[0x12] >> 23) & 1) ? -1 : ((R[0x12] >> 9) & 3) ? 0 : 1);
    int n = arg & 0xFFFF;
    if (dual_full()) { primitive_dual(arg); return; }
    if (dual2() && split2_frame == alt_frame && !split_pass && !hud_code && !view2d_on && n >= 2 && n <= 2048 && !(R[0xD3] & 1)) {
        Fmt fm = format(R[0x12]);
        if (fm.through && fb().width >= 480) {
            float x0 = 1e9f, x1 = -1e9f;
            for (int i = 0; i < n; i++) { Vert v = read_vert(fm, vert_addr(fm, i)); x0 = std::fmin(x0, v.x); x1 = std::fmax(x1, v.x); }
            float y1 = -1e9f;
            for (int i = 0; i < n; i++) y1 = std::fmax(y1, read_vert(fm, vert_addr(fm, i)).y);
            bool straddle = x0 < 236.0f && x1 > 244.0f;
            int pv = psp_local_pause_view();
            if (x1 - x0 <= 470.0f && (y1 > 80.0f || (straddle && x1 - x0 <= 240.0f))) {
                uint32_t va = v_addr, ia = i_addr;
                if (y1 > 80.0f && pv >= 0) { dual_dup = pv == 0 ? 1 : 2; primitive_one(arg); dual_dup = 0; return; }
                dual_dup = 1; primitive_one(arg);
                v_addr = va; i_addr = ia;
                dual_dup = 2; primitive_one(arg);
                dual_dup = 0;
                return;
            }
            dual_dup = x1 - x0 > 470.0f ? 0 : (x0 + x1) * 0.5f < 240.0f ? 3 : 4;
            primitive_one(arg);
            dual_dup = 0;
            return;
        }
    }
    primitive_one(arg);
}

// game room: a big render drawn by the menu; true = skipped (that row's model is drawn instead)
static bool room_quad(const Fmt& fm, int n, bool d3) {
    uint32_t ta = (R[0xA0] & 0xFFFFFF) | ((R[0xA8] & 0xFF0000) << 8);
    int tw = 0, th = 0, m = psp_tex_bigmenu(ta, &tw, &th);
    if (m < 0 || tw <= 0 || th <= 0) return false;
    float lo[4] = {1e9f, 1e9f, 1e9f, 1e9f}, hi[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
    float ox = (R[0x4C] & 0xFFFF) / 16.0f, oy = (R[0x4D] & 0xFFFF) / 16.0f;
    for (int i = 0; i < n && i < 64; i++) {
        uint32_t d = vert_addr(fm, i);
        float v[4];
        if (d3) {
            VC c; read_clip(fm, d, c);
            float iw = c.c[3] != 0.0f ? 1.0f / c.c[3] : 1.0f;
            v[0] = f24(R[0x45]) + f24(R[0x42]) * c.c[0] * iw - ox; v[1] = f24(R[0x46]) + f24(R[0x43]) * c.c[1] * iw - oy; v[2] = c.uv[0]; v[3] = c.uv[1];
        } else { Vert t = read_vert(fm, d); v[0] = t.x; v[1] = t.y; v[2] = t.u; v[3] = t.v; }
        for (int k = 0; k < 4; k++) { lo[k] = std::min(lo[k], v[k]); hi[k] = std::max(hi[k], v[k]); }
    }
    std::lock_guard<std::mutex> g(rm_mx);
    int k = room_row(lo[1], hi[1]);
    if (k < 0) return false;
    bool mine = rm_s[k].live;   // the row shows its model, also while Random turns its render
    if (n < 2 || hi[0] - lo[0] < 24.0f || hi[1] - lo[1] < 4.0f || hi[2] - lo[2] < 1.0f || hi[3] - lo[3] < 1.0f) return mine;   // the thin slices of the row's fade
    RoomQ q; q.menu = m; q.tw = tw; q.th = th;
    q.x0 = lo[0]; q.y0 = lo[1]; q.x1 = hi[0]; q.y1 = hi[1]; q.u0 = lo[2]; q.v0 = lo[3]; q.u1 = hi[2]; q.v1 = hi[3];
    static int clip = -1;   // TTT_ROOM3D_CLIP=0: the model clipped to the render quad, not to the game's scissor of the row
    if (clip < 0) { const char* e = getenv("TTT_ROOM3D_CLIP"); clip = e && *e ? atoi(e) : 1; }
    int sy0 = (int)((R[0xD4] >> 10) & 0x3FF), sy1 = (int)((R[0xD5] >> 10) & 0x3FF) + 1;
    bool sc = clip && sy1 - sy0 >= 24 && sy1 - sy0 <= 80;
    q.cy0 = sc ? (float)sy0 : q.y0; q.cy1 = sc ? (float)sy1 : q.y1;
    if (psp_room_cut && !d3 && sc) {
        float hy = q.y1 - q.y0, a = std::max(0.0f, (q.cy0 - q.y0) / hy), b = std::min(1.0f, (q.cy1 - q.y0) / hy);
        if (b > a) psp_room_cut(q.u0 / tw, (q.v0 + a * (q.v1 - q.v0)) / th, q.u1 / tw, (q.v0 + b * (q.v1 - q.v0)) / th, (q.x1 - q.x0) / std::max(1.0f, (b - a) * hy));
    }
    rm_row[k] = m;
    static int logs = 0;
    if (logs++ < 8) printf("gpu: sala: render grande menu %d (fila %d) en (%.1f, %.1f)-(%.1f, %.1f), texeles (%.1f, %.1f)-(%.1f, %.1f) de %dx%d, %s, recorte del juego y %d-%d%s\n", m, k, q.x0, q.y0, q.x1, q.y1, q.u0, q.v0, q.u1, q.v1, tw, th, d3 ? "3D" : "2D", sy0, sy1, sc ? " (usado)" : "");
    rm_at[k] = q; rm_atf[k] = rm_frame;
    return mine;
}

// BT3 face (bt3_hud.inc): the portrait's image and placement, so the port draws it clipped by the panel silhouette
struct BfCap { std::vector<uint32_t> px; int pw = 0, ph = 0, tw = 0, th = 0; float g[8] = {}; uint32_t col = 0, gen = 0, seq = 0; bool d3 = false; };
static std::mutex bf_mx;
static BfCap bf_cap[4];
static int bf_side = -1;
static bool lt_drop = false;
static int sz_dbg = 0;   // senzu draw of the game logged (marker 0xF9)
static void quad_box(const Fmt& fm, int n, bool d3, float* g, uint32_t* pcol) {
    float ox = (R[0x4C] & 0xFFFF) / 16.0f, oy = (R[0x4D] & 0xFFFF) / 16.0f;
    g[0] = 1e9f; g[1] = -1e9f; g[2] = 1e9f; g[3] = -1e9f; g[4] = g[5] = g[6] = g[7] = 0;
    for (int i = 0; i < n; i++) {
        uint32_t d = vert_addr(fm, i);
        float x, y, u, v;
        if (d3) {
            VC c; read_clip(fm, d, c);
            float iw = c.c[3] != 0.0f ? 1.0f / c.c[3] : 1.0f;
            x = f24(R[0x45]) + f24(R[0x42]) * c.c[0] * iw - ox; y = f24(R[0x46]) + f24(R[0x43]) * c.c[1] * iw - oy; u = c.uv[0]; v = c.uv[1];
            if (!i && fm.col) *pcol = c.color;
        } else { Vert t = read_vert(fm, d); x = t.x; y = t.y; u = t.u; v = t.v; if (!i && t.has_color) *pcol = t.color; }
        if (x < g[0]) { g[0] = x; g[4] = u; }
        if (x > g[1]) { g[1] = x; g[5] = u; }
        if (y < g[2]) { g[2] = y; g[6] = v; }
        if (y > g[3]) { g[3] = y; g[7] = v; }
    }
}
// TTT_LIST_TRACE=1: every textured draw in the technique list area, printed once per settled frame layout
struct LtQ { float g[8]; uint32_t col, dir; int w, h, fmt; };
static void list_trace(const Fmt& fm, int n, bool d3) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_LIST_TRACE"); on = e && atoi(e) ? 1 : 0; }
    if (!on || n < 2 || n > 64) return;
    Tex t = tex();
    static std::vector<LtQ> cur, prev; static uint32_t fno = 0xFFFFFFFFu; static int still = 0, shown = 0; static size_t seen[4];
    if (fno != toon_fno) {
        fno = toon_fno;
        bool same = cur.size() == prev.size() && !cur.empty() && !memcmp(cur.data(), prev.data(), cur.size() * sizeof(LtQ));
        still = same ? still + 1 : 0;
        if (still == 20 && shown < 4) {
            bool dup = false;
            for (int i = 0; i < shown; i++) dup = dup || seen[i] == cur.size();
            if (!dup) {
                seen[shown++] = cur.size();
                printf("charge: traza de %d dibujos\n", (int)cur.size());
                for (size_t i = 0; i < cur.size(); i++) printf("charge: dibujo %d (%.1f, %.1f)-(%.1f, %.1f) u %.1f..%.1f v %.1f..%.1f color 0x%08X textura 0x%08X %dx%d formato %d\n", (int)i, cur[i].g[0], cur[i].g[2], cur[i].g[1], cur[i].g[3], cur[i].g[4], cur[i].g[5], cur[i].g[6], cur[i].g[7], cur[i].col, cur[i].dir, cur[i].w, cur[i].h, cur[i].fmt);
            }
        }
        prev.swap(cur); cur.clear();
    }
    LtQ q; memset(&q, 0, sizeof q); q.col = 0xFFFFFFFFu;
    quad_box(fm, n, d3, q.g, &q.col);
    if (q.g[0] > 130 || q.g[1] < 0 || q.g[2] > 245 || q.g[3] < 160) return;   // list area of the 1v1 slot
    q.dir = t.dir; q.w = t.w; q.h = t.h; q.fmt = t.fmt;
    if (cur.size() < 96) cur.push_back(q);
}
extern std::atomic<int> psp_hud_raw;
static bool face_capture(const Fmt& fm, int n, bool d3) {
    if (n < 2 || n > 64) return true;
    float g[8];
    uint32_t col = 0xFFFFFFFFu;
    quad_box(fm, n, d3, g, &col);
    if (g[1] - g[0] < 2.0f || g[3] - g[2] < 2.0f) return true;
    Tex t = tex();
    int pw = 0, ph = 0; const uint32_t* ppx = nullptr;
    psp_tex_pin_req.store(0);
    psp_hud_raw.store(1);
    bool png = psp_tex_png && psp_tex_png(psp_p(t.dir), t.dir, clut_dir, t.w, t.h, t.stride, t.swz, t.fmt, &pw, &ph, &ppx) && ppx && pw > 0 && ph > 0;
    psp_hud_raw.store(0);
    psp_tex_pin_req.store(0);
    std::vector<uint32_t> px;
    if (png) px.assign(ppx, ppx + (size_t)pw * ph);
    else { pw = t.w; ph = t.h; px.resize((size_t)pw * ph); for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) px[(size_t)y * pw + x] = texel_n(t, 0, x, y); }
    // the game's texture function, vertex colour and blend factor go into the pixels (doubled source alpha: 0..128 is opaque)
    uint32_t tf = R[0xC9], bm = R[0xDF]; bool bl = (R[0x21] & 1) != 0, dbl = bl && (bm & 15) == 6;
    for (auto& q : px) {
        uint32_t o = apply_texture2(col, q, tf), a = o >> 24;
        a = !bl ? (a ? 255u : 0u) : dbl ? std::min(255u, a * 2) : a;
        q = (o & 0xFFFFFFu) | a << 24;
    }
    uint32_t vcol = col; col = 0xFFFFFFFFu;
    std::lock_guard<std::mutex> lk(bf_mx);
    BfCap& c = bf_cap[bf_side & 3];
    bool same = c.pw == pw && c.ph == ph && c.tw == t.w && c.th == t.h && c.col == col && !memcmp(c.g, g, sizeof g) && c.px == px;
    if (!same) { c.px.swap(px); c.pw = pw; c.ph = ph; c.tw = t.w; c.th = t.h; c.col = col; memcpy(c.g, g, sizeof g); c.d3 = d3; c.gen++; }
    c.seq++;
    static int logs = 0;
    if (!same && logs++ < 6) printf("hud bt3: retrato lado %d capturado %s (%.1f, %.1f)-(%.1f, %.1f) texeles u %.1f..%.1f v %.1f..%.1f de %dx%d, imagen %dx%d%s, color 0x%08X funcion 0x%X mezcla %d 0x%X\n", bf_side, d3 ? "3D" : "2D", g[0], g[2], g[1], g[3], g[4], g[5], g[6], g[7], t.w, t.h, pw, ph, png ? " (servida)" : "", vcol, tf, (int)bl, bm);
    return true;
}
bool psp_bt3_face(int side, uint32_t have, uint32_t* gen, uint32_t* seq, int* dim, float* geo, uint32_t* col, std::vector<uint32_t>* px) {
    std::lock_guard<std::mutex> lk(bf_mx);
    const BfCap& c = bf_cap[side & 3];
    if (!c.gen) return false;
    *gen = c.gen; *seq = c.seq; dim[0] = c.pw; dim[1] = c.ph; dim[2] = c.tw; dim[3] = c.th; memcpy(geo, c.g, sizeof c.g); *col = c.col;
    if (c.gen != have) *px = c.px;
    return true;
}

struct AtCam { uint32_t dir, vh, ph; int n; float v[12], p[16], vp[6], off[2], asp[2], ke; };
static AtCam at_vc[8], at_vp[8]; static int at_nvc = 0, at_nvp = 0; static uint32_t at_cf = 0xFFFFFFFFu;
static void atm_note(int nv) {   // every main-view 3D draw votes for its camera; the camera with most vertices per target is the scene camera
    if (at_cf != toon_fno) { at_cf = toon_fno; memcpy(at_vp, at_vc, sizeof(at_vc)); at_nvp = at_nvc; at_nvc = 0; }
    uint32_t dir = fb().dir & 0x0FFFFFFFu, ph = 2166136261u;
    for (int i = 0; i < 16; i++) { uint32_t u; memcpy(&u, &m_proj[i], 4); ph = (ph ^ u) * 16777619u; }
    for (int i = 0; i < at_nvc; i++) if (at_vc[i].dir == dir && at_vc[i].vh == view_hash && at_vc[i].ph == ph) { at_vc[i].n += nv; return; }
    if (at_nvc >= 8) return;
    AtCam& c = at_vc[at_nvc++];
    c.dir = dir; c.vh = view_hash; c.ph = ph; c.n = nv;
    memcpy(c.v, m_view, sizeof(c.v)); memcpy(c.p, m_proj, sizeof(c.p));
    c.vp[0] = f24(R[0x42]); c.vp[1] = f24(R[0x43]); c.vp[2] = f24(R[0x44]); c.vp[3] = f24(R[0x45]); c.vp[4] = f24(R[0x46]); c.vp[5] = f24(R[0x47]);
    c.off[0] = (R[0x4C] & 0xFFFF) / 16.0f; c.off[1] = (R[0x4D] & 0xFFFF) / 16.0f; c.asp[0] = asp_x; c.asp[1] = asp_y; c.ke = (float)scale_int();
}
static void atm_pick(uint32_t dir) {   // sun position A of the map on screen (BT3 StgGlare_Update), camera height and GE depth -> view depth
    const AtCam* t = at_nvc ? at_vc : at_vp; int nt = at_nvc ? at_nvc : at_nvp;
    int k = -1, ka = -1;
    for (int i = 0; i < nt; i++) {
        if (t[i].dir == dir && (k < 0 || t[i].n > t[k].n)) k = i;
        if (ka < 0 || t[i].n > t[ka].n) ka = i;
    }
    if (k < 0) k = ka;
    if (k < 0) return;
    {   // sticky: keep last frame's camera while it is still a strong candidate (big effect batches must not flip the pick)
        static uint32_t lvh = 0, lph = 0; static int cand = 0;
        int kp = -1; for (int i = 0; i < nt; i++) if (t[i].dir == dir && t[i].vh == lvh && t[i].ph == lph) kp = i;
        if (kp >= 0 && kp != k && t[kp].n * 4 >= t[k].n) { if (++cand < 4) k = kp; else cand = 0; } else cand = 0;
        if (t[k].vh != lvh || t[k].ph != lph) { lvh = t[k].vh; lph = t[k].ph; }
    }
    const AtCam& c = t[k];
    float C[16]; bool cam = false;
    uint32_t cp = psp_l32(0x08A7E25Cu);   // the GE view is only the Y/Z flip: the world matrix carries the camera, [[0x08A7E25C]+0x40] is the real one (EBOOT_TTT 68.2)
    if (cp >= 0x08800000u && cp < (patch_cam_heap.val ? 0x10000000u : 0x0A000000u)) { for (int i = 0; i < 16; i++) { uint32_t u = psp_l32(cp + 0x40u + 4u * i); memcpy(&C[i], &u, 4); } cam = C[15] == C[15] && std::fabs(C[15] - 1.0f) < 1e-3f; }   // the camera can live in the port heap (heap_spill)
    { static int cl = 0; static bool cw = true; if (cam != cw && cl < 6) { cl++; cw = cam; printf("atm: camara del juego 0x%08X %s (w %.3f)\n", cp, cam ? "leida" : "no valida", C[15]); } }
    auto proj = [&](float x, float y, float z, float* sp) {   // world -> eye (row vectors) -> GE flip -> projection -> screen
        float e[3], vi[3], o[4];
        for (int j = 0; j < 3; j++) e[j] = cam ? x * C[j] + y * C[4 + j] + z * C[8 + j] + C[12 + j] : (j == 0 ? x : j == 1 ? y : z);
        mat43(c.v, e[0], e[1], e[2], vi, true);
        mat44(c.p, vi[0], vi[1], vi[2], o);
        o[0] *= c.asp[0]; o[1] *= c.asp[1];
        float iw = o[3] > 1e-4f ? 1.0f / o[3] : 0.0f;
        sp[0] = (c.vp[3] + c.vp[0] * o[0] * iw - c.off[0]) * c.ke; sp[1] = (c.vp[4] + c.vp[1] * o[1] * iw - c.off[1]) * c.ke; sp[2] = o[3] > 1e-4f ? 1.0f : 0.0f;
    };
    static const float sg[3][3] = {{1, -1, 1}, {1, -1, -1}, {1, 1, 1}};   // the MAPD stores Y up (like the water level); the world has Y down
    int sel = std::max(0, std::min(2, (int)toon_env("TTT_FX_SUNAXES", 0)));
    float cand[3][3];
    for (int k = 0; k < 3; k++) proj(psp_map_sun[0] * sg[k][0], psp_map_sun[1] * sg[k][1], psp_map_sun[2] * sg[k][2], cand[k]);
    at_sun[0] = cand[sel][0]; at_sun[1] = cand[sel][1]; at_sun[2] = psp_map_sun[3] > 0.5f && cam ? cand[sel][2] : 0.0f;
    {   // sparking flame particles to the screen: anchor box centre + axis offset + rise; size from the box
        static int lg = 0; memset(spk_gpu, 0, sizeof(spk_gpu));
        for (int sl = 0; sl < 8; sl++) {   // anchor = this frame's fighter box: centre, half size, body axis (vertical standing, horizontal in flight)
            if (spk_f[sl] <= 0.0f) continue;
            const double* b = fg_bb[sl]; float tg[4];
            if (fg_bbok[sl] && b[0] >= 30) {
                double mx = b[1] / b[0], my = b[2] / b[0], sx = std::sqrt(std::max(0.0, b[3] / b[0] - mx * mx)), sy = std::sqrt(std::max(0.0, b[4] / b[0] - my * my));   // vertex mean and spread: a tail or an arm barely moves them
                tg[0] = (float)mx; tg[1] = (float)my; tg[2] = (float)(std::max(sx, sy) * 2.3); tg[3] = sx > sy ? 1.0f : 0.0f;
            } else if (cam) {   // no fighter triangles this frame (far LOD draw): waist and head bones projected
                const float* w = spk_p[sl]; float m[3], h[3]; proj(w[0], w[1], w[2], m); proj(w[3], w[4], w[5], h);
                if (m[2] < 0.5f || h[2] < 0.5f) continue;
                float dx = h[0] - m[0], dy = h[1] - m[1];
                tg[0] = m[0] + dx * 0.2f; tg[1] = m[1] + dy * 0.2f; tg[2] = std::hypot(dx, dy) * 1.7f; tg[3] = std::fabs(dx) > std::fabs(dy) ? 1.0f : 0.0f;
                static int lgf = 0; if (lgf < 3) { lgf++; printf("carga: llamas, casilla %d sin triangulos de luchador, ancla por huesos (%.0f %.0f) media medida %.0f\n", sl, tg[0], tg[1], tg[2]); }
            } else continue;
            if (tg[2] < 2.0f) continue;
            float ke = spk_a[sl][2] >= 2.0f ? 1.0f - std::exp(-(float)at_dt * 40.0f) : 1.0f;
            for (int j = 0; j < 4; j++) spk_a[sl][j] += (tg[j] - spk_a[sl][j]) * ke;
        }
        for (int i = 0; i < spk_fn; i++) {
            const SpkFlame& f = spk_fl[i]; const float* a = spk_a[f.slot]; float hs = a[2]; if (hs < 2.0f) continue;
            float ux = a[3], uy = 1.0f - a[3];   // body axis in screen space (x when flying flat, y standing), blended while it changes
            spk_gpu[i][0] = a[0] + (ux * f.t + uy * f.ox) * hs; spk_gpu[i][1] = a[1] + (uy * f.t + ux * f.ox + f.oy) * hs;
            spk_gpu[i][2] = f.sw * hs * 0.9f; spk_gpu[i][3] = (float)(f.slot * 1024 + f.id) + std::min(0.999f, f.age / f.life);
            if (lg < 3 && i == 0) { lg++; printf("carga: llamas, %d particulas, caja del luchador centro (%.0f %.0f) media medida %.0f eje %s\n", spk_fn, a[0], a[1], hs, a[3] > 0.5f ? "horizontal" : "vertical"); }
        }
    }
    if (cam) at_cam_y = -(C[12] * C[4] + C[13] * C[5] + C[14] * C[6]);
    at_full = c.vp[0] * 2.0f >= 470.0f;
    aw_ok = cam;
    if (cam) {   // water surface: world -> game eye (C), GE view, projection, viewport
        float* q = aw_q;
        for (int r = 0; r < 3; r++) { for (int j = 0; j < 3; j++) q[4 * r + j] = C[4 * r + j]; }
        q[3] = -(C[12] * C[0] + C[13] * C[1] + C[14] * C[2]); q[7] = -(C[12] * C[4] + C[13] * C[5] + C[14] * C[6]); q[11] = -(C[12] * C[8] + C[13] * C[9] + C[14] * C[10]);
        q[12] = C[12]; q[13] = C[13]; q[14] = C[14]; q[15] = c.ke;
        for (int r = 0; r < 3; r++) { for (int j = 0; j < 3; j++) q[16 + 4 * r + j] = c.v[3 * r + j]; q[19 + 4 * r] = c.v[9 + r]; }
        q[28] = c.p[0]; q[29] = c.p[5]; q[30] = c.p[8]; q[31] = c.p[9]; q[32] = c.p[12]; q[33] = c.p[13]; q[34] = c.asp[0]; q[35] = c.asp[1];
        q[36] = c.vp[0]; q[37] = c.vp[1]; q[38] = c.vp[3] - c.off[0]; q[39] = c.vp[4] - c.off[1];
    }
    {   // BT3 StgPanBlur_UpdateView: strength from the camera's horizontal move (Y dropped), side from the turn around the origin
        static float pp[3] = {0, 0, 0}, st = 0; static bool ph = false;
        float px = 0, pz = 0;
        if (cam) { px = -(C[12] * C[0] + C[13] * C[1] + C[14] * C[2]); pz = -(C[12] * C[8] + C[13] * C[9] + C[14] * C[10]); }
        float k = (float)std::max(0.25, std::min(4.0, at_dt * 60.0));
        if (!cam || !ph) { pp[0] = px; pp[2] = pz; ph = cam; st = 0; }
        float mx = pp[0] - px, mz = pp[2] - pz, dist = std::sqrt(mx * mx + mz * mz) / k;
        float l0 = std::sqrt(pp[0] * pp[0] + pp[2] * pp[2]), l1 = std::sqrt(px * px + pz * pz);
        float sy = l0 > 1e-4f && l1 > 1e-4f ? (pp[2] * px - pp[0] * pz) / (l0 * l1) : 0.0f;
        pp[0] = px; pp[2] = pz;
        if (dist >= 1.0f) { if (dist < 2.5f) st += 0.1f * k; else if (dist < 4.0f) st -= 0.15f * k; else if (dist > 4.0f) st += 0.1f * k; }
        else st -= 0.15f * k;
        st = std::max(0.0f, std::min(1.0f, st));
        float sd = std::max(-1.0f, std::min(1.0f, sy * 100.0f / k)), pk = std::max(0.0f, toon_env("TTT_FX_PANK", 1.0f));
        for (int i = 0; i < 2; i++) { at_pan[2 * i] = 2.0f * (i + 1) * sd; at_pan[2 * i + 1] = st > 0 ? std::min(1.0f, std::max(0.0f, 101.0f * st - 32.0f * i) / 128.0f * pk) : 0.0f; }
    }
    at_zk[0] = c.vp[5]; at_zk[1] = c.vp[2]; at_zk[2] = c.p[10]; at_zk[3] = c.p[14]; at_zk[4] = c.p[11]; at_zk[5] = c.p[15];
    if (std::fabs(at_zk[1]) < 1e-6f) at_zk[1] = 1.0f;
    at_ok = true;
}

// smooth motion: which draw of this vertex and index buffer this is in the frame (two fighters with one model, outline passes)
static void sm_prim(bool d3) {
    sm_rec = sm_on && d3 && !split_now && fb().width >= 480 * scale_int();
    if (!sm_rec) return;
    uint32_t h = sm_h(v_addr, i_addr) & 4095u;
    for (int k = 0; k < 16; k++, h = (h + 1) & 4095u) {
        SmOcc& o = sm_occ_tab[h];
        if (o.stamp != sm_fno) { o.a = v_addr; o.b = i_addr; o.stamp = sm_fno; o.n = 0; sm_occ = 0; return; }
        if (o.a == v_addr && o.b == i_addr) { sm_occ = ++o.n; return; }
    }
    sm_rec = false;
}

// GPU vertex path: a non-skinned 3D list/strip/fan becomes one indexed GL draw (vertices written once, GL clips and assembles)
static int gv_var = -1;
static uint64_t gv_n[6], gv_old[8];   // 3D list/strip/fan prims, taken, vertices, triangles, dropped, -; vertices left on the old path by reason
static bool gv_on() {
    if (gv_var < 0) { const char* e = getenv("TTT_GPU_VTX"); gv_var = e && *e ? atoi(e) : 1; }
    return gv_var > 0;
}
void psp_gpu_vtx_toggle() {
    gv_var = gv_on() ? 0 : 1;
    printf("vtx: camino de vertices por GPU %s (Insert)\n", gv_var ? "encendido" : "apagado"); fflush(stdout);
}
void psp_gpu_copy_toggle() {
    glr_copy_part_v = glr_copy_part() ? 0 : 1;
    printf("copias: parciales %s (Supr)\n", glr_copy_part_v ? "encendidas" : "apagadas"); fflush(stdout);
}
void psp_gpu_lite_toggle() {
    glr_lite_var = glr_lite_on() ? 0 : 1;
    printf("gl: programa ligero %s (Bloq Despl)\n", glr_lite_var ? "encendido" : "apagado"); fflush(stdout);
}
void psp_gpu_olbox_toggle() {
    atb_var = atb_on() ? 0 : 1;
    printf("gpu: carga sola sin copia y en caja %s (Pausa)\n", atb_var ? "encendida" : "apagada"); fflush(stdout);
}
#ifdef _WIN32
#include <cpuid.h>
static const int GV_MAXCH = 16;
struct GvJob {
    VtxC C; Fmt fm; const uint32_t* adr; VtxGL* ov; GvX* ox; float* sk;
    float sw, sh, tw, th; int itw, ith; bool tt; uint32_t tint, tp, epoch;
    float A[3], B[3];
    struct Part { float x0, x1, y0, y1; bool behind; } part[GV_MAXCH];
    int nu, nch;
};
static GvJob gv_job;
static void gv_chunk(int c);
static void (*gv_run_fn)(int) = gv_chunk;
struct GvPool { std::atomic<uint64_t> ticket{0}; std::atomic<uint32_t> gen{0}, done{0}; std::atomic<int> sleepers{0}; std::mutex m; std::condition_variable cv; };
static GvPool* gv_pool = nullptr;
static int gv_mt_var = -1, gv_mt_chk = -1, gv_mt_min = -1, gv_mt_thr = 1, gv_mt_ready = -1;
static bool gv_rep_reset = false;
static uint64_t gv_mt_n[4], gv_mt_t[2], gv_p[5];
static bool gv_mt_on() {
    if (gv_mt_var < 0) { const char* e = getenv("TTT_GPU_MT"); gv_mt_var = e && *e ? atoi(e) : 0; }
    return gv_mt_var > 0;
}
static int gv_mt_minv() { return gv_mt_var == 2 ? std::max(gv_mt_min, 160) : gv_mt_min; }
static int gv_df_var = -1;
static bool gv_df_on() { if (gv_df_var < 0) { const char* e = getenv("TTT_GPU_DEFER"); gv_df_var = e && *e ? atoi(e) : 1; } return gv_df_var > 0; }
void psp_gpu_mt_toggle() {
    gv_df_var = gv_df_on() ? 0 : 1;
    gv_rep_reset = true;
    printf("vtx: vertices diferidos en hilos %s (Bloq Despl)\n", gv_df_var ? "encendidos" : "apagados"); fflush(stdout);
}
static void gv_chunk(int c) {
    GvJob& J = gv_job;
    int a = (int)((int64_t)J.nu * c / J.nch), b = (int)((int64_t)J.nu * (c + 1) / J.nch);
    GvJob::Part& P = J.part[c];
    P.x0 = 1e30f; P.x1 = -1e30f; P.y0 = 1e30f; P.y1 = -1e30f; P.behind = false;
    const float eps = 1e-4f;
    for (int j = a; j < b; j++) {
        uint32_t d = J.adr[j];
        float* skp = J.sk ? J.sk + 7 * j : nullptr;
        if (skp) skp[6] = 0.0f;
        Vert v = read_vert(J.fm, d, skp);
        float c4[4], uv[2], vp[4]; uint32_t col, fn;
        to_clip_core(J.C, v, c4, &col, uv, vp, &fn, J.itw, J.ith);
        VtxGL& o = J.ov[j];
        if (c4[3] > eps) {
            VP p = to_screen_c(J.C, c4, col, uv);
            float w = (p.iw > 1e-20f || p.iw < -1e-20f) ? 1.0f / p.iw : 1.0f;
            o.x = (2.0f * p.x / J.sw - 1.0f) * w; o.y = (2.0f * p.y / J.sh - 1.0f) * w; o.z = (2.0f * (p.z / 65535.0f) - 1.0f) * w; o.w = w;
            o.u = (p.u * w) / J.tw; o.v = (p.v * w) / J.th;
            P.x0 = std::min(P.x0, p.x); P.x1 = std::max(P.x1, p.x); P.y0 = std::min(P.y0, p.y); P.y1 = std::max(P.y1, p.y);
        } else {
            P.behind = true;
            o.x = (2.0f * J.B[0] / J.sw - 1.0f) * c4[3] + 2.0f * J.A[0] / J.sw * c4[0];
            o.y = (2.0f * J.B[1] / J.sh - 1.0f) * c4[3] + 2.0f * J.A[1] / J.sh * c4[1];
            o.z = (2.0f * J.B[2] / 65535.0f - 1.0f) * c4[3] + 2.0f * J.A[2] / 65535.0f * c4[2];
            o.w = c4[3]; o.u = uv[0] / J.tw; o.v = uv[1] / J.th;
        }
        o.col = col; o.tl = -9.0f; o.tf = 0.0f; o.tint = J.tt ? J.tint : 0; o.ext = 0; o.tp = J.tt ? J.tp : 0; o.wd = -1e4f; o.wc = o.wet = o.wpad = 0.0f; o.nrm = o.lgt = 0;
        o.px = vp[0]; o.py = vp[1]; o.pz = vp[2]; o.pf = 0.0f;
        GvX x = {0u, J.tt ? fn : 0u};
        if (J.tt && J.fm.pos) {
            int nb = 3 * (J.fm.pos == 3 ? 4 : J.fm.pos);
            uint32_t h = 2166136261u ^ (J.epoch * 0x9E3779B9u);
            for (int i = 0; i < nb; i++) h = (h ^ psp_l8(d + J.fm.o_pos + i)) * 16777619u;
            x.key = h ? h : 1;
        }
        J.ox[j] = x;
    }
}
static bool gv_claim(uint32_t g, int* c) {
    uint64_t t = gv_pool->ticket.load();
    for (;;) {
        if ((uint32_t)(t >> 32) != g) return false;
        uint32_t idx = (uint32_t)(t & 0xFFFF), nch = (uint32_t)((t >> 16) & 0xFFFF);
        if (idx >= nch) return false;
        if (gv_pool->ticket.compare_exchange_weak(t, t + 1)) { *c = (int)idx; return true; }
    }
}
static void gv_worker() {
    GvPool* P = gv_pool;
    uint32_t seen = 0;
    for (;;) {
        uint64_t t0 = perf_clock();
        uint32_t g;
        while ((g = P->gen.load()) == seen) {
            if (perf_clock() - t0 > 250000) {
                std::unique_lock<std::mutex> lk(P->m);
                P->sleepers++;
                P->cv.wait(lk, [&] { return P->gen.load() != seen; });
                P->sleepers--;
            } else _mm_pause();
        }
        seen = g;
        int c;
        while (gv_claim(g, &c)) { gv_run_fn(c); P->done.fetch_add(1); }
    }
}
static bool gv_mt_init() {
    if (gv_mt_ready >= 0) return gv_mt_ready > 0;
    gv_mt_ready = 0;
    char brand[49] = {0};
    unsigned r[4];
    for (unsigned i = 0; i < 3; i++) { __get_cpuid(0x80000002 + i, &r[0], &r[1], &r[2], &r[3]); memcpy(brand + 16 * i, r, 16); }
    unsigned hc = std::thread::hardware_concurrency();
    int total = hc >= 8 ? 4 : hc >= 4 ? 3 : hc >= 2 ? 2 : 1;
    const char* e = getenv("TTT_GPU_MT_N");
    if (e && *e) total = std::max(1, std::min(8, atoi(e)));
    e = getenv("TTT_GPU_MT_MIN");
    gv_mt_min = e && *e ? std::max(8, atoi(e)) : 32;
    e = getenv("TTT_GPU_MT_CHECK");
    gv_mt_chk = e && *e ? atoi(e) : 0;
    printf("vtx: hilos de calculo: CPU \"%s\", hardware_concurrency %u, hilos %d (trabajadores %d), minimo %d vertices, verificacion %d\n", brand, hc, total, total - 1, gv_mt_min, gv_mt_chk); fflush(stdout);
    if (total < 2) return false;
    ge_fast2(); through_z_u16(); zbias(); ge_light_att(); tmap_variant(); scale_int();
    { SProf warm(0); }
    gv_mt_thr = total;
    gv_pool = new GvPool();
    for (int i = 1; i < total; i++) std::thread(gv_worker).detach();
    gv_mt_ready = 1;
    return true;
}
static void gv_mt_dispatch(int nch) {
    GvPool* P = gv_pool;
    uint64_t t0 = perf_clock();
    P->done.store(0);
    uint32_t g = P->gen.load() + 1;
    P->ticket.store(((uint64_t)g << 32) | ((uint64_t)nch << 16));
    gv_mt_active.store(1);
    P->gen.store(g);
    if (P->sleepers.load()) { std::lock_guard<std::mutex> lk(P->m); P->cv.notify_all(); }
    int c;
    while (gv_claim(g, &c)) { gv_run_fn(c); P->done.fetch_add(1); }
    uint64_t w0 = perf_clock();
    for (int sp = 0; P->done.load() < (uint32_t)nch; sp++) { if (sp < 2000) _mm_pause(); else std::this_thread::yield(); }
    uint64_t t1 = perf_clock();
    gv_mt_t[0] += w0 - t0; gv_mt_t[1] += t1 - w0;
    gv_mt_active.store(0);
}
#endif
// deferred vertex path: each GPU prim is snapshotted when the GE draws it; worker threads compute it while the GE goes on, the flush joins
struct GvD {
    Fmt fm; GvXf X; float bn[8][12]; uint32_t ci, amb, a0, nu; int qi, itw, ith;
    float sw, sh, tw, th, A[3], B[3]; bool tt; uint32_t tint, tp, epoch;
    VtxGL* ov; GvX* ox;
    float x0, x1, y0, y1; bool behind; int bx0, by0, bx1, by1;
    bool ag; float fw, fh; const uint32_t* ix; uint32_t ni, base; GvAg g; float* tno;
};
static const uint32_t GVD_N = 16384, GVC_N = 4096, GVA_N = 1u << 20;
static GvD* gv_dq = nullptr; static VtxC* gv_dc = nullptr; static uint32_t* gv_da = nullptr;
static uint32_t gv_dqn = 0, gv_dcn = 0, gv_dan = 0, gv_wake = 0;
static VtxC gv_kc;
static std::atomic<uint32_t> gv_pub{0}, gv_clm{0}, gv_dn{0};
static std::atomic<int> gv_slp{0};
static std::mutex* gv_am = nullptr; static std::condition_variable* gv_acv = nullptr;   // never destroyed: waiters at exit
static int gv_dk_var = -1, gv_dk_cnt = 0, gv_aw = -1;
static uint64_t gv_df_n[8], gv_df_t[2];
static int gv_dk() { if (gv_dk_var < 0) { const char* e = getenv("TTT_GPU_DEFER_CHECK"); gv_dk_var = e && *e ? atoi(e) : 0; } return gv_dk_var; }
static void gv_dproc(GvD& D, const VtxC& C, const uint32_t* adr, VtxGL* ov, GvX* ox) {
    const float eps = 1e-4f;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; bool behind = false;
    float scr[7];
    for (uint32_t j = 0; j < D.nu; j++) {
        uint32_t d = adr[j];
        Vert v = read_vert(D.fm, d, scr, D.bn, &D.amb);
        float c4[4], uv[2], vp[4]; uint32_t col, fn;
        to_clip_core(C, v, c4, &col, uv, vp, &fn, D.itw, D.ith, &D.X);
        VtxGL& o = ov[j];
        if (c4[3] > eps) {
            VP p = to_screen_c(C, c4, col, uv, true);
            float w = (p.iw > 1e-20f || p.iw < -1e-20f) ? 1.0f / p.iw : 1.0f;
            o.x = (2.0f * p.x / D.sw - 1.0f) * w; o.y = (2.0f * p.y / D.sh - 1.0f) * w; o.z = (2.0f * (p.z / 65535.0f) - 1.0f) * w; o.w = w;
            o.u = (p.u * w) / D.tw; o.v = (p.v * w) / D.th;
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        } else {
            behind = true;
            o.x = (2.0f * D.B[0] / D.sw - 1.0f) * c4[3] + 2.0f * D.A[0] / D.sw * c4[0];
            o.y = (2.0f * D.B[1] / D.sh - 1.0f) * c4[3] + 2.0f * D.A[1] / D.sh * c4[1];
            o.z = (2.0f * D.B[2] / 65535.0f - 1.0f) * c4[3] + 2.0f * D.A[2] / 65535.0f * c4[2];
            o.w = c4[3]; o.u = uv[0] / D.tw; o.v = uv[1] / D.th;
        }
        o.col = col; o.tl = -9.0f; o.tf = 0.0f; o.tint = D.tt ? D.tint : 0; o.ext = 0; o.tp = D.tt ? D.tp : 0; o.wd = -1e4f; o.wc = o.wet = o.wpad = 0.0f; o.nrm = o.lgt = 0;
        o.px = vp[0]; o.py = vp[1]; o.pz = vp[2]; o.pf = 0.0f;
        GvX x = {0u, D.tt ? fn : 0u};
        if (D.tt && D.fm.pos) {
            int nb = 3 * (D.fm.pos == 3 ? 4 : D.fm.pos);
            uint32_t h = 2166136261u ^ (D.epoch * 0x9E3779B9u);
            for (int i = 0; i < nb; i++) h = (h ^ psp_l8(d + D.fm.o_pos + i)) * 16777619u;
            x.key = h ? h : 1;
        }
        ox[j] = x;
    }
    D.x0 = x0; D.x1 = x1; D.y0 = y0; D.y1 = y1; D.behind = behind;
    if (!D.ag) return;
    thread_local float* sxy = nullptr; thread_local uint32_t cap = 0;
    if (cap < D.nu) { cap = D.nu + 1024; sxy = new float[(size_t)cap * 2]; }
    GvAg& g = D.g;
    g.b[0] = g.b[1] = 1e30f; g.b[2] = g.b[3] = -1e30f; g.big = false; g.z0 = 1e30f; g.z1 = 0; for (double& v : g.s) v = 0;
    for (uint32_t j = 0; j < D.nu; j++) {
        const VtxGL& o = ov[j];
        float z = std::fabs(o.pz); g.z0 = std::min(g.z0, z); g.z1 = std::max(g.z1, z);
        if (o.w > 1e-4f) {
            float x = (o.x / o.w + 1.0f) * 0.5f * D.fw, y = (o.y / o.w + 1.0f) * 0.5f * D.fh;
            sxy[2 * j] = x; sxy[2 * j + 1] = y;
            g.b[0] = std::min(g.b[0], x); g.b[1] = std::min(g.b[1], y); g.b[2] = std::max(g.b[2], x); g.b[3] = std::max(g.b[3], y);
        } else { g.big = true; sxy[2 * j] = NAN; }
    }
    for (uint32_t k = 0; k < D.ni; k++) {
        uint32_t j = D.ix[k] - D.base;
        if (j >= D.nu || std::isnan(sxy[2 * j])) continue;
        float x = sxy[2 * j], y = sxy[2 * j + 1];
        g.s[0] += 1; g.s[1] += x; g.s[2] += y; g.s[3] += (double)x * x; g.s[4] += (double)y * y;
    }
    g.tn = D.tno != nullptr;
    if (!g.tn) return;
    for (uint32_t q = 0; q + 2 < D.ni; q += 3) {   // same arithmetic as toon_normals_gv
        const float *a = &ov[D.ix[q] - D.base].px, *b = &ov[D.ix[q + 1] - D.base].px, *c = &ov[D.ix[q + 2] - D.base].px;
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        float cx = a[0] + b[0] + c[0], cy = a[1] + b[1] + c[1], cz = a[2] + b[2] + c[2];
        if (nx * cx + ny * cy + nz * cz > 0) { nx = -nx; ny = -ny; nz = -nz; }
        float* T = D.tno + (size_t)q * 2;
        T[0] = nx; T[1] = ny; T[2] = nz;
        float nn = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nn > 1e-20f) {
            const float* P[3] = {a, b, c};
            for (int k = 0; k < 3; k++) {
                const float *o = P[k], *p1 = P[(k + 1) % 3], *p2 = P[(k + 2) % 3];
                float e1[3] = {p1[0] - o[0], p1[1] - o[1], p1[2] - o[2]}, e2[3] = {p2[0] - o[0], p2[1] - o[1], p2[2] - o[2]};
                float l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
                float cs = (l1 > 0 && l2 > 0) ? (e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2]) / std::sqrt(l1 * l2) : 1.0f;
                T[3 + k] = std::acos(std::min(std::max(cs, -1.0f), 1.0f)) / nn;
            }
        } else T[3] = T[4] = T[5] = NAN;
    }
}
static int gv_take() {
    uint32_t i = gv_clm.load(std::memory_order_relaxed);
    if (i >= gv_pub.load(std::memory_order_acquire)) return 0;
    if (!gv_clm.compare_exchange_weak(i, i + 1)) return 1;
    GvD& D = gv_dq[i];
    gv_dproc(D, gv_dc[D.ci], gv_da + D.a0, D.ov, D.ox);
    gv_dn.fetch_add(1, std::memory_order_release);
    return 2;
}
static void gv_aworker() {
    uint64_t t0 = perf_clock();
    for (;;) {
        while (gv_take()) t0 = perf_clock();
        if (perf_clock() - t0 < 150000) { _mm_pause(); continue; }
        std::unique_lock<std::mutex> lk(*gv_am);
        gv_slp++;
        gv_acv->wait(lk, [] { return gv_clm.load() < gv_pub.load(); });
        gv_slp--;
        t0 = perf_clock();
    }
}
static bool gv_ainit() {
    if (gv_aw >= 0) return gv_aw > 0;
    unsigned hc = std::thread::hardware_concurrency();
    gv_aw = hc >= 8 ? 3 : hc >= 4 ? 2 : hc >= 2 ? 1 : 0;
    const char* e = getenv("TTT_GPU_DEFER_N");
    if (e && *e) gv_aw = std::max(0, std::min(7, atoi(e)));
    printf("vtx: vertices diferidos: hardware_concurrency %u, %d hilos de calculo\n", hc, gv_aw); fflush(stdout);
    if (gv_aw <= 0) return false;
    ge_fast2(); through_z_u16(); zbias(); ge_light_att(); tmap_variant(); scale_int(); lvf_on();
    { SProf warm(0); }
    gv_am = new std::mutex(); gv_acv = new std::condition_variable();
    gv_dq = new GvD[GVD_N]; gv_dc = new VtxC[GVC_N]; gv_da = new uint32_t[GVA_N];
    gv_vtx.reserve(1u << 17); gv_x.reserve(1u << 17); gv_idx.reserve(1u << 19); gv_tn_fit();
    for (int i = 0; i < gv_aw; i++) std::thread(gv_aworker).detach();
    return true;
}
static bool gv_pnl_on() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_GPU_PANEL"); v = e && *e ? atoi(e) : 1; } return v > 0; }
static bool gv_ag_on() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_GPU_AGG"); v = e && *e ? atoi(e) : 1; } return v > 0; }
static bool gv_df_ok() { return gv_df_on() && !gv_mt_on() && !skc_on() && (R[0xC0] & 3) != 2 && gv_ainit(); }
static bool gs_on();
static uint64_t gc_st[2][8];   // per mode: prims, screen cycles, reserve cycles, queue cycles, reused, differences, wakes, join cycles
static int gc_mode();
static void gv_defer_run() {
    if (!gv_dqn) return;
    PerfZone pz_(PZ_GE);
    static int gs_id = perf_sub_id("prim: vertices diferidos");
    struct GsT { uint64_t t = gs_on() ? perf_clock() : 0; ~GsT() { if (t) perf_sub_add(gs_id, perf_clock() - t); } } gs_t_;
    uint64_t t0 = perf_clock();
    uint32_t mine = 0;
    while (gv_clm.load() < gv_dqn) if (gv_take() == 2) mine++;
    for (int sp = 0; gv_dn.load(std::memory_order_acquire) < gv_dqn; sp++) { if (sp < 4000) _mm_pause(); else std::this_thread::yield(); }
    uint64_t t1 = perf_clock();
    gc_st[gc_mode()][7] += t1 - t0;
    uint32_t tot = 0;
    for (uint32_t i = 0; i < gv_dqn; i++) {
        GvD& D = gv_dq[i];
        tot += D.nu;
        if (D.x0 > D.x1 || (!D.behind && (std::max((int)std::floor(D.x0), D.bx0) > std::min((int)std::ceil(D.x1), D.bx1) || std::max((int)std::floor(D.y0), D.by0) > std::min((int)std::ceil(D.y1), D.by1)))) {
            queue[D.qi].x1 = 0; queue[D.qi].y1 = 0; gv_n[4]++; continue;
        }
        gv_n[2] += D.nu; gv_n[3] += (uint64_t)queue[D.qi].y1 / 3;
        if (D.ag) { queue[D.qi].ag = (int)gv_agv.size(); gv_agv.push_back(D.g); }
    }
    gv_df_n[0] += gv_dqn; gv_df_n[1] += tot; gv_df_n[2]++; gv_df_n[5] += mine;
    gv_pub.store(0); gv_clm.store(0); gv_dn.store(0);
    gv_dqn = gv_dcn = gv_dan = 0; gv_wake = 0;
    gv_mt_active.store(0);
    gv_df_t[0] += t1 - t0; gv_df_t[1] += perf_clock() - t1;
}

// TTT_PICO_GE=1: parts of a GPU-path primitive in the pico lines
static bool gs_on() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_PICO_GE"); v = e && *e ? atoi(e) : 0; } return v > 0; }
struct GSub { int id; uint64_t t; explicit GSub(int i) : id(i), t(gs_on() ? perf_clock() : 0) {} ~GSub() { if (t) perf_sub_add(id, perf_clock() - t); } };
#define GSUB(name) static int GSUB_C(_gsid_, __LINE__) = perf_sub_id(name); GSub GSUB_C(_gs_, __LINE__)(GSUB_C(_gsid_, __LINE__))
#define GSUB_C2(a, b) a##b
#define GSUB_C(a, b) GSUB_C2(a, b)
// texture state of a GPU prim reused from the previous prim while nothing it reads changed (TTT_TEXSTATE_FAST: 1 on, 0 off, 2 alternates
// every 10 s and reports both; TTT_TEXSTATE_CHECK=1 also builds it again and counts differences). Texture memory may change between
// GE runs, block transfers and palette loads: those bump the epoch
static uint32_t tsm_epoch = 1;
void psp_gpu_tex_memo_bump() { tsm_epoch++; }
static uint64_t tsm_st[2][4];   // per mode: prims, cycles, reused, differences
static int tsm_mode() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("TTT_TEXSTATE_FAST"); v = e && *e ? atoi(e) : 1; }
    if (v != 2) return v ? 1 : 0;
    static uint32_t n = 0; static int m = 0; static uint64_t t0 = 0, rep = 0;
    if ((n++ & 255) == 0) {
        uint64_t t = box_ms();
        if (!t0) t0 = rep = t;
        m = (int)(((t - t0) / 10000) & 1);
        if (t >= rep + 20000) {
            rep = t;
            for (int k = 0; k < 2; k++) {
                double p = tsm_st[k][0] ? (double)tsm_st[k][0] : 1.0;
                printf("estado: %s | %.0f ciclos por prim, %.0f%% reusados | diferencias %llu\n", k ? "rapido" : "anterior", tsm_st[k][1] / p, 100.0 * tsm_st[k][2] / p, (unsigned long long)tsm_st[k][3]);
            }
            fflush(stdout);
        }
    }
    return m;
}
static void prepare_tex_state_gv(TexState& e, bool texture) {
    struct In { uint32_t r1[13], r2[112]; int g[24]; uint32_t ep; };
    static In last; static TexState last_e; static bool have = false; static uint64_t last_key = 0;
    int m = tsm_mode();
    uint64_t c0 = perf_clock();
    tsm_st[m][0]++;
    In in;
    memset(&in, 0, sizeof(in));
    if (m) {
        memcpy(in.r1, &R[0x1C], sizeof(in.r1));
        memcpy(in.r2, &R[0x90], sizeof(in.r2));
        int g[24] = {texture, split_clip, sp_x0, sp_x1, sp_y0, sp_y1, pnl_on, pnl_hide, pnl_x0, pnl_x1, pnl_y0, pnl_y1, cap_on, dual_side, (int)clut_bytes, prim_mat, prim_blink,
                     toon_dmg1, toon_who, toon_only_nrm, (int)ly_tex_force, (int)mat_live_gen, scale_int(), (int)psp_tex_force};
        memcpy(in.g, g, sizeof(g));
        in.ep = tsm_epoch;
        if (have && !dual_side && !memcmp(&in, &last, sizeof(in))) {
            const Chain* ch = last_e.chain;
            int i = ch ? (int)(ch - cache) : -1;
            if (!ch || (i >= 0 && i < N_CACHE && cache[i].nn && cache[i].key == last_key)) {
                memcpy((void*)&e, (const void*)&last_e, sizeof(e));
                if (ch) { cache_viva[i] = 1; cache[i].stamp = ++cache_clock; e.gltex = ch->gl; }   // the async upload may have finished since
                tsm_st[1][2]++;
                static int chk = -1;
                if (chk < 0) { const char* x = getenv("TTT_TEXSTATE_CHECK"); chk = x && *x == '1'; }
                tsm_st[m][1] += perf_clock() - c0;
                if (chk) {
                    TexState f;
                    prepare_tex_state(f, texture, false, true);
                    if (memcmp(&f, &e, sizeof(f))) {
                        tsm_st[1][3]++;
                        static int logs = 0;
                        if (logs++ < 10) { printf("estado: diferencia con textura 0x%08X %dx%d fmt %d\n", f.t.dir, f.t.w, f.t.h, f.t.fmt); fflush(stdout); }
                    }
                }
                return;
            }
        }
    }
    prepare_tex_state(e, texture, false, true);
    if (m) {
        bool vram = texture && (is_vram(e.t.dir) || is_vram_high(e.t.dir) || is_twin(e.t.dir));
        have = !vram && !dual_side;
        last = in; memcpy((void*)&last_e, (const void*)&e, sizeof(e)); last_key = e.chain ? e.chain->key : 0;
    }
    tsm_st[m][1] += perf_clock() - c0;
}
static int gv_why_last = 8;
// index and address lists of a GPU prim through host pointers and direct writes (TTT_GV_FASTQ: 1 on, 0 the old one, 2 alternates every
// 10 s and reports both; TTT_GV_FASTQ_CHECK=1 also builds the old lists and counts differences)
static uint64_t gq_st[2][4];   // per mode: prims, cycles, indices, differences
static int gq_mode() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("TTT_GV_FASTQ"); v = e && *e ? atoi(e) : 1; }
    if (v != 2) return v ? 1 : 0;
    static uint32_t n = 0; static int m = 0; static uint64_t t0 = 0, rep = 0;
    if ((n++ & 255) == 0) {
        uint64_t t = box_ms();
        if (!t0) t0 = rep = t;
        m = (int)(((t - t0) / 10000) & 1);
        if (t >= rep + 20000) {
            rep = t;
            for (int k = 0; k < 2; k++) {
                double p = gq_st[k][0] ? (double)gq_st[k][0] : 1.0;
                printf("listas: %s | %.0f ciclos por prim, %.0f indices por prim | diferencias %llu\n", k ? "rapida" : "anterior", gq_st[k][1] / p, gq_st[k][2] / p, (unsigned long long)gq_st[k][3]);
            }
            fflush(stdout);
        }
    }
    return m;
}
// per-prim screen constants and clip box reused while their inputs hold, vertex arrays grown without zeroing, workers woken every 64
// prims instead of 16 (TTT_GV_CONST: 1 on, 0 the old one, 2 alternates every 10 s and reports both; TTT_GV_CONST_CHECK=1 counts differences)
static int gc_mode() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("TTT_GV_CONST"); v = e && *e ? atoi(e) : 1; }
    if (v != 2) return v ? 1 : 0;
    static uint32_t n = 0; static int m = 0; static uint64_t t0 = 0, rep = 0;
    if ((n++ & 255) == 0) {
        uint64_t t = box_ms();
        if (!t0) t0 = rep = t;
        m = (int)(((t - t0) / 10000) & 1);
        if (t >= rep + 20000) {
            rep = t;
            for (int k = 0; k < 2; k++) {
                double p = gc_st[k][0] ? (double)gc_st[k][0] : 1.0;
                printf("constantes: %s | ciclos por prim: pantalla %.0f, reserva %.0f, encolar %.0f, union %.0f | %.0f%% reusadas, %.1f avisos por 1000 prims | diferencias %llu\n", k ? "rapida" : "anterior",
                       gc_st[k][1] / p, gc_st[k][2] / p, gc_st[k][3] / p, gc_st[k][7] / p, 100.0 * gc_st[k][4] / p, 1000.0 * gc_st[k][6] / p, (unsigned long long)gc_st[k][5]);
            }
            fflush(stdout);
        }
    }
    return m;
}
struct GcKey { uint64_t gen; uint32_t d4, d5; int ke, spc, sx0, sx1, sy0, sy1, cap; };
static void gv_consts(float* A, float* B, int& x0, int& y0, int& x1, int& y1, int m) {
    static GcKey last; static float lA[3], lB[3]; static int lb[4]; static bool have = false;
    static int chk = -1;
    if (chk < 0) { const char* e = getenv("TTT_GV_CONST_CHECK"); chk = e && *e == '1'; }
    bool ok = m && !pnl_on && !cap_zoom && !rm_zoom;
    GcKey k;
    memset(&k, 0, sizeof(k));
    if (ok) {
        k.gen = tc_key(); k.d4 = R[0xD4]; k.d5 = R[0xD5]; k.ke = scale_int(); k.spc = split_clip; k.sx0 = sp_x0; k.sx1 = sp_x1; k.sy0 = sp_y0; k.sy1 = sp_y1; k.cap = cap_on;
        if (have && !memcmp(&k, &last, sizeof(k))) {
            memcpy(A, lA, sizeof(lA)); memcpy(B, lB, sizeof(lB)); x0 = lb[0]; y0 = lb[1]; x1 = lb[2]; y1 = lb[3];
            gc_st[1][4]++;
            if (!chk) return;
        }
    }
    const float z0[4] = {0, 0, 0, 1}, z1[4] = {1, 1, 1, 1}, u0[2] = {0, 0};
    VP a = to_screen(z0, 0, u0), b = to_screen(z1, 0, u0);
    float nA[3] = {b.x - a.x, b.y - a.y, b.z - a.z}, nB[3] = {a.x, a.y, a.z};
    int nb[4]; clip_box(nb[0], nb[1], nb[2], nb[3]);
    if (ok && have && !memcmp(&k, &last, sizeof(k))) {
        if (memcmp(nA, A, sizeof(nA)) || memcmp(nB, B, sizeof(nB)) || nb[0] != x0 || nb[1] != y0 || nb[2] != x1 || nb[3] != y1) {
            gc_st[1][5]++;
            static int logs = 0;
            if (logs++ < 10) { printf("constantes: diferencia (escala %.3f %.3f, caja %d %d %d %d | %d %d %d %d)\n", A[0], nA[0], x0, y0, x1, y1, nb[0], nb[1], nb[2], nb[3]); fflush(stdout); }
        }
        return;
    }
    memcpy(A, nA, sizeof(nA)); memcpy(B, nB, sizeof(nB)); x0 = nb[0]; y0 = nb[1]; x1 = nb[2]; y1 = nb[3];
    if (ok) { last = k; memcpy(lA, nA, sizeof(lA)); memcpy(lB, nB, sizeof(lB)); memcpy(lb, nb, sizeof(lb)); have = true; }
}
static bool gq_chk() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_GV_FASTQ_CHECK"); v = e && *e == '1'; } return v > 0; }
static bool gv_prim(const Fmt& fm, int type, int n, bool texture) {
    gv_why_last = 8;
#ifdef _WIN32
    gv_n[0]++;
    if (!gv_on()) return false;
    static int gskin = -1;
    if (gskin < 0) { const char* e = getenv("TTT_GPU_SKIN"); gskin = e && *e ? atoi(e) : 1; }
    int why = (fm.wt && !gskin) ? 0 : (R[0x1D] & 1) ? 1 : !(R[0x50] & 1) ? 2 : ((toon_now || fig_now) && !gskin) ? 3 : sm_rec ? 4 : ((psp_map_water && !gv_water_on()) || (surf_now && !gv_surf_on())) ? 5 : ((pnl_on && (cap_face_prim || !gv_pnl_on())) || cap_on || cap_zoom || rm_zoom) ? 6 :
              (fm.through || n < 3 || !glr_enabled() || !defer() || (R[0xD3] & 1) || in_2d || tint() || watch_tex() || batch_cap()) ? 7 : -1;
    if (why >= 0) { gv_old[why] += (uint64_t)n; gv_why_last = why; return false; }
    static int cull = -1;
    if (cull < 0) { const char* e = getenv("TTT_GPU_CULL"); cull = e && *e ? atoi(e) : 1; }
    uint64_t pc0 = perf_clock();
    bool outside;
    { GSUB("prim: caja fuera"); outside = cull && fm.pos && !fm.wt && prim_outside(fm, n); }
    if (outside) { gv_n[5]++; gv_p[0] += perf_clock() - pc0; return true; }   // whole box outside the view: no decode
    uint64_t pc1 = perf_clock(); gv_p[0] += pc1 - pc0;
    TexState e;
    { GSUB("prim: estado de textura"); prepare_tex_state_gv(e, texture); }
    uint64_t ps0 = cp_on() ? perf_clock() : 0;
    if (ps0) cp_st[0] += ps0 - pc1;
    bool in_vram = (e.t.dir >= 0x04000000u && e.t.dir < 0x04200000u) || (patch_rt_texture.val && is_vram_high(e.t.dir)) || is_twin(e.t.dir);
    if (texture && !in_vram && e.t.levels <= 1 && e.t.w >= 8 && e.t.h >= 8 && !(patch_ge_fast.val && e.chain)) { GSUB("prim: cadena de textura"); e.chain = chain_of(e.t); }
    gv_n[1]++;
    if (texture && skip_draw(e.t.dir)) { gv_p[1] += perf_clock() - pc1; return true; }
    if (texture && in_vram && hits_target(e.t)) psp_gpu_flush_by(PC_VVRAM);
    uint64_t ps1 = ps0 ? perf_clock() : 0;
    if (ps0) cp_st[1] += ps1 - ps0;
    static uint32_t st[65536], sl[65536], stamp = 0;
    static std::vector<uint32_t> loc;
    if (++stamp == 0) { memset(st, 0, sizeof(st)); stamp = 1; }
    if ((int)loc.size() < n) loc.resize(n);
    float sw = (float)e.f.width, sh = 512.0f * (float)scale_int();
    float tw = texture && e.gltex_w ? (float)e.gltex_w : 1.0f, th = texture && e.gltex_h ? (float)e.gltex_h : 1.0f;
    const float eps = 1e-4f;
    bool aff = false, behind = false; float A[3], B[3];
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    uint32_t base = (uint32_t)gv_vtx.size();
    bool tt = toon_now && texture, tf_ = fig_now && texture;
    const bool cpe = cp_on();
    uint64_t* cpm = cp_em[fm.wt ? 1 : 0];
    auto emit = [&](uint32_t d) {
        uint64_t e0 = cpe ? perf_clock() : 0;
        Vert v = read_vert(fm, d);
        uint64_t e1 = cpe ? perf_clock() : 0;
        float c[4], uv[2]; uint32_t col;
        to_clip(v, c, &col, uv);
        uint64_t e2 = cpe ? perf_clock() : 0;
        VtxGL o;
        if (c[3] > eps) {
            VP p = to_screen(c, col, uv);
            float w = (p.iw > 1e-20f || p.iw < -1e-20f) ? 1.0f / p.iw : 1.0f;
            o.x = (2.0f * p.x / sw - 1.0f) * w; o.y = (2.0f * p.y / sh - 1.0f) * w; o.z = (2.0f * (p.z / 65535.0f) - 1.0f) * w; o.w = w;
            o.u = (p.u * w) / tw; o.v = (p.v * w) / th;
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        } else {
            if (!aff) {
                const float z0[4] = {0, 0, 0, 1}, z1[4] = {1, 1, 1, 1}, u0[2] = {0, 0};
                VP a = to_screen(z0, 0, u0), b = to_screen(z1, 0, u0);
                A[0] = b.x - a.x; A[1] = b.y - a.y; A[2] = b.z - a.z; B[0] = a.x; B[1] = a.y; B[2] = a.z; aff = true;
            }
            behind = true;
            o.x = (2.0f * B[0] / sw - 1.0f) * c[3] + 2.0f * A[0] / sw * c[0];
            o.y = (2.0f * B[1] / sh - 1.0f) * c[3] + 2.0f * A[1] / sh * c[1];
            o.z = (2.0f * B[2] / 65535.0f - 1.0f) * c[3] + 2.0f * A[2] / 65535.0f * c[2];
            o.w = c[3]; o.u = uv[0] / tw; o.v = uv[1] / th;
        }
        o.col = col; o.tl = -9.0f; o.tf = 0.0f; o.tint = tt ? toon_tint : 0; o.ext = 0; o.tp = tt ? toon_tp : 0; o.wd = -1e4f; o.wc = o.wet = o.wpad = 0.0f; o.nrm = o.lgt = 0;
        o.px = tc_vp[0]; o.py = tc_vp[1]; o.pz = tc_vp[2]; o.pf = 0.0f;
        gv_vtx.push_back(o);
        uint64_t e3 = cpe ? perf_clock() : 0;
        GvX x = {0u, tt ? tc_fn : 0u};
        if (tt && fm.pos) {   // vertex identity as read_clip: raw model position, per fighter draw
            int nb = 3 * (fm.pos == 3 ? 4 : fm.pos);
            uint32_t h = 2166136261u ^ (toon_epoch * 0x9E3779B9u);
            for (int i = 0; i < nb; i++) h = (h ^ psp_l8(d + fm.o_pos + i)) * 16777619u;
            x.key = h ? h : 1;
        }
        gv_x.push_back(x);
        if (cpe) { uint64_t e4 = perf_clock(); cpm[0] += e1 - e0; cpm[1] += e2 - e1; cpm[2] += e3 - e2; cpm[3] += e4 - e3; cpm[4]++; }
    };
    static std::vector<uint32_t> uadr;
    const int qm = gq_mode();
    uint64_t q0 = perf_clock(), qc = 0;
    gq_st[qm][0]++;
    uadr.clear();
    if (qm) {
        uadr.resize((size_t)n);
        uint32_t* ua = uadr.data(); uint32_t m = 0;
        if (!fm.idx) { for (int i = 0; i < n; i++) { ua[i] = v_addr + (uint32_t)i * fm.stride; loc[i] = base + (uint32_t)i; } m = (uint32_t)n; }
        else {
            const uint8_t* ib = psp_p(i_addr);
            for (int i = 0; i < n; i++) {
                uint32_t k; if (fm.idx == 1) k = ib[i]; else { uint16_t h; memcpy(&h, ib + 2 * i, 2); k = h; }
                if (st[k] != stamp) { st[k] = stamp; sl[k] = base + m; ua[m++] = v_addr + k * fm.stride; }
                loc[i] = sl[k];
            }
        }
        uadr.resize(m);
    } else if (!fm.idx) { for (int i = 0; i < n; i++) { uadr.push_back(v_addr + i * fm.stride); loc[i] = base + (uint32_t)i; } }
    else for (int i = 0; i < n; i++) {
        uint32_t k = fm.idx == 1 ? psp_l8(i_addr + i) : psp_l16(i_addr + i * 2);
        if (st[k] != stamp) { st[k] = stamp; sl[k] = base + (uint32_t)uadr.size(); uadr.push_back(v_addr + k * fm.stride); }
        loc[i] = sl[k];
    }
    qc += perf_clock() - q0;
    if (qm && gq_chk()) {
        static std::vector<uint32_t> ka, kl; static uint32_t kst[65536], ksl[65536], kstamp = 0;
        if (++kstamp == 0) { memset(kst, 0, sizeof(kst)); kstamp = 1; }
        ka.clear(); kl.resize((size_t)n);
        for (int i = 0; i < n; i++) {
            if (!fm.idx) { ka.push_back(v_addr + i * fm.stride); kl[i] = base + (uint32_t)i; continue; }
            uint32_t k = fm.idx == 1 ? psp_l8(i_addr + i) : psp_l16(i_addr + i * 2);
            if (kst[k] != kstamp) { kst[k] = kstamp; ksl[k] = base + (uint32_t)ka.size(); ka.push_back(v_addr + k * fm.stride); }
            kl[i] = ksl[k];
        }
        if (ka != uadr || memcmp(kl.data(), loc.data(), (size_t)n * 4)) {
            gq_st[1][3]++;
            static int logs = 0;
            if (logs++ < 10) { printf("listas: diferencia en direcciones (%d indices, tipo %d, formato de indice %d)\n", n, fm.idx, type); fflush(stdout); }
        }
    }
    uint32_t nu = (uint32_t)uadr.size();
    uint64_t pc2 = perf_clock(); gv_p[1] += pc2 - pc1;
    uint64_t qlast = pc2;
    auto qt = [&](int k) { if (cpe) { uint64_t t = perf_clock(); cp_vq[k] += t - qlast; qlast = t; } };
    if (ps0) { cp_st[2] += pc2 - ps1; cp_st[3] += n; }
    bool dfr = false, dchk = false; GvD* DP = nullptr; static GvD dk_d;
    const int gm = gc_mode();
    if (gv_df_ok()) {
        dfr = true;
        if (gv_dk() > 0 && ++gv_dk_cnt >= gv_dk()) { gv_dk_cnt = 0; dchk = true; }
        if (!dchk && (gv_dqn >= GVD_N || gv_dcn >= GVC_N || gv_dan + nu > GVA_N || gv_vtx.size() + nu > gv_vtx.capacity() || gv_x.size() + nu > gv_x.capacity() || gv_idx.size() + 3u * (uint32_t)n > gv_idx.capacity())) {
            gv_defer_run(); if (cpe) cp_vq[10]++;
            if (gv_idx.size() + 3u * (uint32_t)n > gv_idx.capacity()) { gv_idx.reserve(gv_idx.capacity() * 2 + 3u * (uint32_t)n); gv_tn_fit(); }
            if (gv_vtx.size() + nu > gv_vtx.capacity()) gv_vtx.reserve(gv_vtx.capacity() * 2 + nu);
            if (gv_x.size() + nu > gv_x.capacity()) gv_x.reserve(gv_x.capacity() * 2 + nu);
            if (gv_dan + nu > GVA_N) { dfr = false; gv_df_n[6]++; }
        }
    }
    qt(0);
    uint64_t vg0 = perf_clock(); const int vgm = vg_mode();
    if (dfr) {
        GvD& D = dchk ? dk_d : gv_dq[gv_dqn];
        DP = &D;
        const VtxC& C = vtx_cur();
        uint32_t* dap = gv_da + gv_dan;
        if (dchk) { gv_kc = C; D.ci = 0; dap = nullptr; }
        else {
            if (!gv_dcn || gv_dc[gv_dcn - 1].gen != C.gen) gv_dc[gv_dcn++] = C;
            D.ci = gv_dcn - 1;
            memcpy(dap, uadr.data(), nu * sizeof(uint32_t));
        }
        D.fm = fm;
        memcpy(D.X.W, m_world, sizeof(D.X.W)); memcpy(D.X.V, m_view, sizeof(D.X.V)); memcpy(D.X.P, m_proj, sizeof(D.X.P));
        if (C.tmap == 1) memcpy(D.X.T, m_tgen, sizeof(D.X.T));
        D.X.ax = asp_x; D.X.ay = asp_y; D.X.ssx = sp_sx; D.X.ssy = sp_sy; D.X.sox = sp_ox; D.X.soy = sp_oy; D.X.split = split_now; D.X.toon = toon_now;
        D.X.pnl = pnl_on; D.X.ps = pnl_s; D.X.pox = pnl_ox; D.X.pdy = pnl_dy; D.X.pys = (R[0x43] >> 23) & 1 ? 1.0f : -1.0f;
        if (fm.wt) memcpy(D.bn, m_bone, sizeof(float) * 12 * std::min(std::max(fm.nw, 0), 8));
        D.amb = (R[0x55] & 0xFFFFFFu) | ((R[0x58] & 0xFFu) << 24);
        D.a0 = dchk ? 0 : gv_dan; D.nu = nu;
        D.sw = sw; D.sh = sh; D.tw = tw; D.th = th;
        qt(1);
        if (ge_fast2()) { tc_tex_upd(); D.itw = tc_tw; D.ith = tc_th; }
        else { Tex tx = tex(); D.itw = tx.w; D.ith = tx.h; }
        D.tt = tt; D.tint = toon_tint; D.tp = toon_tp; D.epoch = toon_epoch; D.qi = -1; D.ag = false; D.tno = nullptr;
        uint64_t gc0 = perf_clock();
        gv_consts(D.A, D.B, D.bx0, D.by0, D.bx1, D.by1, gm);
        uint64_t gc1 = perf_clock(); gc_st[gm][1] += gc1 - gc0;
        vg_st[vgm][3]++; vg_st[vgm][4] += gc1 - vg0;
        qt(2);
        if (!dchk) {
            gv_dan += nu;
            gp_n[0] += nu; gp_n[3] += nu;
            gv_noinit = gm != 0;
            gv_vtx.resize(base + nu); gv_x.resize(base + nu);
            gv_noinit = false;
            gc_st[gm][2] += perf_clock() - gc1;
            D.ov = gv_vtx.data() + base; D.ox = gv_x.data() + base;
            behind = true;
        }
    }
    qt(3);
    bool dnow = dfr && !dchk;
    if (!dnow && gv_dqn && (gv_vtx.size() + nu > gv_vtx.capacity() || gv_x.size() + nu > gv_x.capacity() || gv_idx.size() + 3u * (uint32_t)n > gv_idx.capacity())) gv_defer_run();
    bool par = false;
    if (!dfr && gv_mt_on() && gv_mt_init() && gv_mt_ready > 0 && (int)nu >= gv_mt_minv() && (R[0xC0] & 3) != 2) par = true;
    if (dnow) {
    } else if (par) {
        GvJob& J = gv_job;
        static std::vector<float> skb;
        J.C = vtx_cur(); J.fm = fm; J.adr = uadr.data(); J.nu = (int)nu;
        gv_vtx.resize(base + nu); gv_x.resize(base + nu);
        J.ov = gv_vtx.data() + base; J.ox = gv_x.data() + base;
        J.sk = nullptr;
        if (fm.wt && skin_key) { skb.resize((size_t)nu * 7); J.sk = skb.data(); }
        J.sw = sw; J.sh = sh; J.tw = tw; J.th = th; J.tt = tt; J.tint = toon_tint; J.tp = toon_tp; J.epoch = toon_epoch;
        if (ge_fast2()) {
            tc_tex_upd();
            J.itw = tc_tw; J.ith = tc_th;
        } else { Tex tx = tex(); J.itw = tx.w; J.ith = tx.h; }
        {
            const float z0[4] = {0, 0, 0, 1}, z1[4] = {1, 1, 1, 1}, u0[2] = {0, 0};
            VP a = to_screen(z0, 0, u0), b = to_screen(z1, 0, u0);
            J.A[0] = b.x - a.x; J.A[1] = b.y - a.y; J.A[2] = b.z - a.z; J.B[0] = a.x; J.B[1] = a.y; J.B[2] = a.z;
        }
        int nch = std::min(std::min(GV_MAXCH, 2 * gv_mt_thr), std::max(2, (int)nu / 16));
        gv_job.nch = nch; gv_run_fn = gv_chunk;
        gv_mt_dispatch(nch);
        gv_mt_n[0]++; gv_mt_n[1] += nu;
        gp_n[0] += nu; gp_n[3] += nu;
        for (int c = 0; c < nch; c++) {
            const GvJob::Part& P = J.part[c];
            x0 = std::min(x0, P.x0); x1 = std::max(x1, P.x1); y0 = std::min(y0, P.y0); y1 = std::max(y1, P.y1);
            if (P.behind) behind = true;
        }
        if (gv_mt_chk > 0) {
            std::vector<VtxGL> rv(gv_vtx.begin() + base, gv_vtx.end());
            std::vector<GvX> rx(gv_x.begin() + base, gv_x.end());
            float rb[4] = {x0, x1, y0, y1}; bool rbh = behind;
            gv_vtx.resize(base); gv_x.resize(base);
            x0 = 1e30f; x1 = -1e30f; y0 = 1e30f; y1 = -1e30f; behind = false; aff = false;
            for (uint32_t j = 0; j < nu; j++) emit(uadr[j]);
            bool bad = memcmp(rv.data(), gv_vtx.data() + base, nu * sizeof(VtxGL)) != 0 || memcmp(rx.data(), gv_x.data() + base, nu * sizeof(GvX)) != 0 ||
                       rbh != behind || (!(x0 > x1) && (rb[0] != x0 || rb[1] != x1 || rb[2] != y0 || rb[3] != y1));
            gv_mt_n[3]++;
            if (bad) {
                gv_mt_n[2]++;
                static int shown = 0;
                if (shown < 6) {
                    shown++;
                    for (uint32_t j = 0; j < nu; j++) {
                        if (memcmp(&rv[j], &gv_vtx[base + j], sizeof(VtxGL)) || memcmp(&rx[j], &gv_x[base + j], sizeof(GvX))) {
                            printf("vtx: verificacion: diferencia en vertice %u de %u (piel %d, cel %d): hilos pos %.5f %.5f %.5f %.5f | serie %.5f %.5f %.5f %.5f\n", j, nu, fm.wt, tt,
                                   rv[j].x, rv[j].y, rv[j].z, rv[j].w, gv_vtx[base + j].x, gv_vtx[base + j].y, gv_vtx[base + j].z, gv_vtx[base + j].w);
                            break;
                        }
                    }
                    fflush(stdout);
                }
            }
        } else if (J.sk) {
            for (uint32_t j = 0; j < nu; j++) {
                const float* q = J.sk + 7 * j;
                if (q[6] == 0.0f) { sk_n[1]++; continue; }
                sk_n[0]++;
                SkinSlot* ss = &skin_tab[((uadr[j] >> 2) * 2654435761u ^ (uint32_t)skin_key) & (SKIN_N - 1)];
                ss->key = skin_key; ss->d = uadr[j]; for (int k = 0; k < 6; k++) ss->p[k] = q[k];
            }
        }
    } else for (uint32_t j = 0; j < nu; j++) emit(uadr[j]);
    qt(4);
    uint64_t pc3 = perf_clock(); gv_p[2] += pc3 - pc2;
    uint32_t nv = (uint32_t)gv_vtx.size() - base;
    if (dchk) {
        GvD& D = *DP;
        static std::vector<VtxGL> kv; static std::vector<GvX> kx;
        kv.resize(nu); kx.resize(nu);
        gv_dproc(D, gv_kc, uadr.data(), kv.data(), kx.data());
        bool bad = nv != nu || memcmp(kv.data(), gv_vtx.data() + base, nu * sizeof(VtxGL)) != 0 || memcmp(kx.data(), gv_x.data() + base, nu * sizeof(GvX)) != 0 ||
                   D.behind != behind || (!(x0 > x1) && (D.x0 != x0 || D.x1 != x1 || D.y0 != y0 || D.y1 != y1));
        gv_df_n[3]++;
        if (bad) { gv_df_n[4]++; static int shown = 0; if (shown < 6) { shown++; printf("vtx: diferido: diferencia en una prim verificada (%u vertices, piel %d, cel %d)\n", nu, fm.wt, tt); fflush(stdout); } }
    }
    int bx0, by0, bx1, by1;
    clip_box(bx0, by0, bx1, by1);
    if (!dnow && (x0 > x1 || (!behind && (std::max((int)std::floor(x0), bx0) > std::min((int)std::ceil(x1), bx1) || std::max((int)std::floor(y0), by0) > std::min((int)std::ceil(y1), by1))))) {
        gv_vtx.resize(base); gv_x.resize(base); gv_n[4]++; gv_p[3] += perf_clock() - pc3; return true;
    }
    qt(5);
    uint32_t ib = (uint32_t)gv_idx.size();
    uint64_t q1 = perf_clock();
    if (qm) {
        uint32_t cnt = type == 3 ? (uint32_t)(n / 3) * 3 : n >= 3 ? 3u * (uint32_t)(n - 2) : 0u;
        gv_idx.resize(ib + cnt);
        uint32_t* o = gv_idx.data() + ib; const uint32_t* L = loc.data();
        if (type == 3) memcpy(o, L, cnt * 4);
        else if (type == 4) for (int i = 2; i < n; i++, o += 3) { o[0] = L[i - 2]; o[1] = L[i - 1]; o[2] = L[i]; }
        else for (int i = 2; i < n; i++, o += 3) { o[0] = L[0]; o[1] = L[i - 1]; o[2] = L[i]; }
    } else if (type == 3) for (int i = 0; i + 2 < n; i += 3) { gv_idx.push_back(loc[i]); gv_idx.push_back(loc[i + 1]); gv_idx.push_back(loc[i + 2]); }
    else if (type == 4) for (int i = 2; i < n; i++) { gv_idx.push_back(loc[i - 2]); gv_idx.push_back(loc[i - 1]); gv_idx.push_back(loc[i]); }
    else for (int i = 2; i < n; i++) { gv_idx.push_back(loc[0]); gv_idx.push_back(loc[i - 1]); gv_idx.push_back(loc[i]); }
    uint32_t ni = (uint32_t)gv_idx.size() - ib;
    qc += perf_clock() - q1; gq_st[qm][1] += qc; gq_st[qm][2] += ni;
    qt(6);
    if (qm && gq_chk()) {
        static std::vector<uint32_t> ki; ki.clear();
        if (type == 3) for (int i = 0; i + 2 < n; i += 3) { ki.push_back(loc[i]); ki.push_back(loc[i + 1]); ki.push_back(loc[i + 2]); }
        else if (type == 4) for (int i = 2; i < n; i++) { ki.push_back(loc[i - 2]); ki.push_back(loc[i - 1]); ki.push_back(loc[i]); }
        else for (int i = 2; i < n; i++) { ki.push_back(loc[0]); ki.push_back(loc[i - 1]); ki.push_back(loc[i]); }
        if (ki.size() != ni || (ni && memcmp(ki.data(), gv_idx.data() + ib, (size_t)ni * 4))) {
            gq_st[1][3]++;
            static int logs = 0;
            if (logs++ < 10) { printf("listas: diferencia en indices (%d vertices, tipo %d)\n", n, type); fflush(stdout); }
        }
        if (cpe) qlast = perf_clock();
    }
    if (!dnow) { gv_n[2] += nv; gv_n[3] += ni / 3; }
    Job t;
    t.cls = 2; t.col_flat = 0; t.signature = 0;
    t.toon = tt; t.fig = tf_; t.glow = tf_ ? glow_now : 0;
    memcpy(t.lv, toon_lv, sizeof(t.lv));
    t.tint = tt ? toon_tint : 0; t.grp = tt ? toon_grp : 0; t.tp = tt ? toon_tp : 0; t.who = (uint8_t)(tt || tf_ ? toon_who : 0); t.aura = (uint8_t)toon_aura; t.face = (uint8_t)(tt ? toon_face : 0);
    t.d3 = 1; t.vh = view_hash; memcpy(t.up, view_up, sizeof(t.up));
    memcpy(t.vi, view_inv, sizeof(t.vi)); memcpy(t.eye, proj_eye, sizeof(t.eye)); t.sv = (uint8_t)split_now; t.wsf = (uint8_t)surf_now; t.back = (uint8_t)ef_now;
    t.x0 = (int)base; t.x1 = (int)nv; t.y0 = (int)ib; t.y1 = (int)ni;
    t.st_tex = tex_state_index(e);
    qt(7);
    uint64_t gq7 = perf_clock();
    if (dnow) {
        gc_st[gm][0]++;
        DP->qi = (int)queue.size();
        if ((tt || tf_) && gv_ag_on()) { DP->ag = true; DP->fw = (float)e.f.width; DP->fh = (float)(512 * scale_int()); DP->ix = gv_idx.data() + ib; DP->ni = ni; DP->base = (uint32_t)base; DP->tno = tt && (ib + ni) / 3 <= gv_tn_cap ? gv_tn + (size_t)ib / 3 * 6 : nullptr; }
        queue.push_back(t);
        gv_mt_active.store(1, std::memory_order_relaxed);
        gv_dqn++;
        gv_pub.store(gv_dqn);
        if (gv_dqn - gv_wake >= 16u && gv_slp.load() > 0) { gv_wake = gv_dqn; gv_acv->notify_all(); gc_st[gm][6]++; if (cpe) cp_vq[11]++; }
        gc_st[gm][3] += perf_clock() - gq7;
    } else queue.push_back(t);
    qt(8);
    int ty0 = e.sy0, ty1 = e.sy1;
    if (!behind) { ty0 = std::max(ty0, (int)std::floor(y0)); ty1 = std::min(ty1, (int)std::ceil(y1)); }
    if (ty1 > 512 * scale_int() - 1) ty1 = 512 * scale_int() - 1;
    if (ty0 < 0) ty0 = 0;
    if (ty0 <= ty1) note_target(e, ty0, ty1);
    qt(9);
    gv_p[3] += perf_clock() - pc3;
    return true;
#else
    (void)fm; (void)type; (void)n; (void)texture;
    return false;
#endif
}

static void gv_report() {
    static int frames = 0, lines = 0;
    if (lines >= 60) return;
#ifdef _WIN32
    if (gv_rep_reset) {
        gv_rep_reset = false; frames = 0; gv_draws = 0;
        for (auto& x : gv_n) x = 0;
        for (auto& x : gv_old) x = 0;
        for (auto& x : gv_t) x = 0;
        for (auto& x : gv_mt_n) x = 0;
        for (auto& x : gv_mt_t) x = 0;
        for (auto& x : gv_p) x = 0;
        for (auto& x : sk_n) x = 0;
        memset(gv_df_n, 0, sizeof(gv_df_n)); memset(gv_df_t, 0, sizeof(gv_df_t));
        return;
    }
#endif
    if (++frames < 300) return;
#ifdef _WIN32
    double d = frames;
    printf("vtx: GPU %s | prims 3D %.0f, por GPU %.0f (fuera por caja %.0f, descartadas %.0f) | vertices %.0f, triangulos %.0f, dibujos %.0f por cuadro | vertices al camino viejo: piel %.0f, cara trasera %.0f, plano %.0f, cel %.0f, suave %.0f, agua %.0f, menus %.0f, otros %.0f\n",
           gv_on() ? "si" : "no", gv_n[0] / d, gv_n[1] / d + gv_n[5] / d, gv_n[5] / d, gv_n[4] / d, gv_n[2] / d, gv_n[3] / d, gv_draws / d,
           gv_old[0] / d, gv_old[1] / d, gv_old[2] / d, gv_old[3] / d, gv_old[4] / d, gv_old[5] / d, gv_old[6] / d, gv_old[7] / d);
    printf("vtx: M ciclos por cuadro: prims 3D con piel %.2f, sin piel %.2f | vaciado GL %.2f (cel escena %.2f, cel normales %.2f, subida %.2f, bucle de dibujos %.2f, ultimo lote %.2f)\n",
           gv_t[0] / d / 1e6, gv_t[1] / d / 1e6, gv_t[4] / d / 1e6, gv_t[2] / d / 1e6, gv_t[3] / d / 1e6, gv_t[5] / d / 1e6, gv_t[6] / d / 1e6, gv_t[7] / d / 1e6);
    printf("vtx: partes de gv_prim M ciclos por cuadro: caja %.2f, estado %.2f, vertices %.2f, cola %.2f (%.0f prims por caja, %.0f decodificadas)\n",
           gv_p[0] / d / 1e6, gv_p[1] / d / 1e6, gv_p[2] / d / 1e6, gv_p[3] / d / 1e6, gv_n[5] / d, gv_n[1] / d);
    printf("vtx: cache de piel %s: vertices con piel calculados %.0f, reusados %.0f por cuadro\n", skc_on() ? "si" : "no", sk_n[0] / d, sk_n[1] / d);
    printf("vtx: hilos modo %d minimo %d | prims en hilos %.1f, vertices %.0f por cuadro | M ciclos: reparto %.2f, espera de union %.2f | verificadas %.0f, con diferencias %.0f\n",
           gv_mt_var < 0 ? 0 : gv_mt_var, gv_mt_ready > 0 ? gv_mt_minv() : 0, gv_mt_n[0] / d, gv_mt_n[1] / d, gv_mt_t[0] / d / 1e6, gv_mt_t[1] / d / 1e6, (double)gv_mt_n[3], (double)gv_mt_n[2]);
    if (gv_df_on() || gv_df_n[2]) printf("vtx: diferido %s | prims %.0f, vertices %.0f, uniones %.1f por cuadro, %.0f%% de las prims en el hilo principal | M ciclos: espera de union %.2f, cierre %.2f | verificadas %.0f, con diferencias %.0f, sin espacio %.0f\n",
                                         gv_df_on() ? "si" : "no", gv_df_n[0] / d, gv_df_n[1] / d, gv_df_n[2] / d, gv_df_n[0] ? 100.0 * gv_df_n[5] / gv_df_n[0] : 0.0, gv_df_t[0] / d / 1e6, gv_df_t[1] / d / 1e6, (double)gv_df_n[3], (double)gv_df_n[4], (double)gv_df_n[6]);
    memset(gv_df_n, 0, sizeof(gv_df_n)); memset(gv_df_t, 0, sizeof(gv_df_t));
    if (cp_on()) {
        printf("cpu: caja M ciclos por cuadro: lectura %.2f, esquinas %.2f (%.0f vertices leidos) | bucle de dibujos por clase M (trabajos por cuadro): triangulo %.2f (%.0f), 2D %.2f (%.1f), GPU %.2f (%.1f), otro %.2f (%.1f)\n",
               cp_box[0] / d / 1e6, cp_box[1] / d / 1e6, cp_box[2] / d, cp_job[0][0] / d / 1e6, cp_job[0][1] / d, cp_job[1][0] / d / 1e6, cp_job[1][1] / d, cp_job[2][0] / d / 1e6, cp_job[2][1] / d, cp_job[3][0] / d / 1e6, cp_job[3][1] / d);
        printf("cpu: pases dentro del bucle de dibujos M ciclos por cuadro: oscuridad %.2f, efectos/atm %.2f, copia suave %.2f, delineado %.2f | triangulos sueltos 3D %.0f, 2D %.0f por cuadro\n",
               cp_tj[0] / d / 1e6, cp_tj[1] / d / 1e6, cp_tj[2] / d / 1e6, cp_tj[3] / d / 1e6, cp_tj[4] / d, cp_tj[5] / d);
        printf("cpu: sueltos 2D M ciclos por cuadro: total %.2f (%.0f trabajos), antes del estado %.2f, cambios de estado %.0f (lote previo %.2f, estado GL %.2f, resto %.2f) | tras un pase %.0f, iguales %.0f, solo textura %.0f, otros %.0f | grupos distintos: destino %.0f, recorte %.0f, mezcla %.0f, profundidad %.0f, stencil %.0f, alfa %.0f, modo %.0f, textura %.0f\n",
               cp_lj[0] / d / 1e6, cp_lj[1] / d, cp_lj[2] / d / 1e6, cp_lj[3] / d, cp_lj[8] / d / 1e6, cp_lj[9] / d / 1e6, cp_lj[10] / d / 1e6, cp_lj[4] / d, cp_lj[5] / d, cp_lj[6] / d, cp_lj[7] / d,
               cp_lg[0] / d, cp_lg[1] / d, cp_lg[2] / d, cp_lg[3] / d, cp_lg[4] / d, cp_lg[5] / d, cp_lg[6] / d, cp_lg[7] / d);
        printf("cpu: lotes GL %.0f por cuadro, %.1f vertices cada uno, M ciclos: preparar %.2f, subida %.2f, dibujo %.2f | pase efectos/atm M ciclos: brillo %.2f, atm %.2f, copia %.2f, final %.2f\n",
               cp_db[0] / d, cp_db[0] ? (double)cp_db[1] / cp_db[0] : 0.0, cp_db[2] / d / 1e6, cp_db[3] / d / 1e6, cp_db[4] / d / 1e6, cp_at[0] / d / 1e6, cp_at[1] / d / 1e6, cp_at[2] / d / 1e6, cp_at[3] / d / 1e6);
        printf("cpu: atm %.1f pases por cuadro, M ciclos: entrada %.2f, preparar %.2f, profundidad %.2f, constantes %.2f, sol %.2f, reducciones y brillo %.2f, calima %.2f, final %.2f\n",
               cp_ap[8] / d, cp_ap[0] / d / 1e6, cp_ap[1] / d / 1e6, cp_ap[2] / d / 1e6, cp_ap[3] / d / 1e6, cp_ap[4] / d / 1e6, cp_ap[5] / d / 1e6, cp_ap[6] / d / 1e6, cp_ap[7] / d / 1e6);
        int top[10]; int k = 0; double tot = 0;
        for (int i = 0; i < 256; i++) tot += (double)cp_op[i][0];
        for (int j = 0; j < 10; j++) { int b = -1; for (int i = 0; i < 256; i++) { bool u = false; for (int q = 0; q < k; q++) u |= top[q] == i; if (!u && cp_op[i][0] && (b < 0 || cp_op[i][0] > cp_op[b][0])) b = i; } if (b < 0) break; top[k++] = b; }
        for (int q = 0; q < 2; q++) {
            double nv = cp_em[q][4] ? (double)cp_em[q][4] : 1.0;
            printf("cpu: vertices %s %.0f por cuadro, ciclos por vertice: lectura %.0f, transformar y luz %.0f, pantalla y relleno %.0f, identidad %.0f\n", q ? "con piel" : "sin piel",
                   cp_em[q][4] / d, cp_em[q][0] / nv, cp_em[q][1] / nv, cp_em[q][2] / nv, cp_em[q][3] / nv);
        }
        printf("cpu: estado de gv_prim M ciclos por cuadro: textura %.2f, cadena y vaciado %.2f, indices %.2f (%.0f indices)\n", cp_st[0] / d / 1e6, cp_st[1] / d / 1e6, cp_st[2] / d / 1e6, cp_st[3] / d);
        printf("cpu: vertices de gv_prim M ciclos por cuadro: espacio %.2f (%.2f vaciados), copia %.2f, pantalla y recorte %.2f, reserva %.2f, resto %.2f | cola: recorte %.2f, indices %.2f, trabajo %.2f, encolar %.2f (%.1f avisos a los hilos), destino %.2f\n",
               cp_vq[0] / d / 1e6, cp_vq[10] / d, cp_vq[1] / d / 1e6, cp_vq[2] / d / 1e6, cp_vq[3] / d / 1e6, cp_vq[4] / d / 1e6, cp_vq[5] / d / 1e6, cp_vq[6] / d / 1e6, cp_vq[7] / d / 1e6, cp_vq[8] / d / 1e6, cp_vq[11] / d, cp_vq[9] / d / 1e6);
        printf("cpu: dibujos GPU M ciclos por cuadro (%.1f dibujos, %.1f unidos, %.1f cambios de estado): cambio de estado %.2f, caja del borde %.2f, sumas de luchador %.2f, lote previo %.2f, copia pendiente %.2f, revision ligera %.2f, area %.2f, llamada de dibujo %.2f | marcas antes de subir %.2f%s\n",
               cp_gj[7] / d, cp_gj[8] / d, cp_gj[10] / d, cp_gj[9] / d / 1e6, cp_gj[0] / d / 1e6, cp_gj[1] / d / 1e6, cp_gj[6] / d / 1e6, cp_gj[2] / d / 1e6, cp_gj[3] / d / 1e6, cp_gj[4] / d / 1e6, cp_gj[5] / d / 1e6, cp_gj[11] / d / 1e6, gv_ag_on() ? ", sumas en hilos" : "");
        printf("cpu: prim fuera de gv_prim M ciclos por cuadro (%.0f prims, %.0f por GPU): cabecera %.2f, piel y puntos %.2f, partes del luchador %.2f, camara atm %.2f, sol cel %.2f, resto %.2f | gv_prim %.2f | comando 04 total %.2f\n",
               cp_pr[8] / d, cp_pr[7] / d, cp_pr[0] / d / 1e6, cp_pr[1] / d / 1e6, cp_pr[2] / d / 1e6, cp_pr[3] / d / 1e6, cp_pr[4] / d / 1e6, cp_pr[5] / d / 1e6, cp_pr[6] / d / 1e6, cp_op[4][0] / d / 1e6);
        {
            int mt[8], mn = 0;
            for (int j = 0; j < 8; j++) { int b = -1; for (int i = 0; i < 256; i++) { bool u = false; for (int q = 0; q < mn; q++) u |= mt[q] == i; if (!u && cp_mk[i][0] && (b < 0 || cp_mk[i][0] > cp_mk[b][0])) b = i; } if (b < 0) break; mt[mn++] = b; }
            printf("cpu: marcas 00 por etiqueta (M ciclos, veces por cuadro):");
            for (int j = 0; j < mn; j++) printf(" %02X: %.2f (%.0f)", mt[j], cp_mk[mt[j]][0] / d / 1e6, cp_mk[mt[j]][1] / d);
            printf("\n");
        }
        printf("cpu: comandos GE %.2f M por cuadro, mayores (op: M ciclos, veces por cuadro):", tot / d / 1e6);
        for (int j = 0; j < k; j++) printf(" %02X: %.2f (%.0f)", top[j], cp_op[top[j]][0] / d / 1e6, cp_op[top[j]][1] / d);
        printf("\n");
    }
    memset(cp_box, 0, sizeof(cp_box)); memset(cp_job, 0, sizeof(cp_job)); memset(cp_op, 0, sizeof(cp_op)); memset(cp_em, 0, sizeof(cp_em)); memset(cp_st, 0, sizeof(cp_st)); memset(cp_gj, 0, sizeof(cp_gj)); memset(cp_vq, 0, sizeof(cp_vq)); memset(cp_tj, 0, sizeof(cp_tj)); memset(cp_lj, 0, sizeof(cp_lj)); memset(cp_lg, 0, sizeof(cp_lg)); memset(cp_db, 0, sizeof(cp_db)); memset(cp_at, 0, sizeof(cp_at)); memset(cp_ap, 0, sizeof(cp_ap)); memset(cp_pr, 0, sizeof(cp_pr)); memset(cp_mk, 0, sizeof(cp_mk));
    fflush(stdout);
    gv_draws = 0;
#endif
    lines++; frames = 0;
    for (auto& x : gv_n) x = 0;
    for (auto& x : gv_old) x = 0;
    for (auto& x : gv_t) x = 0;
#ifdef _WIN32
    for (auto& x : gv_mt_n) x = 0;
    for (auto& x : gv_mt_t) x = 0;
    for (auto& x : gv_p) x = 0;
    for (auto& x : sk_n) x = 0;
#endif
}

static uint64_t pm_st[2][4];   // per mode: prims, part cycles, sun cycles, differences
static int pm_mode() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("TTT_PART_MEMO"); v = e && *e ? atoi(e) : 1; }
    if (v != 2) return v ? 1 : 0;
    static uint32_t n = 0; static int m = 0; static uint64_t t0 = 0, rep = 0;
    if ((n++ & 255) == 0) {
        uint64_t t = box_ms();
        if (!t0) t0 = rep = t;
        m = (int)(((t - t0) / 10000) & 1);
        if (t >= rep + 20000) {
            rep = t;
            for (int k = 0; k < 2; k++) {
                double p = pm_st[k][0] ? (double)pm_st[k][0] : 1.0;
                printf("partes: %s | ciclos por prim: partes del luchador %.0f, sol cel %.0f | %llu prims | diferencias %llu\n", k ? "una busqueda" : "anterior",
                       pm_st[k][1] / p, pm_st[k][2] / p, (unsigned long long)pm_st[k][0], (unsigned long long)pm_st[k][3]);
            }
            fflush(stdout);
        }
    }
    return m;
}
static bool pm_check() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_PART_MEMO_CHECK"); v = e && *e ? atoi(e) : 0; } return v > 0; }

static void primitive_one(uint32_t arg) {
    PERF_N(PC_PRIM, 1);
    uint64_t pq0 = cp_on() ? perf_clock() : 0, pqt = pq0;
    const int pmm = pm_mode();
    auto pqk = [&](int k) { if (pq0) { uint64_t x = perf_clock(); cp_pr[k] += x - pqt; if (k == 2 || k == 4) pm_st[pmm][k == 2 ? 1 : 2] += x - pqt; pqt = x; } };
    if (pq0) { cp_pr[8]++; pm_st[pmm][0]++; }
    int pm_p = -2, pm_m = -1, pm_f = 0; uint32_t pm_h = 0;
    auto tpart = [&](int* m, int* f) -> int {   // the fighter part of this prim: looked up once per prim
        if (!pmm) return toon_part(v_addr, m, f);
        if (pm_p == -2) { pm_p = toon_part(v_addr, &pm_m, &pm_f); pm_h = toon_last_hdr; }
        else {
            toon_last_hdr = pm_h;
            if (pm_check()) { int cm = -1, cf = 0, cp = toon_part(v_addr, &cm, &cf); if (cp != pm_p || (cp >= 0 && (cm != pm_m || cf != pm_f || toon_last_hdr != pm_h))) { pm_st[1][3]++; static int lg = 0; if (lg++ < 10) printf("partes: diferencia en 0x%08X: %d/%d/%d frente a %d/%d/%d\n", v_addr, cp, cm, cf, pm_p, pm_m, pm_f); } toon_last_hdr = pm_h; }
        }
        *m = pm_m; *f = pm_f; return pm_p;
    };
    tc_gen++;
    prims_frame++;
    prim_serial++;
    skin_key = 0;
    if (in_vram_addr(v_addr) || in_vram_addr(i_addr)) psp_gpu_flush_by(PC_VVRAM);
    int type = (arg >> 16) & 7, n = arg & 0xFFFF;
    Fmt fm = format(R[0x12]);
    bool cleared = R[0xD3] & 1;
#if TTT_DIAG
    if (cleared && n > 0) note_clear(read_vert(fm, vert_addr(fm, 0)).color);
#endif
    // with TMAP 1 or 2 the uv come from position or normal, the vertex does not need them
    bool texture = (R[0x1E] & 1) && (fm.tc || (R[0xC0] & 3)) && !cleared;
    bool d3 = !fm.through;
    if (!d3) { if (cleared) pk_clr++; else if (texture) pk_tex++; else pk_flat++; }
    if (rm_track && texture && !(d3 && fm.wt) && psp_tex_bigmenu && room_quad(fm, n, d3)) { if (!fm.idx) v_addr += n * fm.stride; return; }
    if (texture) list_trace(fm, n, d3);
    if (sz_dbg) {
        static int logs = 0;
        if (logs++ < 16) {
            float g[8] = {}; uint32_t col = 0; quad_box(fm, n, d3, g, &col); Tex t = tex();
            printf("gpu: semillas %d prim tipo %d n %d tex %d 3d %d caja (%.1f, %.1f)-(%.1f, %.1f) color %08X textura %08X %dx%d fmt %d mezcla %d %06X prueba %06X %06X recorte %06X %06X descarte %d cara %d\n", sz_dbg, type, n, (int)texture, (int)d3,
                   g[0], g[2], g[1], g[3], col, t.dir, t.w, t.h, (int)t.fmt, (int)(R[0x21] & 1), R[0xDF] & 0xFFFFFF, R[0x22] & 0xFFFFFF, R[0xDB] & 0xFFFFFF, R[0xD4] & 0xFFFFFF, R[0xD5] & 0xFFFFFF, (int)lt_drop, bf_side);
        }
    }
    if (lt_drop) { if (!fm.idx) v_addr += n * fm.stride; return; }
    if (bf_side >= 0 && texture && !(d3 && fm.wt) && face_capture(fm, n, d3)) { if (!fm.idx) v_addr += n * fm.stride; return; }
    split_now = split_pass && fb().width >= 480 ? split_pass : 0;
    split_clip = split_now != 0;
    sm_prim(d3);
    if (d3) { prims_3d++; sc_prim3d++; sc_note(texture ? ((R[0xA0] & 0xFFFFFF) | ((R[0xA8] & 0xFF0000) << 8)) : 0u, (uint32_t)n);
             if (sc_act >= 0) sc_type[sc_act] |= (uint32_t)(1 << type); }
    pqk(0);
    if (d3 && type == 0 && n >= 3 && fm.wt && lod_type()) type = lod_type();
    else sc_spr++;
    if (patch_split_rt_once.val && split_pass && !split_hud && split_pass != split_first && fb().width < 480) {
        split_rt_skip++;
        if (!fm.idx) v_addr += n * fm.stride;
        return;
    }
    if (d3) skin_begin(fm);
    if (cap_on && !cap_map && !cap_pt_done && d3 && fm.wt) cap_points();
    if (rm_fit && !rm_done && d3 && fm.wt) room_points();
    pqk(1);
    toon_now = false;
    fig_now = toon_who && d3 && fm.wt;
    glow_now = 0;
    if (fig_now && texture && !split_now && glow_opt()) { int mt = -1, fl = 0; if (tpart(&mt, &fl) >= 0 && mt >= 0) glow_now = toon_glow(toon_last_hdr, mt); }
    cap_face_prim = false;
    if ((cap_on || pnl_on) && fig_now) { int m = -1, f = 0; cap_face_prim = tpart(&m, &f) >= 0 && f == 6; }
    prim_mat = -1; prim_blink = false;
    if (patch_char_textures.val && psp_tex_matid && toon_who && (!toon_only_nrm || fm.nrm) && d3 && fm.wt && texture) { int mt = -1, fl = 0; if (tpart(&mt, &fl) >= 0 && mt >= 0) { prim_mat = mt; prim_blink = fl == 6 && !cap_on && (toon_eyes || blink_now(toon_who - 1)); } }
    pqk(2);
    if (d3 && !split_now && !cap_zoom && !rm_zoom && !pnl_on && fb().width >= 480) atm_note(n);
    pqk(3);
    if (d3 && fm.wt && (!fm.nrm || toon_who) && texture && !(R[0x17] & 1) && glr_enabled() && fb().width >= 480 && (toon_all() || toon_force || (toon_only_nrm && !toon_none && toon_who && fm.nrm))) {   // BT3 models carry normals
        float sx = -0.146f, sy = -0.862f, sz = 0.485f; int sun_src = 0;
        uint32_t b = psp_l32(0x08A7E278u);
        if (b && b < 0x10000000u) {
            FReg x, y, z; x.u = psp_l32(b + 0xDF280u); y.u = psp_l32(b + 0xDF284u); z.u = psp_l32(b + 0xDF288u);
            float l = std::sqrt(x.f * x.f + y.f * y.f + z.f * z.f);
            if (l > 0.5f && l < 1.5f) { sx = x.f / l; sy = y.f / l; sz = z.f / l; sun_src = 1; } else sun_src = 0;
        } else sun_src = 0;
        mat43(m_view, sx, sy, sz, toon_lv, false);
        float l = std::sqrt(toon_lv[0] * toon_lv[0] + toon_lv[1] * toon_lv[1] + toon_lv[2] * toon_lv[2]);
        if (l > 1e-6f) { for (int k = 0; k < 3; k++) toon_lv[k] /= l; toon_now = true; }
        int mat = -1, flag = 0, part = toon_who ? tpart(&mat, &flag) : -1;
        toon_grp = part >= 0 ? (toon_epoch << 8) | (uint32_t)(part + 1) : 0;
        toon_face = part >= 0 && flag == 6;
        toon_tp = part >= 0 ? toon_tpal(toon_last_hdr, mat) : 0;
        if (part >= 0 && mat >= 0 && toon_mode() == 2) for (auto& t : tpal_sets) if (t.hdr == toon_last_hdr) { int s = metal_slot(t.sig, t.src, mat); if (s >= 0) toon_tp = (toon_tp & ~0xFC00u) | (uint32_t)(s + 1) << 10; break; }   // flags bits 2..7: metal slot + 1
        static int logs = 0;
        static int last_src = -1; static float last_s[3];
        if (sun_src != last_src || std::fabs(sx - last_s[0]) + std::fabs(sy - last_s[1]) + std::fabs(sz - last_s[2]) > 0.01f) { last_src = sun_src; last_s[0] = sx; last_s[1] = sy; last_s[2] = sz; static int tot = 0; if (logs >= 1 && tot < 10) { logs = 0; tot++; } }
        if (logs < 1) { logs++; printf("cel: sol %s", sun_src ? "del escenario" : "por defecto"); printf("  (%.3f %.3f %.3f), luz en vista (%.3f %.3f %.3f), modo %d\n", sx, sy, sz, toon_lv[0], toon_lv[1], toon_lv[2], toon_mode()); }
    }
    pqk(4);
    if (cap_dg && ((cap_on && !cap_map) || (cap_dg_post >= 0 && cap_dg_post < 400 && d3 && (R[0x21] & 1))) && cap_dg_n < 900) {
        bool in = cap_on && !cap_map;
        if (!in) cap_dg_post++;
        int m = -1, f = 0, pt = d3 && toon_who ? tpart(&m, &f) : -1;
        uint32_t ta = (R[0xA0] & 0xFFFFFF) | ((R[0xA8] & 0xFF0000) << 8);
        cap_dg_n++;
        printf("render: diag %s tipo %d n %d 3d %d piel %d tex %d %08X mezcla %d modo %06X fijo %06X %06X zprueba %d zfun %d zescribe %d alfa %d %06X mascaras %06X %02X parte %d mat %d bandera %d zoom %d destino %08X cel %d paleta %08X brillo %d mat %d texfunc %u matamb %06X alfamat %02X formato %u color %d normales %d luces %u\n", in ? "dentro" : "despues", type, n, d3, fm.wt, texture, ta,
               R[0x21] & 1, R[0xDF] & 0xFFFFFF, R[0xE0] & 0xFFFFFF, R[0xE1] & 0xFFFFFF, R[0x23] & 1, R[0xDE] & 7, !(R[0xE7] & 1), R[0x22] & 1, R[0xDB] & 0xFFFFFF, R[0xE8] & 0xFFFFFF, R[0xE9] & 0xFF, pt, m, f, cap_zoom, fb().dir, (int)toon_now, toon_tp, (int)glow_now, prim_mat, R[0xC9] & 0xFF, R[0x55] & 0xFFFFFF, R[0x58] & 0xFF, R[0xC3] & 0xF, (int)fm.col, (int)fm.nrm, R[0x17] & 1);
    }
    if (d3 && fm.wt && texture) {   // TTT_LIGHT_LOG: lighting state of skinned (fighter) draws, once per combination
        static int on = -1, n = 0; static uint32_t seen[24];
        if (on < 0) { const char* e = getenv("TTT_LIGHT_LOG"); on = e && *e == '1'; }
        uint32_t k = (uint32_t)fm.nrm | (uint32_t)fm.col << 2 | (R[0x17] & 1) << 5 | (R[0x18] & 1) << 6 | (R[0x19] & 1) << 7 | (R[0x1A] & 1) << 8 | (R[0x1B] & 1) << 9 | (R[0x53] & 7) << 10 | (R[0xC9] & 7) << 13 | ((R[0x5F] >> 8) & 3) << 16;
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= seen[i] == k;
        if (on && !dup && n < 24) {
            seen[n++] = k;
            printf("luz: piel normales %d color %d | luces %u (%u%u%u%u) tipo0 %u | matupd %u | ambiente 0x%06X mat amb 0x%06X dif 0x%06X | l0 (%.2f %.2f %.2f) dif 0x%06X amb 0x%06X | texfunc %u\n",
                   fm.nrm, fm.col, R[0x17] & 1, R[0x18] & 1, R[0x19] & 1, R[0x1A] & 1, R[0x1B] & 1, (R[0x5F] >> 8) & 3, R[0x53] & 7,
                   R[0x5C] & 0xFFFFFF, R[0x55] & 0xFFFFFF, R[0x56] & 0xFFFFFF, f24(R[0x63]), f24(R[0x64]), f24(R[0x65]), R[0x90] & 0xFFFFFF, R[0x8F] & 0xFFFFFF, R[0xC9] & 7);
        }
    }
    if (d3 && split_pass) {
        FB f = fb();
        int i = 0;
        while (i < rt_n && !(rt[i].view == split_pass && rt[i].dir == f.dir && rt[i].width == f.width)) i++;
        if (i == rt_n && rt_n < 24) rt[rt_n++] = {split_pass, f.dir, f.width, 0, 0};
        if (i < rt_n) { rt[i].prims++; rt[i].verts += (uint64_t)n; }
    }
    if (split_now && !split_hud && (alt_mask & 0x10)) { alt_drew |= 1 << split_now; FB af = fb(); alt_dir = af.dir; alt_w = af.width; }
    if (d3 && split_now) split_prims[split_now]++;
    if (d3 && type >= 3 && type <= 5 && !cleared && (patch_view_cull.val >= 2 || (patch_view_cull.val == 1 && split_now)) && prim_outside(fm, n)) {
        split_culled[split_now]++;
        if (!fm.idx) v_addr += n * fm.stride;
        return;
    }
    if (R[0x1F] & 1) { ge_fog++; ge_fog_col = R[0x5D] & 0xFFFFFF; ge_fog_f1 = f24(R[0x5B]); ge_fog_f2 = f24(R[0x5C]); }
    if (R[0x20] & 1) ge_dither++;
    if (R[0x25] & 1) ge_aa++;
    if (R[0x26] & 1) ge_flat++;
    if (R[0x27] & 1) ge_ctest++;
    if (R[0x28] & 1) ge_logop++;
#if TTT_DIAG
    if (d3) { note_stencil(); note_blend(); note_light(); if (texture) note_filter(); }
    if (d3 && texture && (R[0xC0] & 3) && n > 0) note_tmap(fm, vert_addr(fm, 0));
    if (d3 && fm.wt) { if (flat_bones(fm)) skin_flat++; else skin_full++; }
    if (d3 && n > 0 && (trace3d > 0 || (is_ramp(tex()) && trace_ramp < 8))) trace_3d(fm, R[0x12], type, n, vert_addr(fm, 0));
#endif
    float ke2 = (float)scale_int();
    bool corr2d = !d3 && !cleared && fit_2d_on() && (asp_x != 1.0f || asp_y != 1.0f) &&
                  !(texture && is_vram((R[0xA0] & 0xFFFFFF) | ((R[0xA8] & 0xFF0000) << 8)));
    // full-screen overlays keep covering the widened window: untextured ones (fades) always, textured blended ones only in a
    // battle (attack darkening; byte "battle active" 0x08B4E688): menu backgrounds keep their shape
    if (corr2d && patch_fit_full.val && n >= 2 && n <= 4096 && (!texture || ((R[0x21] & 1) && psp_l8(0x08B4E688u))) && full_screen_2d(fm, n)) corr2d = false;
    view2d_on = !d3 && split_now && !split_hud && !hud_code && !(n >= 2 && n <= 4096 && full_screen_2d(fm, n));
    bool touch2d = !d3 && (corr2d || ke2 != 1.0f || (hud_code && split_now) || view2d_on);
    if (type == 6) {
        for (int i = 0; i + 1 < n; i += 2) {
            Vert va = read_vert(fm, vert_addr(fm, i)), vb = read_vert(fm, vert_addr(fm, i + 1));
            if (touch2d) { fit_2d(va, corr2d, ke2); fit_2d(vb, corr2d, ke2); }
            if (d3) sprite_3d(va, vb, texture, cleared); else sprite(va, vb, texture, cleared);
            sprites++;
        }
        if (!fm.idx) v_addr += n * fm.stride;
        return;
    }
    if (d3) gp_n[type != 3 && type <= 5 && use_strips() ? 5 : type == 3 && fm.idx ? 6 : 7] += (uint64_t)n;
    pqk(5);
    if (type == 3 || type == 4 || type == 5) {
        if (d3 && gv_prim(fm, type, n, texture)) { pqk(6); if (pq0) cp_pr[7]++; if (!fm.idx) v_addr += n * fm.stride; return; }
        static const int cpu_ids[9] = {perf_sub_id("prim CPU: piel"), perf_sub_id("prim CPU: 1D"), perf_sub_id("prim CPU: 50"), perf_sub_id("prim CPU: cel"), perf_sub_id("prim CPU: medida"),
                                       perf_sub_id("prim CPU: agua"), perf_sub_id("prim CPU: captura"), perf_sub_id("prim CPU: otros"), perf_sub_id("prim CPU: sin ruta GPU")};
        GSub cpu_t_(cpu_ids[d3 ? gv_why_last : 8]);
        struct SlowP { double t; bool d3, tx; int type, n; ~SlowP() {   // a primitive of the CPU path that costs over 2 ms: what it draws
            if (t <= 0) return;
            double ms = (perf_seconds() - t) * 1000.0;
            static int logs = 0;
            if (ms < 2.0 || logs >= 40) return;
            logs++;
            Tex x = tex();
            printf("prim lenta: %.1f ms, %s, tipo %d, %d vertices, textura %s 0x%08X %dx%d fmt %d, motivo %d\n", ms, d3 ? "3D" : "2D", type, n, tx ? "si" : "no", x.dir, x.w, x.h, x.fmt, d3 ? gv_why_last : -1);
        } } slow_p_{gs_on() ? perf_seconds() : 0.0, d3, texture, type, n};
        if (d3 && type != 3 && n >= 3 && use_strips()) {
            VC c0, c1, c2;
            read_clip(fm, vert_addr(fm, 0), c0);
            read_clip(fm, vert_addr(fm, 1), c1);
            if (ge_fast2()) {
                const float eps = 1e-4f;
                bool gouraud = R[0x50] & 1;
                auto scr = [](const VC& c) { VP p = to_screen(c.c, c.color, c.uv); memcpy(p.vp, c.vp, sizeof(p.vp)); p.fn = c.fn; if (sm_rec) sm_vp(p, c); return p; };
                VP p0, p1, p2;
                auto gb = [](const VC& c) { return std::fabs(c.c[0]) > 8.0f * c.c[3] || std::fabs(c.c[1]) > 8.0f * c.c[3]; };
                bool h0 = c0.c[3] > eps, h1 = c1.c[3] > eps, g0 = gb(c0), g1 = gb(c1);
                if (h0) p0 = scr(c0);
                if (h1) p1 = scr(c1);
                for (int i = 2; i < n; i++) {
                    read_clip(fm, vert_addr(fm, i), c2);
                    bool h2 = c2.c[3] > eps, g2 = gb(c2);
                    if (h2) p2 = scr(c2);
                    bool inv = type == 4 && (i & 1);
                    if (h0 && h1 && h2 && !(g0 || g1 || g2)) {
                        if (sc_act >= 0) sc_in[sc_act]++;
                        rasterize(p0, p1, p2, texture, cleared, gouraud, inv);
                        prims_3d_ok++;
                        sc_draw++;
                    } else { VC in[3] = {c0, c1, c2}; triangle_3d_vc(in, texture, cleared, inv); }
                    if (type == 4) { c0 = c1; p0 = p1; h0 = h1; g0 = g1; }
                    c1 = c2; p1 = p2; h1 = h2; g1 = g2;
                }
                if (!fm.idx) v_addr += n * fm.stride;
                return;
            }
            for (int i = 2; i < n; i++) {
                read_clip(fm, vert_addr(fm, i), c2);
                if (type == 4) {
                    VC in[3] = {c0, c1, c2};
                    triangle_3d_vc(in, texture, cleared, (i & 1) != 0);
                    c0 = c1;
                } else {
                    VC in[3] = {c0, c1, c2};
                    triangle_3d_vc(in, texture, cleared, false);
                }
                c1 = c2;
            }
            if (!fm.idx) v_addr += n * fm.stride;
            return;
        }
        if (d3 && type == 3 && fm.idx && patch_ge_fast.val) {
            for (int i = 0; i + 2 < n; i += 3) {
                VC in[3] = {vc_get(fm, vert_addr(fm, i)), vc_get(fm, vert_addr(fm, i + 1)), vc_get(fm, vert_addr(fm, i + 2))};
                triangle_3d_vc(in, texture, cleared, false);
            }
            return;
        }
        int step = type == 3 ? 3 : 1, begin_at = type == 3 ? 0 : 2;
        for (int i = begin_at; i < n; i += step) {
            Vert v0, v1, v2;
            if (type == 3) { v0 = read_vert(fm, vert_addr(fm, i)); v1 = read_vert(fm, vert_addr(fm, i + 1)); v2 = read_vert(fm, vert_addr(fm, i + 2)); }
            else if (type == 4) { v0 = read_vert(fm, vert_addr(fm, i - 2)); v1 = read_vert(fm, vert_addr(fm, i - 1)); v2 = read_vert(fm, vert_addr(fm, i)); }
            else { v0 = read_vert(fm, vert_addr(fm, 0)); v1 = read_vert(fm, vert_addr(fm, i - 1)); v2 = read_vert(fm, vert_addr(fm, i)); }
            bool inv = type == 4 && (i & 1);   // alternating winding in strips
            if (touch2d) { fit_2d(v0, corr2d, ke2); fit_2d(v1, corr2d, ke2); fit_2d(v2, corr2d, ke2); }
            if (d3) triangle_3d(v0, v1, v2, texture, cleared, inv); else triangle(v0, v1, v2, texture, cleared);
        }
        if (!fm.idx) v_addr += n * fm.stride;
        return;
    }
    if (type == 0 || type == 1 || type == 2) {
        float g = point_size() * (float)scale_int();
        if (g < 1.0f) g = 1.0f;
        if (type == 0) {
            for (int i = 0; i < n; i++) {
                Vert v = read_vert(fm, vert_addr(fm, i));
                if (d3) { VP p; if (vp_of(v, p)) frame_vp(p, g, texture, cleared); }
                else { if (touch2d) fit_2d(v, corr2d, ke2); VP p; p.x = v.x; p.y = v.y; p.z = v.z; p.color = v.color; p.u = 0; p.v = 0; p.iw = 1.0f; frame_vp(p, g, texture, cleared); }
            }
        } else {
            int step = type == 1 ? 2 : 1, begin_at = type == 1 ? 0 : 1;
            for (int i = begin_at; i < n; i += step) {
                int ia = type == 1 ? i : i - 1, ib = type == 1 ? i + 1 : i;
                if (ib >= n) break;
                Vert va = read_vert(fm, vert_addr(fm, ia)), vb = read_vert(fm, vert_addr(fm, ib));
                if (d3) { VP p, q; if (vp_of(va, p) && vp_of(vb, q)) line_vp(p, q, g, texture, cleared); }
                else {
                    if (touch2d) { fit_2d(va, corr2d, ke2); fit_2d(vb, corr2d, ke2); }
                    VP p, q;
                    p.x = va.x; p.y = va.y; p.z = va.z; p.color = va.color; p.u = 0; p.v = 0; p.iw = 1.0f;
                    q.x = vb.x; q.y = vb.y; q.z = vb.z; q.color = vb.color; q.u = 0; q.v = 0; q.iw = 1.0f;
                    line_vp(p, q, g, texture, cleared);
                }
            }
        }
        if (!fm.idx) v_addr += n * fm.stride;
        return;
    }
    prims_other++;
    if (prims_other < 5) warning(d3 ? "primitiva 3D sin cubrir, tipo" : "primitiva 2D sin cubrir, tipo", type);
    if (!fm.idx) v_addr += n * fm.stride;
}

void psp_gpu_origin(uint32_t pc) { offset_addr = pc & 0x0FFFFFFF; }

static void alt_save(int p) {
    if (!(alt_mask & 0x10) || !(alt_drew & (1 << p)) || !glr_enabled()) return;
    psp_gpu_flush();
    int x0, y0, x1, y1, ke = scale_int();
    view_rect(p, x0, y0, x1, y1);
    if (glr_view_copy(alt_dir, alt_w, x0 * ke, y0 * ke, x1 * ke, y1 * ke, true)) { alt_saved[p] = alt_frame; alt_saves++; }
}

static void alt_paste() {
    if (alt_pasted == alt_frame) return;
    int m = alt_mask & ((1 << split_views) - 1);
    if (!(alt_mask & 0x10) || !m || !glr_enabled()) { alt_pasted = alt_frame; return; }
    FB f = fb();
    if (f.width < 480) return;
    psp_gpu_flush();
    int ke = scale_int();
    for (int v = 1; v <= split_views; v++) {
        if (!(m & (1 << (v - 1))) || alt_frame - alt_saved[v] > 3) continue;
        int x0, y0, x1, y1;
        view_rect(v, x0, y0, x1, y1);
        if (glr_view_copy(f.dir, f.width, x0 * ke, y0 * ke, x1 * ke, y1 * ke, false)) alt_pastes++;
    }
    alt_pasted = alt_frame;
}

int psp_gpu_split_alt_ok() { return glr_enabled() ? 1 : 0; }

// black dividers between the views, drawn when the last view ends and before the HUD
static void split_bar(float x0, float y0, float x1, float y1) {
    Vert a, b;
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    a.x = x0; a.y = y0; b.x = x1; b.y = y1;
    a.color = b.color = 0xFF000000u; a.has_color = b.has_color = true;
    float ke = (float)scale_int();
    fit_2d(a, false, ke); fit_2d(b, false, ke);
    sprite(a, b, false, true);
}

static void split_divider() {
    hud_code = 0; hud_flip = false; view2d_on = false;
    if (split_views == 2) split2_frame = alt_frame;
    if (dual2()) return;
    if (fb().width < 480) return;
    uint32_t d3 = R[0xD3], d4 = R[0xD4], d5 = R[0xD5];
    R[0xD3] = 0x101; R[0xD4] = 0; R[0xD5] = (271u << 10) | 479u; tc_rgen++;
    split_bar(239.0f, 0.0f, 241.0f, 272.0f);
    if (split_views == 4) split_bar(0.0f, 135.0f, 480.0f, 137.0f);
    R[0xD3] = d3; R[0xD4] = d4; R[0xD5] = d5; tc_rgen++;
}

// split: per view GE time, 3D primitives (skipped by view_cull) and triangles, averaged every 300 split frames
// TTT_SPLIT_PROF=1 outside split screen: where the GE time of a frame goes, every 300 frames
static void ge_report() {
    static int frames = 0, lines = 0;
    static uint64_t c0 = 0; static double t0 = 0;
    if (sprof_on <= 0 || lines >= 40) return;
    if (!t0) { t0 = perf_seconds(); c0 = perf_clock(); for (auto& x : sprof_c) x = 0; for (auto& x : gp_n) x = 0; return; }
    if (++frames < 300) return;
    double secs = perf_seconds() - t0, mhz = secs > 0 ? (perf_clock() - c0) / secs / 1e6 : 3000.0;
    auto ms = [&](uint64_t c) { return c / mhz / 1000.0 / frames; };
    auto pf = [&](uint64_t c) { return (double)c / frames; };
    printf("ge: ms por cuadro: leer %.2f transformar %.2f triangulos %.2f vaciar %.2f | vertices %.0f (piel %.0f, piel reusada %.0f) transformados %.0f triangulos %.0f | tiras %.0f, listas indexadas %.0f, otras %.0f\n",
           ms(sprof_c[0]), ms(sprof_c[1]), ms(sprof_c[2]), ms(sprof_c[3]), pf(gp_n[0]), pf(gp_n[1]), pf(gp_n[2]), pf(gp_n[3]), pf(gp_n[4]), pf(gp_n[5]), pf(gp_n[6]), pf(gp_n[7]));
    fflush(stdout);
    lines++; frames = 0; t0 = perf_seconds(); c0 = perf_clock();
    for (auto& x : sprof_c) x = 0;
    for (auto& x : gp_n) x = 0;
}

static void split_report() {
    static int lines = 0;
    split_frames++;
    if (split_frames == 1 || split_frames % 300 == 0) {
        if (split_frames > 1 && lines < 20) {
            lines++;
            int nv = split_views;
            printf("split: gpu %d vistas, ms por cuadro:", nv);
            for (int v = 0; v <= nv; v++) printf(" v%d %.2f", v, split_us[v] / 300000.0);
            printf(" | prims 3d (descartadas):");
            for (int v = 1; v <= nv; v++) printf(" v%d %.0f (%.0f)", v, split_prims[v] / 300.0, split_culled[v] / 300.0);
            printf(" | triangulos:");
            for (int v = 0; v <= nv; v++) printf(" v%d %.0f", v, split_tris[v] / 300.0);
            printf(" | destinos repetidos omitidos %.0f", split_rt_skip / 300.0);
            printf(" | alternas: copias %.2f pegadas %.2f", alt_saves / 300.0, alt_pastes / 300.0);
            if (sprof_on > 0) printf(" | M ciclos: leer %.2f transformar %.2f triangulos %.2f vaciar %.2f", sprof_c[0] / 3e8, sprof_c[1] / 3e8, sprof_c[2] / 3e8, sprof_c[3] / 3e8);
            printf("\n");
            if (lines <= 2) for (int i = 0; i < rt_n; i++) printf("split: destino vista %d fb 0x%08X ancho %d prims %.0f vertices %.0f\n", rt[i].view, rt[i].dir, rt[i].width, rt[i].prims / 300.0, rt[i].verts / 300.0);
            fflush(stdout);
        }
        for (int v = 0; v < 5; v++) split_us[v] = split_prims[v] = split_culled[v] = split_tris[v] = 0;
        for (int k = 0; k < 4; k++) sprof_c[k] = 0;
        alt_saves = alt_pastes = 0;
        split_rt_skip = 0;
        rt_n = 0;
    }
}

// BT3 stage layers (item 65): GE markers F1/F2 (word offset of the layer block from the F0 word) and F0 (group) lead every part list
// of a BT3 stage; the scroll sheet (EftStageScroll_Draw) is drawn at the first part after the sky (group 0), with the map's matrices
static uint32_t nop_pc = 0, ly_off = 0;
static bool ly_sky = false;
void psp_gpu_nop_pc(uint32_t pc) { nop_pc = pc; }
struct GeKeep {   // GE state and matrices (as the 0xEF marker keeps them)
    uint32_t r[256], v[5]; float w[12], vw[12], p[16], t[12], d[20], c[2]; int i[4];
    void save() {
        memcpy(r, R, sizeof r); memcpy(w, m_world, sizeof w); memcpy(vw, m_view, sizeof vw); memcpy(p, m_proj, sizeof p); memcpy(t, m_tgen, sizeof t);
        i[0] = i_world; i[1] = i_view; i[2] = i_proj; i[3] = i_tgen; v[0] = v_addr; v[1] = i_addr; v[2] = base; v[3] = offset_addr; v[4] = view_hash;
        memcpy(d, view_up, 12); memcpy(d + 3, view_inv, 48); memcpy(d + 15, proj_eye, 12); c[0] = psp_cam_m[17]; c[1] = psp_cam_m[18];
    }
    void load() {
        memcpy(R, r, sizeof r); tc_rgen++; memcpy(m_world, w, sizeof w); memcpy(m_view, vw, sizeof vw); memcpy(m_proj, p, sizeof p); memcpy(m_tgen, t, sizeof t);
        i_world = i[0]; i_view = i[1]; i_proj = i[2]; i_tgen = i[3]; v_addr = v[0]; i_addr = v[1]; base = v[2]; offset_addr = v[3]; view_hash = v[4];
        memcpy(view_up, d, 12); memcpy(view_inv, d + 3, 48); memcpy(proj_eye, d + 15, 12); psp_cam_m[17] = c[0]; psp_cam_m[18] = c[1];
        load_clut();
    }
};
static GeKeep ly_map;
// the layer data lives on the port side (registered by the disc when the stage is built); the GE reads vertices and marker textures
// from a port scratch block in guest memory (reserved by the game thread, psp_gpu_layer_scratch), never from the map's own memory
static std::mutex ly_mx;
static std::map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>> ly_tab;
static std::shared_ptr<const std::vector<uint8_t>> ly_cur;
static uint64_t ly_frame = 0;
static std::atomic<uint32_t> ly_scr{0}, ly_scr_n{0};
static const uint32_t LY_SCROLL = 0, LY_SURF = 0x80000, LY_OBJ = 0x200000, LY_TEXN = 0x10000;   // scratch regions: textures first, then vertices
void psp_gpu_layers_set(uint32_t token, const std::vector<uint8_t>& d) {
    std::lock_guard<std::mutex> g(ly_mx);
    ly_tab[token] = std::make_shared<const std::vector<uint8_t>>(d);
}
void psp_gpu_layer_scratch(uint32_t a, uint32_t n) { ly_scr_n = n; ly_scr = a; }
struct LyBlk {   // a block inside the layer data, bounds checked
    const uint8_t* p = nullptr; size_t n = 0;
    uint32_t u(size_t o) const { uint32_t v = 0; if (o + 4 <= n) memcpy(&v, p + o, 4); return v; }
    float f(size_t o) const { float v = 0; if (o + 4 <= n) memcpy(&v, p + o, 4); return v; }
    bool ok(size_t o, size_t len) const { return o <= n && len <= n - o; }
};
static LyBlk ly_block(const std::vector<uint8_t>& d, int slot) {
    LyBlk b;
    if (d.size() < 16 || memcmp(d.data(), "BLAY", 4)) return b;
    uint32_t o; memcpy(&o, d.data() + 4 * slot, 4);
    if (!o || o >= d.size()) return b;
    b.p = d.data() + o; b.n = d.size() - o;
    return b;
}
static bool scroll_on() {
    static int n = 0; static bool on = true;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_BT3_SCROLL"); on = !(e && *e == '0'); }
    return on;
}
static int surf_mode() {   // TTT_BT3_SURF: 0 off, 1 all, 2 reflecting only, 3 plain only (diagnostic)
    static int n = 0, v = 1;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_BT3_SURF"); v = e && *e ? atoi(e) : 1; }
    return v;
}
static bool surf_on() { return surf_mode() != 0; }
static uint32_t f24w(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 8; }
static void ly_state() {   // BT3 effect pass: alpha blend 2As / 1-2As (PS2 alpha), depth test without writes, modulate x2, repeat, linear
    offset_addr = 0;
    static const uint32_t S[][2] = {{0x12, 0x19F}, {0x17, 0}, {0x1D, 0}, {0x1E, 1}, {0x1F, 0}, {0x21, 1}, {0x22, 0}, {0x23, 1}, {0x24, 0}, {0x27, 0}, {0x28, 0},
                                    {0xDE, 7}, {0xE7, 1}, {0xDF, 0x76}, {0xC9, 0x10100}, {0xC7, 0}, {0xC6, 0x101}, {0xC0, 0}, {0xC2, 0}, {0xC3, 5}, {0xC5, 0xFF03}};
    for (auto& c : S) psp_gpu_cmd((int)c[0], c[1]);
    psp_gpu_cmd(0x48, f24w(1.0f)); psp_gpu_cmd(0x49, f24w(1.0f)); psp_gpu_cmd(0x4A, 0); psp_gpu_cmd(0x4B, 0);
}
// the marker texture i of a block (pixels + clut offsets in the layer data) copied into the scratch, then bound
static void ly_tex(const LyBlk& b, uint32_t pxo, uint32_t clo, uint32_t at) {
    if (!b.ok(pxo, 256) || !b.ok(clo, 1024)) return;
    memcpy(psp_p(at), b.p + pxo, 256); memcpy(psp_p(at + 256), b.p + clo, 1024);
    uint32_t px = at, cl = at + 256;
    psp_gpu_cmd(0xB0, cl & 0xFFFFFF); psp_gpu_cmd(0xB1, (cl >> 8) & 0x0F0000); psp_gpu_cmd(0xC4, 32);
    psp_gpu_cmd(0xA0, px & 0xFFFFFF); psp_gpu_cmd(0xA8, ((px >> 8) & 0x0F0000) | 16); psp_gpu_cmd(0xB8, 4 | 4 << 8);
}
static void ly_prim(uint32_t vb, uint32_t n) { psp_gpu_cmd(0x10, (vb >> 8) & 0x0F0000); psp_gpu_cmd(0x01, vb & 0xFFFFFF); psp_gpu_cmd(0x04, 3u << 16 | n); }
static void scroll_draw(const LyBlk& b, uint32_t scr) {
    if (!b.ok(0, 32) || memcmp(b.p, "BSCR", 4)) return;
    uint32_t nt = b.u(4);
    if (!nt || nt > 32 || !b.ok(32, 16 * (size_t)nt)) return;
    float sz = b.f(8), sp = b.f(12), dx = b.f(16), dy = b.f(20);
    struct St { float x = 0, y = 0; std::chrono::steady_clock::time_point t; bool live = false; };
    static std::map<const void*, St> st;
    St& q = st[b.p];
    auto now = std::chrono::steady_clock::now();
    if (q.live) {   // EftStageScroll_Update: dir * speed per 30 Hz step, wrapped into (-size, size)
        float steps = std::min(std::chrono::duration<float>(now - q.t).count() * 30.0f, 8.0f);
        q.x += dx * sp * steps; q.y += dy * sp * steps;
        if (q.x > sz) q.x -= sz; else if (q.x < -sz) q.x += sz;
        if (q.y > sz) q.y -= sz; else if (q.y < -sz) q.y += sz;
    }
    q.t = now; q.live = true;
    psp_gpu_flush_by(PC_VDEST);
    static GeKeep keep;
    keep.save();
    ly_state();
    psp_gpu_cmd(0x4A, f24w(q.x)); psp_gpu_cmd(0x4B, f24w(q.y));
    int tris = 0;
    uint32_t vat = scr + LY_TEXN, vend = scr + LY_SURF;
    for (uint32_t i = 0; i < nt; i++) {
        size_t e = 32 + 16 * (size_t)i;
        uint32_t vo = b.u(e + 8), n = b.u(e + 12);
        if (!n || n > 0x30000 || !b.ok(vo, 24 * (size_t)n) || vat + 24 * n > vend) continue;
        memcpy(psp_p(vat), b.p + vo, 24 * (size_t)n);
        ly_tex(b, b.u(e), b.u(e + 4), scr + 1280 * i);
        ly_prim(vat, n);
        vat += (24 * n + 63) & ~63u;
        tris += (int)n / 3;
    }
    psp_gpu_flush_by(PC_VDEST);
    keep.load();
    static int logs = 0;
    if (logs < 3) { logs++; printf("nubes: lamina BT3 dibujada, %d triangulos, desplazamiento (%.4f, %.4f)\n", tris, q.x, q.y); }
}
static void layer_after_objects();
uint32_t ly_tex_force = 0;   // texture id of the BT3 stage material being drawn (texture animation), 0 = none
// StgMTexAnim: stage timers advance 0.1333 per 30 Hz update and fire when they pass their period; a material whose timer fired
// steps its texture on its next draw (loop, or back and forth with bit 15 of cur); the texture drawn is cur & 0xFF
static uint32_t texanim(int k) {
    if (!ly_cur || k < 0) return 0;
    LyBlk b = ly_block(*ly_cur, 3);
    if (!b.ok(0, 16) || memcmp(b.p, "BTAN", 4)) return 0;
    uint32_t ns = b.u(4), nt = b.u(8), ntex = b.u(12);
    if ((uint32_t)k >= ns || nt > 256 || !b.ok(16, 16 * (size_t)nt + 8 * (size_t)ns)) return 0;
    struct Tm { float time = 0; uint32_t fired = 0; };
    struct St { std::vector<Tm> t; std::vector<uint32_t> cur, seen; int64_t tick = -1; };
    static std::map<const void*, St> st;
    St& S = st[b.p];
    if (S.t.size() != nt) {
        S.t.assign(nt, Tm()); S.cur.resize(ns); S.seen.assign(ns, 0);
        for (uint32_t i = 0; i < ns; i++) S.cur[i] = b.u(16 + 16 * nt + 8 * i + 4);
    }
    int64_t now = (int64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 30.0);
    if (S.tick < 0 || now - S.tick > 8) S.tick = now - 1;
    for (; S.tick < now; S.tick++)
        for (uint32_t i = 0; i < nt; i++) {
            Tm& t = S.t[i];
            t.time += 0.13333333f;
            if (b.f(16 + 16 * i + 4) < t.time) { t.fired++; t.time = 0.0f; }
        }
    uint32_t ti = b.u(16 + 16 * nt + 8 * k);
    if (ti < nt && S.seen[k] != S.t[ti].fired) {
        S.seen[k] = S.t[ti].fired;
        uint32_t fl = b.u(16 + 16 * ti), cnt = fl >> 16, fo = b.u(16 + 16 * ti + 8);
        if (cnt && b.ok(fo, 2 * (size_t)cnt)) {
            auto fr = [&](int j) { uint16_t v; memcpy(&v, b.p + fo + 2 * (size_t)j, 2); return (uint32_t)v; };
            uint32_t& c = S.cur[k];
            for (uint32_t j = 0; j < cnt; j++) {
                if (fr((int)j) != (c & 0x7FFF)) continue;
                if (fl & 1) c = j == cnt - 1 ? fr(0) : fr((int)j + 1);
                else if (c & 0x8000) c = j == 0 ? fr(cnt > 1 ? 1 : 0) : fr((int)j - 1) | 0x8000;
                else c = j == cnt - 1 ? fr(cnt > 1 ? (int)cnt - 2 : 0) | 0x8000 : fr((int)j + 1);
                break;
            }
        }
    }
    uint32_t tex = S.cur[k] & 0xFF;
    return tex < ntex ? ly_off + tex : 0;
}
// StgModel_UpdateAnims / StgModel_DrawAnims / Vu1Node_Animate / VU1 prog 8: every MAPD object with anim > 0 plays its node track (frame
// += 2 per 30 Hz update from a random start, back to 0 past the length), object matrix Rz Rx Ry then T, node matrices through a stack
// (T local, then Y, X', Z' local), each vertex blended between the node matrix and the one it was entered with by its weight
struct ObjM { float r[4][4]; };
static void om_id(ObjM& m) { memset(&m, 0, sizeof m); for (int i = 0; i < 4; i++) m.r[i][i] = 1.0f; }
static void om_post(ObjM& m, const float R3[3][3]) {   // m = m * R (rotation applied after)
    for (int i = 0; i < 4; i++) { float a[3]; for (int j = 0; j < 3; j++) a[j] = m.r[i][0] * R3[0][j] + m.r[i][1] * R3[1][j] + m.r[i][2] * R3[2][j]; for (int j = 0; j < 3; j++) m.r[i][j] = a[j]; }
}
static void om_rows(ObjM& m, int i, int j, float c, float s) {   // rows i, j = c ri - s rj, s ri + c rj (Vu0Cur_RotateYXZ, local)
    for (int k = 0; k < 4; k++) { float a = m.r[i][k], b = m.r[j][k]; m.r[i][k] = c * a - s * b; m.r[j][k] = s * a + c * b; }
}
struct ObjSt { std::vector<float> frame; std::vector<std::array<float, 8>> node; std::vector<uint32_t> nbase; int64_t tick = -1; uint32_t seed = 0x2545F491u; };
static bool obj_on() {
    static int n = 0; static bool on = true;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_BT3_OBJ"); on = !(e && *e == '0'); }
    return on;
}
struct ObjGeV { float u, v; uint32_t c; float x, y, z; };
template <class E>
static int obj_pose(const LyBlk& b, ObjSt& S, int64_t now, E emit) {
    if (!b.ok(0, 32) || memcmp(b.p, "BOBJ", 4)) return 0;
    uint32_t na = b.u(4), no = b.u(8), at = b.u(16), oo = b.u(20);
    if (!na || na > 64 || !no || no > 4096 || !b.ok(at, 16 * (size_t)na) || !b.ok(oo, 32 * (size_t)no)) return 0;
    auto alen = [&](uint32_t a) { return a >= 1 && a <= na ? (float)b.u(at + 16 * (a - 1)) : 0.0f; };
    if (S.frame.size() != no) {
        S.frame.assign(no, 0.0f); S.nbase.assign(na + 1, 0); S.node.clear();
        for (uint32_t i = 0; i < no; i++) { S.seed = S.seed * 1664525u + 1013904223u; S.frame[i] = (float)(S.seed >> 8) / 16777216.0f * alen(b.u(oo + 32 * i)); }
        for (uint32_t a = 0; a < na; a++) {
            S.nbase[a] = (uint32_t)S.node.size();
            uint32_t nn = b.u(at + 16 * a + 4), no0 = b.u(at + 16 * a + 8);
            if (nn > 256 || !b.ok(no0, 96 * (size_t)nn)) nn = 0;
            for (uint32_t i = 0; i < nn; i++) { std::array<float, 8> q; for (int j = 0; j < 8; j++) q[j] = b.f(no0 + 96 * i + 32 + 4 * j); S.node.push_back(q); }
        }
        S.nbase[na] = (uint32_t)S.node.size();
    }
    if (S.tick < 0 || now - S.tick > 8) S.tick = now - 1;
    for (; S.tick < now; S.tick++)
        for (uint32_t i = 0; i < no; i++) { float L = alen(b.u(oo + 32 * i)); S.frame[i] += 2.0f; if (L < S.frame[i]) S.frame[i] = 0.0f; }
    static const float K[3] = {16384.0f, 8192.0f, 16384.0f}, DEG = 3.14159265f / 180.0f;
    std::vector<ObjM> par, wor, stk;
    std::vector<ObjGeV> out;
    int drawn = 0;
    for (uint32_t a = 0; a < na; a++) {
        uint32_t nn = S.nbase[a + 1] - S.nbase[a], nt0 = b.u(at + 16 * a + 8);
        if (!nn) continue;
        for (uint32_t j = 0; j < no; j++) {
            size_t ob = oo + 32 * (size_t)j;
            if (b.u(ob) != a + 1) continue;
            float rx = b.f(ob + 20) * DEG, ry = b.f(ob + 24) * DEG, rz = b.f(ob + 28) * DEG, fr = S.frame[j];
            ObjM cur; om_id(cur);
            { float c = std::cos(rz), s = std::sin(rz); const float R3[3][3] = {{c, s, 0}, {-s, c, 0}, {0, 0, 1}}; om_post(cur, R3); }
            { float c = std::cos(rx), s = std::sin(rx); const float R3[3][3] = {{1, 0, 0}, {0, c, s}, {0, -s, c}}; om_post(cur, R3); }
            { float c = std::cos(ry), s = std::sin(ry); const float R3[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}}; om_post(cur, R3); }
            for (int k = 0; k < 3; k++) cur.r[3][k] += b.f(ob + 8 + 4 * k);
            par.assign(nn, cur); wor.assign(nn, cur); stk.clear();
            for (uint32_t i = 0; i < nn; i++) {
                size_t e = nt0 + 96 * (size_t)i;
                std::array<float, 8>& q = S.node[S.nbase[a] + i];
                stk.push_back(cur);
                uint32_t kc = b.u(e + 16), ko = b.u(e + 20);
                if (kc > 1 && b.ok(ko, 32 * (size_t)kc)) {
                    auto kf = [&](uint32_t k) { return (float)b.u(ko + 32 * k + 12); };
                    auto kv = [&](uint32_t k, int c) { return b.f(ko + 32 * k + (c < 3 ? 4 * c : 16 + 4 * (c - 3))); };
                    for (uint32_t k = 1; k < kc; k++) {
                        if (kf(k - 1) == fr) { for (int c = 0; c < 3; c++) { q[c] = kv(k - 1, c); q[4 + c] = kv(k - 1, 3 + c); } break; }
                        if (kf(k - 1) < fr && fr < kf(k)) {
                            float t = (fr - kf(k - 1)) / (kf(k) - kf(k - 1));
                            for (int c = 0; c < 3; c++) { q[c] = (kv(k, c) - kv(k - 1, c)) * t + kv(k - 1, c); q[4 + c] = (kv(k, 3 + c) - kv(k - 1, 3 + c)) * t + kv(k - 1, 3 + c); }
                            break;
                        }
                        if (kf(k) == fr) { for (int c = 0; c < 3; c++) { q[c] = kv(k, c); q[4 + c] = kv(k, 3 + c); } break; }
                    }
                }
                par[i] = cur;
                for (int c = 0; c < 4; c++) cur.r[3][c] = cur.r[0][c] * q[0] + cur.r[1][c] * q[1] + cur.r[2][c] * q[2] + cur.r[3][c];
                om_rows(cur, 0, 2, std::cos(q[5]), std::sin(q[5]));
                om_rows(cur, 1, 2, std::cos(q[4]), std::sin(q[4]));
                om_rows(cur, 0, 1, std::cos(q[6]), std::sin(q[6]));
                wor[i] = cur;
                for (uint32_t p = b.u(e); p > 0 && !stk.empty(); p--) { cur = stk.back(); stk.pop_back(); }
            }
            for (uint32_t i = 0; i < nn; i++) {
                size_t e = nt0 + 96 * (size_t)i;
                uint32_t nb = b.u(e + 8), bo = b.u(e + 12);
                if (!nb || nb > 4096 || !b.ok(bo, 16 * (size_t)nb)) continue;
                float pa[3] = {b.f(e + 64), b.f(e + 68), b.f(e + 72)}, pb[3] = {b.f(e + 80), b.f(e + 84), b.f(e + 88)};
                const ObjM &A = wor[i], &B = par[i];
                for (uint32_t k = 0; k < nb; k++) {
                    uint32_t tx = b.u(bo + 16 * k), nv = b.u(bo + 16 * k + 4), vo = b.u(bo + 16 * k + 8), pr = b.u(bo + 16 * k + 12);
                    if (!nv || nv > 0x8000 || !b.ok(vo, 40 * (size_t)nv)) continue;
                    out.resize(nv);
                    for (uint32_t m = 0; m < nv; m++) {
                        size_t v = vo + 40 * (size_t)m;
                        float x = b.f(v), y = b.f(v + 4), z = b.f(v + 8), w = b.f(v + 12), ra[3], rb[3];
                        for (int c = 0; c < 3; c++) {
                            ra[c] = A.r[0][c] * (x - pa[0]) + A.r[1][c] * (y - pa[1]) + A.r[2][c] * (z - pa[2]) + A.r[3][c];
                            rb[c] = B.r[0][c] * (x - pb[0]) + B.r[1][c] * (y - pb[1]) + B.r[2][c] * (z - pb[2]) + B.r[3][c];
                        }
                        auto ci = [&](float f, float k2) { long r = (long)f; r = std::max(0L, std::min(255L, r)); return (uint32_t)std::min(255L, std::lround((float)r * k2)); };
                        ObjGeV& g = out[m];
                        g.u = b.f(v + 32); g.v = b.f(v + 36);
                        g.c = ci(b.f(v + 16), 1) | ci(b.f(v + 20), 1) << 8 | ci(b.f(v + 24), 1) << 16 | ci(b.f(v + 28), 255.0f / 128.0f) << 24;
                        g.x = (rb[0] + (ra[0] - rb[0]) * w) / K[0]; g.y = (rb[1] + (ra[1] - rb[1]) * w) / K[1]; g.z = (rb[2] + (ra[2] - rb[2]) * w) / K[2];
                    }
                    emit(tx, pr, out.data(), nv);
                    drawn += (int)nv;
                }
            }
        }
    }
    return drawn;
}
static bool ly_obj_g2 = false, ly_obj_done = false;
static void obj_draw(const char* where) {
    auto cur = ly_cur;
    uint32_t scr = ly_scr, n = ly_scr_n;
    if (!cur || !scr || n <= LY_OBJ + LY_TEXN || !obj_on()) return;
    LyBlk b = ly_block(*cur, 4);
    if (!b.ok(0, 32) || memcmp(b.p, "BOBJ", 4)) return;
    static std::map<const void*, ObjSt> st;
    ObjSt& S = st[b.p];
    uint32_t ntex = b.u(12), tt = b.u(24), base = scr + LY_OBJ, vat = base + LY_TEXN, vend = scr + n;
    if (ntex > 32 || !b.ok(tt, 8 * (size_t)ntex)) return;
    int64_t now = (int64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 30.0);
    psp_gpu_flush_by(PC_VDEST);
    static GeKeep keep;
    keep.save();
    ly_map.load();
    ly_state();
    psp_gpu_cmd(0xE7, 0); psp_gpu_cmd(0x22, 1); psp_gpu_cmd(0xDB, 7 | 0x7F << 8 | 0xFF << 16);
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t px = b.u(tt + 8 * i), cl = b.u(tt + 8 * i + 4);
        if (b.ok(px, 256) && b.ok(cl, 1024)) { memcpy(psp_p(base + 1280 * i), b.p + px, 256); memcpy(psp_p(base + 1280 * i + 256), b.p + cl, 1024); }
    }
    uint32_t bound = 0xFFFFFFFEu;
    int batches = 0;
    int nv = obj_pose(b, S, now, [&](uint32_t tx, uint32_t pr, const ObjGeV* v, uint32_t cnt) {
        uint32_t sz = (uint32_t)sizeof(ObjGeV) * cnt;
        if (vat + sz > vend) return;
        memcpy(psp_p(vat), v, sz);
        if (tx != bound) {
            bound = tx;
            if (tx < ntex) {
                uint32_t px = base + 1280 * tx, cl = px + 256;
                psp_gpu_cmd(0x1E, 1);
                psp_gpu_cmd(0xB0, cl & 0xFFFFFF); psp_gpu_cmd(0xB1, (cl >> 8) & 0x0F0000); psp_gpu_cmd(0xC4, 32);
                psp_gpu_cmd(0xA0, px & 0xFFFFFF); psp_gpu_cmd(0xA8, ((px >> 8) & 0x0F0000) | 16); psp_gpu_cmd(0xB8, 4 | 4 << 8);
            } else psp_gpu_cmd(0x1E, 0);
        }
        psp_gpu_cmd(0x10, (vat >> 8) & 0x0F0000); psp_gpu_cmd(0x01, vat & 0xFFFFFF); psp_gpu_cmd(0x04, (pr & 7) << 16 | cnt);
        vat += (sz + 63) & ~63u;
        batches++;
    });
    psp_gpu_flush_by(PC_VDEST);
    keep.load();
    static int logs = 0;
    if (logs < 3) { logs++; printf("objetos: objetos animados BT3 dibujados %s, %d vertices en %d lotes\n", where, nv, batches); }
}
static void layer_mark(uint32_t arg) {
    int tag = (int)(arg >> 16), g = (int)(arg & 0xFF);
    if (tag == 0xF1) { ly_off = (ly_off & 0xFFFF0000u) | (arg & 0xFFFF); return; }
    if (tag == 0xF2) { ly_off = (ly_off & 0xFFFFu) | (arg & 0xFFFF) << 16; return; }
    if (tag == 0xF5) { layer_after_objects(); return; }
    if (tag == 0xF7) { ly_tex_force = 0; return; }
    if (tag == 0xF6) { ly_tex_force = texanim((int)(arg & 0xFFFF) - 1); return; }
    ly_tex_force = 0;
    static std::string seq; static int last = -1, logs = 0;
    if (g != last && seq.size() < 64) { seq += std::to_string(g) + " "; last = g; }
    if (g == 0) { ly_sky = true; ly_obj_g2 = ly_obj_done = false; return; }
    if (g == 2) ly_obj_g2 = true;
    else if (g >= 3 && ly_obj_g2 && !ly_obj_done) { ly_obj_done = true; obj_draw("tras el grupo 2"); }   // BT3: after group 2, before the blended groups
    if (!ly_sky) return;
    ly_sky = false;
    if (logs < 3) { logs++; printf("nubes: grupos antes del dibujo: %s\n", seq.c_str()); }
    seq.clear(); last = -1;
    { std::lock_guard<std::mutex> lk(ly_mx); auto it = ly_tab.find(ly_off); ly_cur = it == ly_tab.end() ? nullptr : it->second; }
    ly_frame = frames_drawn;
    ly_map.save();
    uint32_t scr = ly_scr;
    if (!ly_cur || !scr || !scroll_on()) return;
    LyBlk b = ly_block(*ly_cur, 1);
    if (b.p) scroll_draw(b, scr + LY_SCROLL);
}
// camera position and view axes (right, screen down, forward) in BT3 units from the map's world x view (row vectors: p A + t)
static bool ly_camera(float* cam, float* vx, float* vy, float* fw) {
    float A[9], t[3];
    const float *mw = ly_map.w, *mv = ly_map.vw;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) A[3 * i + j] = mw[3 * i] * mv[j] + mw[3 * i + 1] * mv[3 + j] + mw[3 * i + 2] * mv[6 + j];
    for (int j = 0; j < 3; j++) t[j] = mw[9] * mv[j] + mw[10] * mv[3 + j] + mw[11] * mv[6 + j] + mv[9 + j];
    float det = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
    if (!(std::fabs(det) > 1e-12f)) return false;
    float I[9] = {(A[4] * A[8] - A[5] * A[7]) / det, -(A[1] * A[8] - A[2] * A[7]) / det, (A[1] * A[5] - A[2] * A[4]) / det,
                  -(A[3] * A[8] - A[5] * A[6]) / det, (A[0] * A[8] - A[2] * A[6]) / det, -(A[0] * A[5] - A[2] * A[3]) / det,
                  (A[3] * A[7] - A[4] * A[6]) / det, -(A[0] * A[7] - A[1] * A[6]) / det, (A[0] * A[4] - A[1] * A[3]) / det};
    const float K[3] = {16384.0f, 8192.0f, 16384.0f};
    for (int j = 0; j < 3; j++) { cam[j] = -(t[0] * I[j] + t[1] * I[3 + j] + t[2] * I[6 + j]) * K[j]; fw[j] = -I[6 + j] * K[j]; vx[j] = I[j] * K[j]; vy[j] = -I[3 + j] * K[j]; }
    auto unit = [](float* v) { float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (!(l > 1e-9f)) return false; for (int k = 0; k < 3; k++) v[k] /= l; return true; };
    return unit(fw) && unit(vx) && unit(vy);
}
static int ptcl_next_key();
static void ptcl_upto(int key);
// BT3 EftSurf_Animate (one step per 30 Hz update) + EftSurf_DrawMeshes: scroll / wave / pulse per mesh, near fade, every triangle queued in
// BT3's ordering table by its first vertex's depth (screen z >> 8 less zBias; layer 0 alpha blend, layer 1 additive), drawn far to near
static void surf_draw(const LyBlk& b, uint32_t scr, uint32_t scr_end) {
    if (!b.ok(0, 32) || memcmp(b.p, "BSRF", 4)) return;
    uint32_t nm = b.u(4), nt = b.u(8), total = b.u(24);
    if (!nm || nm > 4096 || !nt || nt > 32 || !total) return;
    float wy = b.f(12);
    bool hw = b.u(16) != 0;
    size_t mt = 32 + 8 * (size_t)nt;
    if (!b.ok(mt, 64 * (size_t)nm)) return;
    struct Ms { float u, v, du, dv; };
    struct St { std::vector<Ms> m; float acc = 0, racc = 0; int rframe = 0; std::chrono::steady_clock::time_point t; };
    static std::map<const void*, St> st;
    St& S = st[b.p];
    uint32_t ro = b.u(28), rfn = 0, rw = 0, rh = 0;   // reflecting surface (EftSurfRt)
    float RP[15] = {0};
    size_t rdat = 0, rstep = 0;
    if (ro && b.ok(ro, 76) && !memcmp(b.p + ro, "BRFL", 4)) {
        rfn = b.u(ro + 4); rw = b.u(ro + 8); rh = b.u(ro + 12);
        for (int i = 0; i < 15; i++) RP[i] = b.f(ro + 16 + 4 * i);
        for (int k = 0; k < 3; k++) wat_scol[k] = std::min(1.0f, (RP[k] + RP[4 + k]) / 510.0f);   // Enhanced water tint: colorA + colorB
        wat_scol[3] = 1.0f;
        rdat = ro + 80; rstep = (size_t)rw * rh + 256 * 12;
        if (!rfn || rw > 512 || rh > 512 || (rw & (rw - 1)) || (rh & (rh - 1)) || !b.ok(rdat, rstep * rfn)) rfn = 0;
    }
    auto now = std::chrono::steady_clock::now();
    if (S.m.size() != nm) {
        S.m.resize(nm);
        for (uint32_t i = 0; i < nm; i++) { size_t e = mt + 64 * i; S.m[i] = {b.f(e + 24), b.f(e + 28), b.f(e + 16), b.f(e + 20)}; }
        S.t = now;
    }
    S.acc = std::min(S.acc + std::chrono::duration<float>(now - S.t).count() * 30.0f, 8.0f);
    S.t = now;
    const float TAU = 6.2831853f;
    for (; S.acc >= 1.0f; S.acc -= 1.0f) {
        if (rfn) {   // EftSurf_StepAnim: animSpeed / frames per update, the normal map frame advances when it reaches 1
            S.racc += RP[13] / (float)rfn;
            if (S.racc >= 1.0f) { S.racc = 0.0f; S.rframe++; }
            if (S.rframe > (int)rfn - 1) S.rframe = 0;
        }
        for (uint32_t i = 0; i < nm; i++) {
            Ms& m = S.m[i];
            uint32_t fl = b.u(mt + 64 * i + 36);
            if (fl & 0x100) {
                m.v += m.dv;
                if (m.dv == 0.0f) { m.dv = -0.001f; m.v = 1.0f; }
                if (m.v > 1.0f) { m.v = 1.0f; m.dv = -m.dv; }
                if (m.v < 0.95f) { m.v = 0.95f; m.dv = -m.dv; }
                m.u += m.du; if (m.u < 0.0f) m.u = 0.0f; if (m.u > TAU) m.u = -TAU;
            } else if (fl & 0x80) {
                m.v += m.dv; m.u += m.du; if (m.v > 1.0f) m.v -= 1.0f; if (m.u > TAU) m.u = -TAU;
            } else {
                m.u += m.du; m.v += m.dv; if (m.u > 1.0f) m.u -= 1.0f; if (m.v > 1.0f) m.v -= 1.0f;
            }
        }
    }
    const float K[3] = {16384.0f, 8192.0f, 16384.0f};
    float cam[3], fw[3], vx[3], vy[3];
    if (!ly_camera(cam, vx, vy, fw)) return;
    const double zn = 0.3, zf = 65536.0, z0 = 1.0, z1 = 10877000.0, cz = (-z1 * zn + z0 * zf) / (zf - zn), az = zf * zn * (z1 - z0) / (zf - zn);
    struct Tq { int key; uint32_t mesh, tri; float fade; int kind; };   // kind 0 plain, 1 reflect diffuse, 2 reflect specular
    std::vector<Tq> q;
    q.reserve(total);
    for (uint32_t i = 0; i < nm; i++) {
        size_t e = mt + 64 * i;
        uint32_t n = b.u(e + 4), src = b.u(e + 8), fl = b.u(e + 36);
        int zb = (int)b.u(e + 32);
        if (!n || (fl & 0x60) || ((fl & 0x800) && !rfn) || !b.ok(src, 108 * (size_t)n)) continue;
        if ((surf_mode() == 2 && !(fl & 0x800)) || (surf_mode() == 3 && (fl & 0x800))) continue;
        float fade = 1.0f;
        if (fl & 0x200) {
            float dx = b.f(e + 40) - cam[0], dy = b.f(e + 44) - (hw ? wy : 0.0f), dz = b.f(e + 48) - cam[2], d = dx * dx + dy * dy + dz * dz;
            if (d > 160000.0f) continue;
            if (d > 40000.0f) fade -= (d - 40000.0f) / 120000.0f;
        }
        int layer = (fl & 8) ? 1 : 0;
        for (uint32_t k = 0; k < n; k++) {
            size_t v = src + 108 * (size_t)k;
            double d = (b.f(v) * K[0] - cam[0]) * fw[0] + (b.f(v + 4) * K[1] - cam[1]) * fw[1] + (b.f(v + 8) * K[2] - cam[2]) * fw[2];
            double z = d > zn ? cz + az / d : 16777215.0;
            if (!(z == z)) z = 16777215.0;
            int slot = (int)(std::min(std::max(z, 0.0), 16777215.0) / 256.0) - zb;
            slot = std::max(0, std::min(slot, 0xFFF));
            if (fl & 0x800) {   // EftSurf_DrawPolyOtClipped: layer 0 diffuse; layer 1 specular when a vertex has fog above 0.01
                bool fogged = false;
                for (int c = 0; c < 3; c++) {
                    size_t vv = v + 36 * c;
                    float dx = b.f(vv) * K[0] - cam[0], dy = b.f(vv + 4) * K[1] - (hw ? wy : 0.0f), dz = b.f(vv + 8) * K[2] - cam[2];
                    float d2 = std::min(dx * dx + dy * dy + dz * dz, RP[14] * RP[14]);
                    fogged = fogged || 1.0f - d2 / (RP[14] * RP[14]) > 0.01f;
                }
                q.push_back({slot * 2, i, k, fade, 1});
                if (fogged) q.push_back({slot * 2 + 1, i, k, fade, 2});
                continue;
            }
            q.push_back({slot * 2 + layer, i, k, fade, 0});
        }
    }
    if (q.empty()) return;
    std::stable_sort(q.begin(), q.end(), [](const Tq& x, const Tq& y) { return x.key < y.key; });
    uint32_t out = scr + LY_TEXN, rtex = 0, rclut = 0;
    if (rfn) {   // EftSurf_RenderPalettes: the frame's indices with two lit palettes (diffuse from the light, specular from the view)
        size_t fr = rdat + rstep * (size_t)S.rframe;
        rtex = out; rclut = (out + rw * rh + 63) & ~63u;
        out = rclut + 2048;
        if (out < scr_end) {
            memcpy(psp_p(rtex), b.p + fr, (size_t)rw * rh);
            const float L[3] = {RP[8], RP[10], RP[9]};   // light in the surface frame (tangent x, bitangent z, normal y)
            float z[3] = {-fw[0], -fw[1], -fw[2]}, c[3] = {z[0], z[2], z[1]};
            uint32_t pa[256], pb[256];
            for (int j = 0; j < 256; j++) {
                const uint8_t* np = b.p + fr + (size_t)rw * rh + 12 * j;
                float n[3]; memcpy(n, np, 12);
                int v = (int)((L[0] * n[0] + L[1] * n[1] + L[2] * n[2]) * 128.0f);
                if (v < 0) v = 0;
                v = v / 2 + 0x20; if (v > 0x80) v = 0x80;
                pa[j] = ((uint32_t)RP[4] & 255) | ((uint32_t)RP[5] & 255) << 8 | ((uint32_t)RP[6] & 255) << 16 | (uint32_t)v << 24;
                float dd = 2.0f * (L[0] * n[0] + L[1] * n[1] + L[2] * n[2]), r[3] = {n[0] * dd - L[0], n[1] * dd - L[1], n[2] * dd - L[2]};
                float h[3]; for (int k = 0; k < 3; k++) h[k] = r[0] * vx[k] + r[1] * vy[k] + r[2] * fw[k];
                int sv = (int)((c[0] * h[0] + c[1] * h[1] + c[2] * h[2]) * (RP[12] * 255.0f));
                sv = std::max(0, std::min(sv, 255));
                pb[j] = (uint32_t)sv | (uint32_t)sv << 8 | (uint32_t)sv << 16 | 0x80u << 24;
            }
            memcpy(psp_p(rclut), pa, 1024); memcpy(psp_p(rclut + 1024), pb, 1024);
        } else rfn = 0;
    }
    uint32_t cap = scr_end > out ? (scr_end - out) / 72 : 0;
    std::vector<uint32_t> at(q.size(), 0);
    uint32_t used = 0;
    for (size_t r = 0; r < q.size() && used < cap; r++) {
        size_t e = mt + 64 * q[r].mesh, src = b.u(e + 8) + 108 * (size_t)q[r].tri;
        uint32_t fl = b.u(e + 36);
        const Ms& m = S.m[q[r].mesh];
        float al[3];
        uint8_t tmp[72];
        for (int k = 0; k < 3; k++) {
            size_t v = src + 36 * k;
            float s0 = b.f(v + 12), s1 = b.f(v + 16), a0 = b.f(v + 32), cr = b.f(v + 20), cg = b.f(v + 24), cl = b.f(v + 28);
            auto cb = [](float c, float k2) { long x = std::lround(c * k2); return (uint32_t)std::max(0L, std::min(255L, x)); };
            if (q[r].kind) {   // colour = colour A, alpha = A.a / 128 * vertex alpha (x fog on the specular pass)
                cr = RP[0]; cg = RP[1]; cl = RP[2]; a0 *= RP[3] / 128.0f;
                if (q[r].kind == 2) {
                    float dx = b.f(v) * K[0] - cam[0], dy = b.f(v + 4) * K[1] - (hw ? wy : 0.0f), dz = b.f(v + 8) * K[2] - cam[2];
                    float d2 = std::min(dx * dx + dy * dy + dz * dz, RP[14] * RP[14]);
                    a0 *= std::max(0.0f, std::min(1.0f, 1.0f - d2 / (RP[14] * RP[14])));
                }
            } else if (fl & 0x80) { s0 += (s1 < 0.5f ? 1.0f : -1.0f) * std::sin(m.u) * 0.05f; s1 += m.v; }
            else { s0 += m.u; s1 += m.v; a0 *= q[r].fade; }
            al[k] = q[r].kind && a0 <= 0.0f ? 0.0f : q[r].kind ? 1.0f : a0;
            uint32_t col = cb(cr, 1) | cb(cg, 1) << 8 | cb(cl, 1) << 16 | cb(a0, 255.0f / 128.0f) << 24;
            memcpy(tmp + 24 * k, &s0, 4); memcpy(tmp + 24 * k + 4, &s1, 4); memcpy(tmp + 24 * k + 8, &col, 4); memcpy(tmp + 24 * k + 12, b.p + v, 12);
        }
        if (al[0] < 0.1f && al[1] < 0.1f && al[2] < 0.1f) continue;
        uint32_t o = out + 72 * used++;
        memcpy(psp_p(o), tmp, 72);
        at[r] = o;
    }
    psp_gpu_flush_by(PC_VDEST);
    static GeKeep keep;
    keep.save();
    ly_map.load();
    ly_state();
    int cur_tex = -1, cur_layer = -1, drawn = 0;
    uint32_t run = 0, run_n = 0;
    auto flush_run = [&]() { if (run_n) { surf_now = 1; ly_prim(run, run_n * 3); surf_now = 0; drawn += (int)run_n; } run_n = 0; };
    for (size_t i = 0; i < q.size(); i++) {
        if (ptcl_next_key() < q[i].key) { flush_run(); ptcl_upto(q[i].key); cur_tex = cur_layer = -1; }   // stage particles share BT3's ordering table
        if (!at[i]) continue;
        int tx = q[i].kind ? 99 + q[i].kind : (int)b.u(mt + 64 * q[i].mesh), ly = q[i].key & 1;
        if (!q[i].kind && (tx < 0 || tx >= (int)nt)) continue;
        if (tx != cur_tex || ly != cur_layer || (run_n && at[i] != run + 72 * run_n)) {
            flush_run();
            if (tx != cur_tex) {
                if (tx >= 100) {   // the frame's 8-bit indices with the diffuse (100) or specular (101) palette
                    uint32_t cl = rclut + 1024 * (tx - 100);
                    int lw = 0, lh = 0; while ((1u << lw) < rw) lw++; while ((1u << lh) < rh) lh++;
                    psp_gpu_cmd(0xB0, cl & 0xFFFFFF); psp_gpu_cmd(0xB1, (cl >> 8) & 0x0F0000); psp_gpu_cmd(0xC4, 32);
                    psp_gpu_cmd(0xA0, rtex & 0xFFFFFF); psp_gpu_cmd(0xA8, ((rtex >> 8) & 0x0F0000) | rw); psp_gpu_cmd(0xB8, (uint32_t)(lw | lh << 8));
                } else ly_tex(b, b.u(32 + 8 * tx), b.u(36 + 8 * tx), scr + 1280 * tx);
                cur_tex = tx;
            }
            if (ly != cur_layer) { psp_gpu_cmd(0xDF, ly ? 0xA6 : 0x76); psp_gpu_cmd(0xE1, 0xFFFFFF); cur_layer = ly; }
        }
        if (!run_n) run = at[i];
        run_n++;
    }
    flush_run();
    ptcl_upto(0x7FFFFFFF);
    psp_gpu_flush_by(PC_VDEST);
    keep.load();
    static int logs = 0;
    if (logs < 3) { logs++; printf("agua: superficies BT3 dibujadas, %d triangulos de %u, camara (%.0f, %.0f, %.0f)\n", drawn, total, cam[0], cam[1], cam[2]); }
}
// BT3 stage particles (EftWeather, EftGeyser, EftSmoke, EftSteam), stepped per 30 Hz update and queued in BT3's ordering table (slot =
// screen z >> 8, layer 0 alpha blend, layer 1 additive) with the animated surfaces; drawn after the fighters without Z writes
struct PtV { float p[3], u, v; uint32_t c; };
struct PtItem { int key; uint32_t tex, n, at; int prim, aref; };
static std::vector<PtItem> ly_pq;
static size_t ly_pq_i = 0;
static uint32_t ly_pq_tex = 0;
static int ptcl_next_key() { return ly_pq_i < ly_pq.size() ? ly_pq[ly_pq_i].key : 0x7FFFFFFF; }
static const uint32_t PT_NOTEX = 0xFFFF;
static void ptcl_upto(int key) {
    int cur_tex = -1, cur_ly = -1, cur_at = -1, tme = 1;
    for (; ly_pq_i < ly_pq.size() && ly_pq[ly_pq_i].key < key; ly_pq_i++) {
        const PtItem& it = ly_pq[ly_pq_i];
        int t = it.tex == PT_NOTEX ? 0 : 1;
        if (t != tme) { psp_gpu_cmd(0x1E, (uint32_t)t); tme = t; }
        if (t && (int)it.tex != cur_tex) {
            uint32_t px = ly_pq_tex + 1280 * it.tex, cl = px + 256;
            psp_gpu_cmd(0xB0, cl & 0xFFFFFF); psp_gpu_cmd(0xB1, (cl >> 8) & 0x0F0000); psp_gpu_cmd(0xC4, 32);
            psp_gpu_cmd(0xA0, px & 0xFFFFFF); psp_gpu_cmd(0xA8, ((px >> 8) & 0x0F0000) | 16); psp_gpu_cmd(0xB8, 4 | 4 << 8);
            cur_tex = (int)it.tex;
        }
        int ly = it.key & 1;
        if (ly != cur_ly) { psp_gpu_cmd(0xDF, ly ? 0xA6 : 0x76); psp_gpu_cmd(0xE1, 0xFFFFFF); cur_ly = ly; }
        if (it.aref != cur_at) { cur_at = it.aref; psp_gpu_cmd(0x22, it.aref >= 0 ? 1 : 0); if (it.aref >= 0) psp_gpu_cmd(0xDB, 6 | (uint32_t)std::min(it.aref, 255) << 8 | 0xFF << 16); }
        psp_gpu_cmd(0x10, (it.at >> 8) & 0x0F0000); psp_gpu_cmd(0x01, it.at & 0xFFFFFF); psp_gpu_cmd(0x04, (uint32_t)it.prim << 16 | it.n);
    }
    if (cur_at >= 0) psp_gpu_cmd(0x22, 0);
    if (!tme) psp_gpu_cmd(0x1E, 1);
}
static bool ptcl_on() {
    static int n = 0; static bool on = true;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_BT3_PTCL"); on = !(e && *e == '0'); }
    return on;
}
struct PtRng { uint64_t s = 0x2545F4914F6CDD1DULL; int r() { s = s * 6364136223846793005ULL + 1; return (int)((s >> 32) & 0x7FFFFFFF); } float f() { return (float)r() / 2147483647.0f; } float u(float a, float b) { return a + (b - a) * f(); } };
#include "gpu/bt3_water.inc"
#include "gpu/bt3_bubble.inc"
static double wtr_zscr(const float* p, const float cam[3], const float fw[3]) {
    const double zn = 0.3, zf = 65536.0, z0 = 1.0, z1 = 10877000.0, cz = (-z1 * zn + z0 * zf) / (zf - zn), az = zf * zn * (z1 - z0) / (zf - zn);
    double d = (p[0] - cam[0]) * fw[0] + (p[1] - cam[1]) * fw[1] + (p[2] - cam[2]) * fw[2], z = d > zn ? cz + az / d : 16777215.0;
    return z == z ? std::min(std::max(z, 0.0), 16777215.0) : 16777215.0;
}
// fighters for EftWater (game thread, per logic frame): valid, feet xyz, body centre xyz
static std::mutex wt_mx;
static float wt_snap[4 * 7];
static int wt_n = 0;
static float bb_snap[2 * 62];
void psp_gpu_bubble_objs(const float* d) { std::lock_guard<std::mutex> g(wt_mx); memcpy(bb_snap, d, sizeof bb_snap); }
void psp_gpu_water_objs(const float* d, int n) { std::lock_guard<std::mutex> g(wt_mx); wt_n = std::min(n, 4); memcpy(wt_snap, d, sizeof(float) * 7 * wt_n); }
static std::vector<float> wt_bq;
static std::vector<float> wt_sq;
static std::atomic<uint32_t> wt_lev_bits{0x7FC00000u};
static std::atomic<int64_t> wt_lev_ms{0};
static int64_t wt_now_ms() { return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
float psp_gpu_wlevel() { if (wt_now_ms() - wt_lev_ms.load() > 500) return NAN; uint32_t u = wt_lev_bits.load(); float f; memcpy(&f, &u, 4); return f; }
static std::atomic<uint32_t> bb_lev_bits{0x7FC00000u};   // water level for the bubbles' objects only (TTT maps with a mod's bubbles: no BT3 water otherwise)
static std::atomic<int64_t> bb_lev_ms{0};
float psp_gpu_blevel() { if (wt_now_ms() - bb_lev_ms.load() > 500) return NAN; uint32_t u = bb_lev_bits.load(); float f; memcpy(&f, &u, 4); return f; }
void psp_gpu_water_sound(int id, const float* pos) { std::lock_guard<std::mutex> g(wt_mx); if (wt_sq.size() < 4 * 16) { wt_sq.push_back((float)id); wt_sq.insert(wt_sq.end(), pos, pos + 3); } }
static std::vector<float> wt_pc;
void psp_gpu_water_piece(const float* p) { std::lock_guard<std::mutex> g(wt_mx); if (wt_pc.size() < 3 * 64) wt_pc.insert(wt_pc.end(), p, p + 3); }
void psp_gpu_water_blasts(const float* e, int n) { std::lock_guard<std::mutex> g(wt_mx); if (wt_bq.size() >= 16 * 256) wt_bq.clear(); wt_bq.insert(wt_bq.end(), e, e + 16 * n); }
void psp_snd_bt3(int id, int vol, int pan);
// BtlCharSnd_CalcVolPan + Snd_PlaySeEx of bank 4 (near/far 200/1500); split screen = 0x40, centre
static void wtr_sound(int id, const float* pos, const float* cam, const float* fw) {
    int v = 0x40, p = 0;
    if (!psp_gpu_split2()) {
        float d[3] = {pos[0] - cam[0], pos[1] - cam[1], pos[2] - cam[2]}, dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), ang = 0.0f;
        if (!(std::fabs(d[0]) < 0.001f && std::fabs(d[2]) < 0.001f) && !(std::fabs(fw[0]) < 0.001f && std::fabs(fw[2]) < 0.001f)) {
            ang = std::atan2(d[0], d[2]) - std::atan2(fw[0], fw[2]);
            while (ang > WTR_PI) ang -= 2.0f * WTR_PI;
            while (ang < -WTR_PI) ang += 2.0f * WTR_PI;
        }
        v = (int)std::min(std::max((1.0f - (dist - 200.0f) / 1300.0f) * 128.0f, 0.0f), 128.0f);
        p = (int)std::min(std::max(std::sin(ang) * 64.0f, -64.0f), 64.0f);
    }
    psp_snd_bt3(id, v, p);
    static int nl = 0;
    if (nl < 8) { nl++; printf("agua: sonido BT3 0x%X volumen %d pan %d\n", id, v, p); fflush(stdout); }
}
// EftWater_UpdateBlast (eft_e.c:785) for one record of the TTT hit list (e: owner, U, pos, prev, radius, type, hit, hit pos, def kind, def class)
static int wtr_update_blast(WtrSt& st, PtRng& R, const float* e, float level, int sid, int* snd = nullptr, float* sp = nullptr) {
    if (sid == 12) level += 5.0f;
    float pos[3], prev[3], U = e[1], rad = e[8];
    int own = (int)e[0], type = (int)e[9];
    memcpy(pos, e + 2, 12); memcpy(prev, e + 5, 12);
    if (e[10] != 0) {
        float d[3] = {prev[0] - pos[0], prev[1] - pos[1], prev[2] - pos[2]};
        float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++) { d[k] = l > 0 ? d[k] / l : 0.0f; prev[k] = e[11 + k] + d[k] * rad; pos[k] = e[11 + k]; }
    }
    float dy = std::fabs(pos[1] - prev[1]);
    if (dy > 20.0f) dy = 20.0f;
    float scale = rad / 5.0f, size = (dy * 0.8f + (R.f() * 3.0f - 1.5f)) * scale;
    bool down = pos[1] >= level && prev[1] <= level, up = !down && pos[1] <= level && level <= prev[1];
    if (down || up) {
        float dd = -(pos[1] - prev[1]), s = (level - pos[1]) / dd, hit[3];
        for (int k = 0; k < 3; k++) hit[k] = pos[k] * (1.0f - s) + prev[k] * s;
        wse_event(hit, type == 0 ? std::max(0.5f, size / 4.0f) : std::max(0.8f, dy / 6.0f), 0);
        if (type == 0) { wtr_splash_for(st, R, 1, hit, size, U); if (snd) { *snd = down ? 0x3C : 0x3D; memcpy(sp, hit, 12); } return 1; }
        if (e[14] == -999.0f) return 0;
        int kd = (int)e[14];
        if ((uint32_t)(kd - 3) < 2u) {
            uint8_t& w = st.blastWait[own == 0 ? 0 : 1];
            if (w != 0) { w--; return 0; }
            wtr_splash_for(st, R, 2, hit, dy, U);
            w = (uint8_t)(int)e[15];
            return 1;
        }
        wtr_splash_for(st, R, 2, hit, dy, U);
        return 1;
    }
    if (type != 0 && (sid == 10 ? pos[1] > level : level - 6.0f < pos[1]) && pos[1] < level + 12.0f) { wtr_blast_trail(st, R, own, pos, prev, U, level); return 2; }
    return 0;
}
struct PtW { float p[30][3], v[30][3], a[30]; int wrap[30], tex[30]; float cp[3] = {0, 0, 0}; int moving = 0, cnt = 0; int64_t tick = -1; };
struct PtSmk { float pos[3], damp, size, speed, alpha; int life0, rate; uint8_t amb[4], dif[4]; struct P { float p[3], v[3]; int life; } q[40]; };
struct PtStm { float pos[3], speed, accel, size, pr, dr, sr, zr; int life0; uint8_t c[4]; bool paused, started; struct P { float p[3], v[3], ang, size; int life; } q[32]; };
struct PtGey { int state, delay, hold, wait; float alpha, step, by, tv; int smk, stm; };
struct PtWf { bool have = false, under = false, surf = false, wake = false; float feet[3], body[3]; };
struct PtRain { float p[64][3], prev[4][3], sk[4][3]; int64_t tick[4]; };
struct PtBolt { float p[3]; int life, lifeMax, ta, tb, wait, kind; };
struct PtSt { WtrSt wt; BubSt bb; PtWf wf[4]; PtRain rn; PtBolt bt[4]; bool init = false; PtRng rng; PtW w[4]; float wang = 0, wnow[3] = {0, 0, 0}; std::vector<PtGey> g; std::vector<PtSmk> sm; std::vector<PtStm> st; int64_t tick = -1; };
static void ptcl_smoke_step(PtSmk& m, PtRng& R) {
    if (m.rate > 0 && R.r() % m.rate == 0)
        for (auto& q : m.q)
            if (q.life <= 0) {
                float d[3] = {R.u(-50, 50), -100, R.u(-50, 50)}, l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (int k = 0; k < 3; k++) { q.p[k] = m.pos[k]; q.v[k] = d[k] / l * m.speed; }
                q.life = m.life0 + R.r() % 30;
                break;
            }
    for (auto& q : m.q) {
        if (q.life <= 0) continue;
        for (int k = 0; k < 3; k++) q.p[k] += q.v[k];
        q.v[0] *= m.damp; q.v[2] *= m.damp;
        q.life--;
    }
}
static void ptcl_steam_step(PtStm& m, PtRng& R, const float* cam) {
    if (m.started && !m.paused)
        for (auto& q : m.q)
            if (q.life <= 0) {
                float tc[3] = {cam[0] - m.pos[0], cam[1] - m.pos[1], cam[2] - m.pos[2]}, l = std::sqrt(tc[0] * tc[0] + tc[1] * tc[1] + tc[2] * tc[2]);
                if (l > 1e-6f) for (float& x : tc) x /= l;
                float sx = -tc[2], sz = tc[0];   // cross((0,-1,0), toCam)
                q.p[0] = m.pos[0] + sx * R.u(-m.pr, m.pr); q.p[1] = m.pos[1]; q.p[2] = m.pos[2] + sz * R.u(-m.pr, m.pr);
                float d[3] = {tc[0] * R.u(-m.dr, m.dr), -1, tc[2] * R.u(-m.dr, m.dr)}, dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                float sp = m.speed + R.u(-m.sr, m.sr);
                q.size = m.size + R.u(-m.zr, m.zr); q.life = m.life0 + (int)R.u(-5, 5); q.ang = R.u(0, 6.2831853f);
                for (int k = 0; k < 3; k++) q.v[k] = d[k] / dl * sp;
                break;
            }
    m.started = true;
    for (auto& q : m.q) {
        if (q.life < 0) continue;
        for (int k = 0; k < 3; k++) q.p[k] += q.v[k];
        q.v[1] += m.accel;
        q.life--;
    }
}
static int ptcl_build(const LyBlk& b, uint32_t base, uint32_t end) {
    ly_pq.clear(); ly_pq_i = 0;
    if (!b.ok(0, 64) || memcmp(b.p, "BPTL", 4) || !ptcl_on()) return 0;
    uint32_t nw = b.u(4), ne = b.u(8), tt = b.u(12), wo = b.u(16), ns = b.ok(64, 4) ? b.u(64) : 0;
    uint32_t gc = b.u(20), go = b.u(24), sc = b.u(28), so = b.u(32), tc = b.u(36), to = b.u(40);
    if (nw + ne + ns > 96 || !b.ok(tt, 8 * (size_t)(nw + ne + ns))) return 0;
    if (gc > 256 || sc > 256 || tc > 256 || (gc && !b.ok(go, 0x30 * (size_t)gc)) || (sc && !b.ok(so, 0x30 * (size_t)sc)) || (tc && !b.ok(to, 0x30 * (size_t)tc))) return 0;
    if (wo && !b.ok(wo, 0x18)) wo = 0;
    float cam[3], vx[3], vy[3], fw[3];
    if (!ly_camera(cam, vx, vy, fw)) return 0;
    static std::map<const void*, PtSt> sts;
    PtSt& S = sts[b.p];
    PtRng& R = S.rng;
    auto u8 = [&](size_t o) { return o < b.n ? b.p[o] : (uint8_t)0; };
    auto u16 = [&](size_t o) { return (int)(u8(o) | u8(o + 1) << 8); };
    const int view = split_now >= 1 && split_now <= 4 ? split_now - 1 : 0;   // split screen: own camera tracking per view, half the count (BT3)
    auto bolt_spawn = [&](int i) {   // EftStorm_SpawnBolt
        PtBolt& t = S.bt[i];
        float dist = (float)((R.r() & 0x3FF) - 0x1FF) + 5000.0f;
        float x = (float)((R.r() & 0x1FF) - 0xFF), z = (float)((R.r() & 0xFF) - 0x1F);
        if (x == 0 && z == 0) x = 1;
        float l = std::sqrt(x * x + z * z);
        t.p[0] = x / l * dist; t.p[1] = 0; t.p[2] = z / l * dist;
        t.lifeMax = t.life = ((R.r() & 0x1F) + 0x20) >> 1;
        t.ta = R.r() % 3;
        int r1 = R.r(), r2 = R.r() & 1;
        t.p[1] = -700.0f;
        t.tb = (r1 & 1) + r2 + 3;
        R.r();
        t.wait = 1;
        t.kind = dist < 5000.0f ? 1 : R.r() & 1;
        for (int j = 0; j < 4; j++) {
            if (j == i || !S.bt[j].life) continue;
            float d[3] = {S.bt[j].p[0] - t.p[0], S.bt[j].p[1] - t.p[1], S.bt[j].p[2] - t.p[2]};
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 490000.0f) { t.life = 0; break; }
        }
    };
    if (!S.init) {   // EftGeyser_MgrInit / Init (smoke and steam emitters per column), EftSmoke_Init (warm up), EftWeatherPtcl_Reset
        S.init = true;
        { std::lock_guard<std::mutex> g(wt_mx); wt_bq.clear(); wt_pc.clear(); }
        auto smoke = [&](const float* pos, size_t src) {
            PtSmk m; memset(&m, 0, sizeof m);
            memcpy(m.pos, pos, 12);
            m.damp = b.f(src + 0xC); m.size = b.f(src + 0x10); m.speed = b.f(src + 0x14); m.life0 = u16(src + 0x18); m.rate = u16(src + 0x1A);
            for (int k = 0; k < 4; k++) { m.amb[k] = u8(src + 0x1C + k); m.dif[k] = u8(src + 0x20 + k); }
            m.alpha = m.amb[3];
            return m;
        };
        for (uint32_t i = 0; i < sc; i++) { size_t r = so + 0x30 * i; float p[3] = {b.f(r), b.f(r + 4), b.f(r + 8)}; S.sm.push_back(smoke(p, r)); }
        for (uint32_t i = 0; i < gc; i++) {
            size_t r = go + 0x30 * i;
            float p[3] = {b.f(r), b.f(r + 4), b.f(r + 8)}, w = b.f(r + 12), h = b.f(r + 16);
            PtGey g{}; g.state = 0; g.delay = R.r() % 180; g.by = p[1] + h; g.smk = g.stm = -1;
            if (sc) {
                PtSmk m = smoke(p, so);
                m.speed = std::fabs(h / 150.0f + (R.f() * 5.0f - 2.5f)); m.rate = R.r() % 2 + 2; m.size = w / 10.0f * 2.0f; m.life0 = (int)(h / 150.0f + 90.0f); m.alpha = 64; m.damp = 0.985f;
                g.smk = (int)S.sm.size(); S.sm.push_back(m);
            }
            if (tc) {
                PtStm m; memset(&m, 0, sizeof m);
                memcpy(m.pos, p, 12);
                for (int k = 0; k < 4; k++) m.c[k] = u8(to + 0x1A + k);
                m.speed = h / 150.0f + 8.0f; m.accel = 9.8f / 30.0f; m.size = w / 10.0f * 4.8f; m.life0 = (int)(h / 150.0f + 25.0f);
                m.pr = 15; m.dr = 0.5f; m.sr = 2; m.zr = 0.5f; m.paused = true;
                for (auto& q : m.q) q.life = -1;
                g.stm = (int)S.st.size(); S.st.push_back(m);
            }
            S.g.push_back(g);
        }
        for (uint32_t i = 0; i < tc; i++) {
            size_t r = to + 0x30 * i;
            PtStm m; memset(&m, 0, sizeof m);
            m.pos[0] = b.f(r); m.pos[1] = b.f(r + 4); m.pos[2] = b.f(r + 8);
            m.speed = b.f(r + 0xC); m.accel = b.f(r + 0x10); m.size = b.f(r + 0x14); m.life0 = u16(r + 0x18);
            for (int k = 0; k < 4; k++) m.c[k] = u8(r + 0x1A + k);
            m.pr = 1; m.dr = 0.3f; m.sr = 1; m.zr = 0.5f;
            for (auto& q : m.q) q.life = -1;
            S.st.push_back(m);
        }
        for (uint32_t i = 0; i < sc; i++) { int n = R.r() % 30 + 60; for (int k = 0; k < n; k++) ptcl_smoke_step(S.sm[i], R); }
        for (auto& w : S.w)
            for (int i = 0; i < 30; i++) {
                for (int k = 0; k < 3; k++) { w.p[i][k] = (float)((R.r() & 0x3FF) - 0x1FF) * 0.1f; w.v[i][k] = 0; }
                w.a[i] = 0; w.wrap[i] = 0; w.tex[i] = nw ? R.r() % (int)nw : 0;
            }
        if (ns) {   // EftStorm_Init: rain around the origin, the 4 bolts spawned
            memset(&S.rn, 0, sizeof S.rn);
            for (auto& q : S.rn.p) for (float& c : q) c = (float)((R.r() & 0x3FF) - 0x1FF);
            for (auto& t : S.rn.tick) t = -1;
            memset(S.bt, 0, sizeof S.bt);
            for (int i = 0; i < 4; i++) bolt_spawn(i);
            printf("tormenta: BT3, %u imagenes, 64 gotas, 4 rayos\n", ns);
        }
    }
    int64_t now = (int64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 30.0);
    const bool tf = b.ok(68, 4) && (b.u(68) & 2);   // TTT map with a mod's effects: water from the TTT map, bubbles only (TTT draws its own splashes)
    bool wl = (tf ? psp_map_water != 0 : b.u(60) != 0) && ne >= 5;
    static int envn = 0; static bool stm_on = true, bub_on = true;
    if ((envn++ & 63) == 0) { const char* e1 = getenv("TTT_BT3_STORM"); stm_on = !(e1 && *e1 == '0'); const char* e2 = getenv("TTT_BT3_BUBBLES"); bub_on = !(e2 && *e2 == '0'); const char* e3 = getenv("TTT_RAIN"); rn_enh = !(e3 && *e3 == '0'); }
    bool bl = bub_on && wl && ne >= 6 && !(b.ok(68, 4) && (b.u(68) & 1));   // EftBubble_Init: entry 17 with 6 images or more; +68 bit 0 = off by the mod
    float wlev = tf ? psp_map_wlevel : b.f(56);
    int n_ev = 0;
    if (S.tick < 0 || now - S.tick > 8) S.tick = now - 1;
    for (; S.tick < now; S.tick++) {   // task updates (30 Hz)
        if (ns) {   // EftStorm_Update: bolt timers, rain falls 19.6 per update
            for (int i = 0; i < 4; i++) { PtBolt& t = S.bt[i]; if (t.wait) t.wait--; else if (t.life) t.life--; else bolt_spawn(i); }
            for (auto& q : S.rn.p) q[1] += 19.6f;
        }
        if (wo) {
            float th = S.wang * 3.14159265f / 180.0f;
            for (int k = 0; k < 3; k++) S.wnow[k] = b.f(wo + 4 * k);
            S.wnow[0] += std::sin(th) * 0.3f; S.wnow[2] += std::sin(th) * 0.3f;
            S.wang = (float)(((int)S.wang + (R.r() & 3)) % 360);
        }
        for (size_t i = 0; i < S.g.size(); i++) {   // EftGeyser_Update
            PtGey& g = S.g[i];
            size_t r = go + 0x30 * i;
            float a = u8(r + 0x21), h = b.f(r + 16), py = b.f(r + 4);
            if (g.state == 0 && --g.delay <= 0) g.state = 1;
            if (g.state == 1) {
                g.hold = u16(r + 0x18); g.wait = u16(r + 0x1C); g.alpha = 0; int fd = u16(r + 0x1A); g.step = fd ? a / (float)fd : a;
                if (g.stm >= 0) S.st[g.stm].paused = false;
                g.state = 2;
            }
            if (g.state == 2) { g.by = py; g.state = 3; }
            else if (g.state == 3) { g.alpha = std::min(a, g.alpha + g.step); if (--g.hold <= 0) { if (g.stm >= 0) S.st[g.stm].paused = true; g.state = 4; } }
            else if (g.state == 4) { g.alpha -= g.step; g.by += 2.0f; if (g.alpha <= 0) { g.alpha = 0; g.state = 5; } }
            else if (g.state == 5 && --g.wait <= 0) g.state = 1;
            g.tv -= b.f(r + 20);
            while (g.tv < 0) g.tv += 1.0f;
            while (g.tv > 1) g.tv -= 1.0f;
            (void)h;
        }
        for (auto& m : S.sm) ptcl_smoke_step(m, R);
        for (auto& m : S.st) ptcl_steam_step(m, R, cam);
        if (wl) {   // BtlFx_UpdateWaterFx per fighter (enter/leave splash, wake on fast moves through the surface), then EftWater's update
            float sn[4 * 7], bs[2 * 62]; int nn;
            { std::lock_guard<std::mutex> g(wt_mx); nn = wt_n; memcpy(sn, wt_snap, sizeof(float) * 7 * nn); memcpy(bs, bb_snap, sizeof bs); }
            int bo_of[4] = {-1, -1, -1, -1};
            for (int k = 0; k < 2; k++) { int j = (int)bs[62 * k] - 1; if (j >= 0 && j < nn && j < 4) bo_of[j] = k; }
            WtrObj ob[4]; memset(ob, 0, sizeof ob);
            for (int i = 0; i < 4; i++) {
                PtWf& f = S.wf[i];
                if (i >= nn || sn[7 * i] == 0) { if (f.wake) wtr_set_wake(S.wt, i, false); f = PtWf(); continue; }
                const float* fe = sn + 7 * i + 1, *bo = sn + 7 * i + 4;
                float v[3] = {0, 0, 0};
                if (f.have) for (int k = 0; k < 3; k++) v[k] = fe[k] - f.feet[k];
                float h = 2.0f * (fe[1] - bo[1]);
                if (!(h > 4.0f && h < 200.0f)) h = 20.0f;
                float U = h * 0.05f, sp = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                bool under = wlev < bo[1], surf = fe[1] - h < wlev && wlev < fe[1] + 1.0f;
                if (!tf && f.have && under != f.under) { float ps[3] = {fe[0], wlev, fe[2]}; wtr_splash_for(S.wt, R, 0, ps, v[1], U); n_ev++; wse_event(ps, std::fabs(v[1]) / 5.0f + 0.4f, 0); wtr_sound(under ? 0x3A : 0x3B, bo, cam, fw); }
                if (bl && f.have && under && !f.under && bo_of[i] >= 0) { bub_start_burst(S.bb, bo_of[i], sp); static int nl = 0; if (nl < 6) { nl++; printf("burbujas: rafaga BT3 del objeto %d (velocidad %.1f)\n", bo_of[i], sp); } }
                bool wk = surf && sp > 6.4814806f;
                if (!tf && wk != f.wake) wtr_set_wake(S.wt, i, wk);
                if (!tf && wk) { float ps[3] = {fe[0], wlev, fe[2]}; wse_event(ps, std::min(sp / 12.0f, 2.0f), 1, v[0], v[2]); }
                f.wake = wk; f.under = under; f.surf = surf; f.have = true;
                memcpy(f.feet, fe, 12); memcpy(f.body, bo, 12);
                ob[i].valid = true; memcpy(ob[i].pos, fe, 12); ob[i].speed = sp; ob[i].U = U;
                if (sp > 1e-6f) for (int k = 0; k < 3; k++) ob[i].dir[k] = v[k] / sp;
            }
            std::vector<float> bq, sq, pq;
            { std::lock_guard<std::mutex> g(wt_mx); bq.swap(wt_bq); sq.swap(wt_sq); pq.swap(wt_pc); }
            { uint32_t u; memcpy(&u, &wlev, 4); if (!tf) { wt_lev_bits = u; wt_lev_ms = wt_now_ms(); } bb_lev_bits = u; bb_lev_ms = wt_now_ms(); }
            if (tf) { sq.clear(); bq.clear(); pq.clear(); }
            for (size_t i = 0; i + 4 <= sq.size(); i += 4) wtr_sound((int)sq[i], &sq[i + 1], cam, fw);
            if (!psp_gpu_split2())
                for (size_t i = 0; i + 16 <= bq.size(); i += 16) {
                    int sd = 0; float spos[3];
                    int k = wtr_update_blast(S.wt, R, bq.data() + i, wlev, (int)(b.u(60) >> 8 & 0xFF) - 1, &sd, spos);
                    if (sd) wtr_sound(sd, spos, cam, fw);
                    static int nl = 0;
                    if (k && nl < 8) { nl++; printf("agua: %s BT3 de golpe (tipo %d) en (%.0f %.0f)\n", k == 1 ? "salpicadura" : "estela", (int)bq[i + 9], bq[i + 2], bq[i + 4]); fflush(stdout); }
                }
            if (!psp_gpu_split2()) for (size_t i = 0; i + 3 <= pq.size(); i += 3) { wtr_splash_at(S.wt, R, &pq[i]); wse_event(&pq[i], 1.0f, 0); }   // EftWater_AddSplashAt: nothing in split screen
            wtr_update(S.wt, R, wlev, ob, 4);
            if (bl) {   // EftBubble_Update: fighter bursts and body bubbles, camera bubbles, motion
                BubObj bo2[2];
                for (int k = 0; k < 2; k++) {
                    BubObj& o = bo2[k];
                    memset(&o, 0, sizeof o);
                    int j = (int)bs[62 * k] - 1;
                    if (j < 0 || j >= 4 || !ob[j].valid) continue;
                    o.valid = true; memcpy(o.pos, ob[j].pos, 12); memcpy(o.dir, ob[j].dir, 12); o.height = ob[j].U * 20.0f; o.inWater = S.wf[j].under;
                    o.nodesOk = bs[62 * k + 1] != 0;
                    for (int q = 0; q < 20; q++) memcpy(o.node[BB_IDS_G[q]], &bs[62 * k + 2 + 3 * q], 12);
                }
                const float cp[1][3] = {{cam[0], cam[1], cam[2]}}, cr[1][3] = {{vx[0], vx[1], vx[2]}}, cd[1][3] = {{vy[0], vy[1], vy[2]}}, cf[1][3] = {{fw[0], fw[1], fw[2]}};
                bub_update(S.bb, R, wlev, bo2, cp, cr, cd, cf, 1, false, split_now >= 1);
            }
        }
    }
    static const float K[3] = {16384.0f, 8192.0f, 16384.0f};
    const double zn = 0.3, zf = 65536.0, z0 = 1.0, z1 = 10877000.0, cz = (-z1 * zn + z0 * zf) / (zf - zn), az = zf * zn * (z1 - z0) / (zf - zn);
    auto depth = [&](const float* p) { return (p[0] - cam[0]) * fw[0] + (p[1] - cam[1]) * fw[1] + (p[2] - cam[2]) * fw[2]; };
    auto zscr = [&](double d) { double z = d > zn ? cz + az / d : 16777215.0; return z == z ? std::min(std::max(z, 0.0), 16777215.0) : 16777215.0; };
    auto cb = [](float c) { long x = std::lround(c); return (uint32_t)std::max(0L, std::min(255L, x)); };
    auto col = [&](float r, float g, float bl, float a) { return cb(r) | cb(g) << 8 | cb(bl) << 16 | cb(a * 255.0f / 128.0f) << 24; };
    uint32_t vat = base + 0x20000;
    for (uint32_t i = 0; i < nw + ne + ns; i++) {
        uint32_t px = b.u(tt + 8 * i), cl = b.u(tt + 8 * i + 4);
        if (b.ok(px, 256) && b.ok(cl, 1024)) { memcpy(psp_p(base + 1280 * i), b.p + px, 256); memcpy(psp_p(base + 1280 * i + 256), b.p + cl, 1024); }
    }
    ly_pq_tex = base;
    auto put = [&](const PtV* v, uint32_t n, uint32_t tex, int layer, int key_slot, int prim = 4, int aref = -1) {
        uint32_t sz = 24 * n;
        if (vat + sz > end || (tex >= nw + ne + ns && tex != PT_NOTEX)) return;
        for (uint32_t k = 0; k < n; k++) {
            float q[6] = {v[k].u, v[k].v, 0, v[k].p[0] / K[0], v[k].p[1] / K[1], v[k].p[2] / K[2]};
            memcpy(&q[2], &v[k].c, 4);
            memcpy(psp_p(vat + 24 * k), q, 24);
        }
        ly_pq.push_back({std::max(0, std::min(key_slot, 0xFFF)) * 2 + layer, tex, n, vat, prim, aref});
        vat += (sz + 63) & ~63u;
    };
    // screen-aligned sprite: half extents in BT3 units along the view's right / down axes; tr = transposed st (EftSpr_DrawFlat)
    auto sprite = [&](const float* c, float hx, float hy, uint32_t colr, uint32_t tex, int layer, bool tr) {
        double d = depth(c);
        if (d <= 1.0) return;
        PtV v[4];
        static const float SX[4] = {-1, -1, 1, 1}, SY[4] = {-1, 1, -1, 1};
        for (int k = 0; k < 4; k++) {
            for (int j = 0; j < 3; j++) v[k].p[j] = c[j] + vx[j] * hx * SX[k] + vy[j] * hy * SY[k];
            float s0 = SX[k] > 0 ? 1.0f : 0.0f, t0 = SY[k] > 0 ? 1.0f : 0.0f;
            v[k].u = tr ? t0 : s0; v[k].v = tr ? s0 : t0; v[k].c = colr;
        }
        put(v, 4, tex, layer, (int)(zscr(d) / 256.0));
    };
    int n_w = 0, n_g = 0, n_s = 0, n_t = 0;
    if (wo) {   // EftWeather_Move (per view, per update) and EftWeather_DrawPtcls
        PtW& W = S.w[view];
        float size = b.f(wo + 12);
        uint32_t pf = b.u(wo + 0x14) & 0xFFFF;
        int cnt = std::min((int)(b.u(wo + 0x14) >> 16), 30);
        if (split_now >= 1 && split_views > 1) cnt /= 2;
        if (W.tick < 0 || now - W.tick > 8) W.tick = now - 1;
        for (; W.tick < now; W.tick++) {
            float dc = std::sqrt((cam[0] - W.cp[0]) * (cam[0] - W.cp[0]) + (cam[1] - W.cp[1]) * (cam[1] - W.cp[1]) + (cam[2] - W.cp[2]) * (cam[2] - W.cp[2]));
            if (dc > 15) { W.moving = 2; W.cnt = 0; }
            else if (dc > 4) { W.moving = 1; W.cnt = std::min(W.cnt + 1, 15); }
            else if (W.cnt) { W.cnt--; W.moving = 1; }
            else W.moving = 0;
            memcpy(W.cp, cam, 12);
            for (int i = 0; i < cnt; i++) {
                float* p = W.p[i]; float* v = W.v[i];
                for (int k = 0; k < 3; k++) { v[k] += (float)((R.r() & 0x1FF) - 0xFF) / 255.0f * 0.02f; v[k] = std::max(-0.4f, std::min(0.4f, v[k])); }
                float q[3] = {p[0] + S.wnow[0] + v[0] - cam[0], p[1] + S.wnow[1] + v[1] - cam[1], p[2] + S.wnow[2] + v[2] - cam[2]};
                float x = q[0] * vx[0] + q[1] * vx[1] + q[2] * vx[2], y = q[0] * vy[0] + q[1] * vy[1] + q[2] * vy[2], z = q[0] * fw[0] + q[1] * fw[1] + q[2] * fw[2] - 55.0f;
                for (int it = 0; it < 66; it++) {
                    bool out = false;
                    if (x > 40) { x -= 80; out = true; } else if (x < -40) { x += 80; out = true; }
                    if (y > 40) { y -= 80; out = true; } else if (y < -40) { y += 80; out = true; }
                    if (z > 40) { z -= 80; out = true; } else if (z < -40) { z += 80; out = true; }
                    if (!out) break;
                    W.wrap[i] = 3;
                }
                if (W.moving == 2) W.wrap[i] = 0;
                z += 55.0f;
                for (int k = 0; k < 3; k++) p[k] = cam[k] + vx[k] * x + vy[k] * y + fw[k] * z;
                float hy = p[1];
                if ((pf & 2) && hy >= 0) hy = -150;
                hy = std::max(hy, -500.0f);
                float fy = 1 + hy / 500.0f;
                float t = std::sqrt((cam[0] - p[0]) * (cam[0] - p[0]) + (cam[1] - p[1]) * (cam[1] - p[1]) + (cam[2] - p[2]) * (cam[2] - p[2])) / 110.0f, a;
                if (t > 1) a = 0; else if (t < 0.5f) a = t <= 0.1f ? 0 : (t - 0.1f) * 2.5f; else a = 1 - (t - 0.5f) * 2;
                if (W.wrap[i]) { a *= (3 - W.wrap[i]) / 3.0f; W.wrap[i]--; }
                if (W.moving == 1 && W.cnt) a *= (30 - W.cnt) / 30.0f;
                a *= fy;
                if ((pf & 1) && p[1] + size + 4 > 0) a = p[1] + size < 0 ? a * -(p[1] + size) * 0.25f : 0;
                W.a[i] = a;
            }
        }
        for (int i = 0; i < cnt; i++) if (W.a[i] > 0) { sprite(W.p[i], size, size, col(128, 128, 128, W.a[i] * 128.0f), (uint32_t)W.tex[i], 0, false); n_w++; }
    }
    int n_r = 0, n_b = 0;
    if (ns && stm_on) {   // EftStorm_Draw: rain lines wrapped per view around 150 ahead of the camera, then the bolts (additive, camera-facing)
        PtRain& N = S.rn;
        int r0 = 0, r1 = 64;
        if (split_now >= 1 && split_views > 1) { r0 = view ? 0 : 32; r1 = r0 + 32; }
        float c[3] = {cam[0] + fw[0] * 150.0f, cam[1] + fw[1] * 150.0f, cam[2] + fw[2] * 150.0f};
        if (N.tick[view] != now) {
            N.tick[view] = now;
            for (int i = r0; i < r1; i++) {
                float* p = N.p[i];
                if (p[0] > c[0] + 100.0f) p[0] -= 200.0f; else if (p[0] < c[0] - 100.0f) p[0] += 200.0f;
                if (p[1] > c[1] + 100.0f) { p[0] = c[0] + (float)((R.r() & 0x3FF) - 0x1FF) * 0.25f; p[1] -= 200.0f; p[2] = c[2] + (float)((R.r() & 0x3FF) - 0x1FF) * 0.25f; }
                else if (p[1] < c[1] - 100.0f) { p[0] = c[0] + (float)((R.r() & 0x3FF) - 0x1FF) * 0.25f; p[1] += 200.0f; p[2] = c[2] + (float)((R.r() & 0x3FF) - 0x1FF) * 0.25f; }
                if (p[2] > c[2] + 100.0f) p[2] -= 200.0f; else if (p[2] < c[2] - 100.0f) p[2] += 200.0f;
            }
            float d[3] = {N.prev[view][0] - cam[0], 100.0f + N.prev[view][1] - cam[1], N.prev[view][2] - cam[2]}, l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int k = 0; k < 3; k++) N.sk[view][k] = l > 1e-6f ? d[k] / l * 20.0f : (k == 1 ? 20.0f : 0.0f);
            memcpy(N.prev[view], cam, 12);
        }
        std::vector<PtV> rl;
        rl.reserve(128);
        for (int i = r0; i < r1; i++) {
            PtV a, e;
            memcpy(a.p, N.p[i], 12);
            for (int k = 0; k < 3; k++) e.p[k] = a.p[k] + N.sk[view][k];
            if (depth(a.p) <= zn || depth(e.p) <= zn) continue;
            a.u = a.v = e.u = e.v = 0; a.c = e.c = 0x20606060u;
            rl.push_back(a); rl.push_back(e);
        }
        bool rne = rn_enh && !(split_now >= 1 && split_views > 1);
        if (rne) {   // Enhanced rain: the atmosphere pass draws it; BT3 lines only until it does
            rn_fno = toon_fno + 1;
            float fl = 0.0f;
            for (auto& t : S.bt) if (t.kind == 0 && t.life && !t.wait && t.lifeMax) fl = std::max(fl, (float)t.life / t.lifeMax);
            rn_flash = fl;
            for (int k = 0; k < 2; k++) rn_wind[k] = std::max(-0.4f, std::min(0.4f, S.wnow[k * 2] / 19.6f + (k ? 0.04f : 0.12f)));
        }
        if (rne && rn_drawn && toon_fno + 1 - rn_drawn <= 2u) rl.clear();
        if (!rl.empty()) { put(rl.data(), (uint32_t)rl.size(), PT_NOTEX, 0, 0, 1); n_r = (int)rl.size() / 2; }
        for (auto& t : S.bt) {
            if (!t.life || t.wait) continue;
            double d = depth(t.p);
            if (d <= zn) continue;
            uint32_t cl = col(128, 128, 128, (float)((t.life << 7) / t.lifeMax));
            auto quad = [&](float x0, float y0, float x1, float y1, int tex) {
                if (tex >= (int)ns) return;
                PtV v[4];
                static const int SX[4] = {0, 1, 0, 1}, SY[4] = {0, 0, 1, 1};
                for (int k = 0; k < 4; k++) {
                    float ox = (SX[k] ? x1 : x0) * 150.0f, oy = (SY[k] ? y1 : y0) * 150.0f;
                    for (int j = 0; j < 3; j++) v[k].p[j] = t.p[j] + vx[j] * ox + vy[j] * oy;
                    v[k].u = (float)SX[k]; v[k].v = (float)SY[k]; v[k].c = cl;
                }
                put(v, 4, nw + ne + (uint32_t)tex, 1, (int)(zscr(d) / 256.0));
                n_b++;
            };
            if (t.kind == 0) quad(-4.0f, -1.5f, 4.0f, 8.0f, t.ta);
            quad(-8.0f, -3.0f, 8.0f, 0.0f, t.tb);
        }
    }
    if (ne) {
        float L[3] = {b.f(44), b.f(48), b.f(52)}, ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (ll > 1e-6f) for (float& x : L) x /= ll;
        for (size_t i = 0; i < S.g.size(); i++) {   // EftGeyser_DrawColumn: 3 segments facing the camera, texture image 1 scrolled in t
            const PtGey& g = S.g[i];
            size_t r = go + 0x30 * i;
            if (g.alpha <= 0) continue;
            float w = b.f(r + 12), h = b.f(r + 16), pt[4][3], sd[4][3];
            for (int k = 0; k < 4; k++) { pt[k][0] = b.f(r); pt[k][1] = g.by - 0.25f * h * k; pt[k][2] = b.f(r + 8); }
            bool ok = true;
            for (int k = 0; k < 3 && ok; k++) {
                float a[3] = {pt[k + 1][0] - pt[k][0], pt[k + 1][1] - pt[k][1], pt[k + 1][2] - pt[k][2]}, c[3] = {pt[k][0] - cam[0], pt[k][1] - cam[1], pt[k][2] - cam[2]};
                float x[3] = {a[1] * c[2] - a[2] * c[1], a[2] * c[0] - a[0] * c[2], a[0] * c[1] - a[1] * c[0]}, l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                if (!(l > 1e-6f)) ok = false; else for (int j = 0; j < 3; j++) sd[k][j] = x[j] / l;
            }
            if (!ok) continue;
            memcpy(sd[3], sd[2], 12);
            PtV v[8];
            double zs = 0;
            for (int k = 0; k < 4; k++)
                for (int e = 0; e < 2; e++) {
                    PtV& q = v[2 * k + e];
                    for (int j = 0; j < 3; j++) q.p[j] = pt[k][j] + sd[k][j] * w * (e ? 1.0f : -1.0f);
                    q.u = (float)e; q.v = k / 3.0f + g.tv;
                    q.c = col(u8(r + 0x1E), u8(r + 0x1F), u8(r + 0x20), k == 3 ? 0.0f : g.alpha);
                }
            bool vis = true;
            for (int k = 0; k < 4; k++) { double d = depth(v[k].p); if (d <= zn) vis = false; zs += zscr(d); }
            if (!vis) continue;
            put(v, 8, nw + 1, 0, (int)(zs / 4.0 / 256.0));
            n_g++;
        }
        for (auto& m : S.sm) {   // EftSmoke_Draw: lit by the stage's object light, faded over the last 50 frames, EftSpr_DrawFlat
            float bs[3] = {m.pos[0], m.pos[1] - 50.0f, m.pos[2]};
            for (auto& q : m.q) {
                if (q.life <= 0) continue;
                float d[3] = {q.p[0] - bs[0], (q.p[1] - bs[1]) * 0.8f, q.p[2] - bs[2]}, l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                float dl = l > 1e-6f ? (d[0] * L[0] + d[1] * L[1] + d[2] * L[2]) / l : 0.0f;
                float c[3];
                for (int k = 0; k < 3; k++) c[k] = std::min(255.0f, std::max(0.0f, m.dif[k] * dl) + m.amb[k]);
                float a = m.alpha * (q.life < 50 ? q.life / 50.0f : 1.0f);
                sprite(q.p, 8.0f * m.size, 8.0f * m.size, col(c[0], c[1], c[2], a), nw + 2, 0, true);
                n_s++;
            }
        }
        for (auto& m : S.st)   // EftSteam_Draw: world quad of half 2 x size facing the camera with its roll, additive layer
            for (auto& q : m.q) {
                if (q.life < 0) continue;
                float f[3] = {cam[0] - q.p[0], cam[1] - q.p[1], cam[2] - q.p[2]}, fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                if (!(fl > 1e-6f)) continue;
                for (float& x : f) x /= fl;
                float rt[3] = {-f[2], 0, f[0]}, rl = std::sqrt(rt[0] * rt[0] + rt[2] * rt[2]);
                if (!(rl > 1e-6f)) continue;
                rt[0] /= rl; rt[2] /= rl;
                float up[3] = {rt[1] * f[2] - rt[2] * f[1], rt[2] * f[0] - rt[0] * f[2], rt[0] * f[1] - rt[1] * f[0]};
                float cs = std::cos(q.ang), sn = std::sin(q.ang), R0[3], R1[3];
                for (int k = 0; k < 3; k++) { R0[k] = cs * rt[k] + sn * up[k]; R1[k] = -sn * rt[k] + cs * up[k]; }
                float hs = 2.0f * q.size;
                PtV v[4];
                static const float SX[4] = {-1, -1, 1, 1}, SY[4] = {-1, 1, -1, 1};
                for (int k = 0; k < 4; k++) {
                    for (int j = 0; j < 3; j++) v[k].p[j] = q.p[j] + R0[j] * hs * SX[k] + R1[j] * hs * SY[k];
                    v[k].u = SX[k] > 0 ? 1.0f : 0.0f; v[k].v = SY[k] > 0 ? 1.0f : 0.0f; v[k].c = col(m.c[0], m.c[1], m.c[2], m.c[3]);
                }
                double d = depth(v[0].p);
                if (d <= zn) continue;
                put(v, 4, nw + 3, 1, (int)(zscr(d) / 256.0));
                n_t++;
            }
    }
    int n_wv = 0, n_bv = 0;
    if (wl) {   // EftWater_Draw: splashes, wakes, trails (entry 17 images 0..4)
        auto wemit = [&](const PtV* v, int n, int tex, int prim, int layer, double zt, int aref) {
            if (n <= 0 || tex < 0 || tex > 4) return;
            put(v, (uint32_t)n, nw + (uint32_t)tex, layer, (int)zt, prim, aref);
            n_wv += n;
        };
        bool wse = wat_enh && psp_map_wimm && !(split_now >= 1 && split_views > 1) && toon_fno - wat_enh_ok <= 2u;   // Enhanced splashes while the water shader runs
        if (!tf && wse) {
            int nr = 0; float rip[4 * WSR_N];
            wtr_draw_enh(S.wt, cam, vx, vy, fw, wlev, wemit, rip, nr, WSR_N);
            if (view == 0) { memcpy(wsr_q, rip, sizeof(float) * 4 * nr); for (int k = nr; k < WSR_N; k++) wsr_q[4 * k + 3] = 0.0f; wsr_f = toon_fno + 1; }
            static int sl = 0;
            if (nr && sl < 3) { sl++; printf("salpicaduras: Enhanced, %d ondas, la primera en (%.1f %.1f) radio %.1f, camara (%.1f %.1f)\n", nr, rip[0], rip[1], rip[2], cam[0], cam[2]); }
        } else if (!tf) wtr_draw(S.wt, cam, vx, vy, fw, wlev, wemit);
        if (bl)
            bub_draw(S.bb, cam, vx, vy, fw, wlev, view, [&](const PtV* v, int n, int tex, int prim, int layer, double zt, int aref) {
                if (n <= 0 || tex < 5 || tex >= (int)ne) return;
                put(v, (uint32_t)n, nw + (uint32_t)tex, layer, (int)zt, prim, aref);
                n_bv += n;
            });
        static int blogs = 0;
        if (n_bv && blogs < 4) { blogs++; printf("burbujas: BT3 en cola, %d vertices, %d vivas, camara %s el agua\n", n_bv, S.bb.count, cam[1] >= wlev ? "bajo" : "sobre"); }
        static int wlogs = 0;
        if (n_ev && wlogs < 8) { wlogs++; printf("agua: salpicadura BT3 al cruzar el agua (nivel %.1f), camara (%.0f %.0f %.0f)\n", wlev, cam[0], cam[1], cam[2]); }
        static int ulogs = 0;
        if (ulogs < 2 && S.wf[0].have) { ulogs++; printf("agua: luchador 1 pies (%.1f %.1f %.1f) cuerpo (%.1f %.1f %.1f), nivel %.1f, camara (%.0f %.0f %.0f)\n", S.wf[0].feet[0], S.wf[0].feet[1], S.wf[0].feet[2], S.wf[0].body[0], S.wf[0].body[1], S.wf[0].body[2], wlev, cam[0], cam[1], cam[2]); }
    }
    std::stable_sort(ly_pq.begin(), ly_pq.end(), [](const PtItem& a, const PtItem& c) { return a.key < c.key; });
    static int logs = 0;
    if (logs < 3 && !ly_pq.empty()) { logs++; printf("particulas: BT3 en cola, clima %d, geiseres %d, humo %d, vapor %d, agua %d vertices, lluvia %d, rayos %d\n", n_w, n_g, n_s, n_t, n_wv, n_r, n_b); }
    return (int)ly_pq.size();
}
extern int psp_map_fx_tok, psp_map_water;
extern float psp_map_wlevel;
// map state for a TTT map: the GE state at F5 with a world matrix W = K x camera (the game camera is PS2 eye space; the GE view at F5 is its flip)
static bool ttt_cam_state() {
    static const float K[3] = {16384.0f, 8192.0f, 16384.0f};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (psp_cam_m[4 * i + j] != psp_cam_m[4 * i + j]) return false;
    ly_map.save();
    for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) ly_map.w[3 * i + j] = psp_cam_m[4 * i + j] * (i < 3 ? K[i] : 1.0f);
    return true;
}
static uint64_t f5_p[4], f5_tex; static size_t f5_n;
static void layer_after_objects() {   // F5: after the fighters (BT3: BtlObjDraw_Draw, then the effect tasks and Ot_Draw)
    const bool cpe = cp_on();
    uint64_t f0 = cpe ? perf_clock() : 0, f1 = f0;
    if (cpe) { memset(f5_p, 0, sizeof f5_p); f5_n = 0; f5_tex = perf_count[PC_TEXSUB]; }
    auto ft = [&](int k) { if (cpe) { uint64_t t = perf_clock(); f5_p[k] += t - f1; f1 = t; } };
    auto cur = ly_cur;
    ly_cur = nullptr;
    uint32_t scr = ly_scr, n = ly_scr_n;
    static int logs = 0;
    if (!cur && psp_map_fx_tok > 0 && psp_cam_m[16] > 0 && split_now < 1) {   // TTT map with a mod's effects: layers by token, map state = GE at F5 with the game camera
        std::shared_ptr<const std::vector<uint8_t>> t;
        { std::lock_guard<std::mutex> lk(ly_mx); auto it = ly_tab.find((uint32_t)psp_map_fx_tok); if (it != ly_tab.end()) t = it->second; }
        if (t && ttt_cam_state()) { cur = t; ly_frame = frames_drawn; }
        static int tl = 0;
        if (tl < 3) { tl++; printf("tormenta: mapa TTT con efectos del mod, capas %s, camara del juego %s\n", t ? "si" : "no", cur ? "ok" : "fallo"); }
    }
    int why = !cur ? 1 : frames_drawn - ly_frame > 2 ? 2 : !scr || n <= LY_OBJ + 0x40000 ? 3 : 0;
    if (logs < 6) { logs++; printf("agua: marca F5 en el GPU, cuadros desde el mapa %llu, motivo %d\n", (unsigned long long)(frames_drawn - ly_frame), why); }
    {
        static int dl = 0;
        if (cur && !psp_map_fx_tok && dl < 3 && psp_cam_m[16] > 0 && (frames_drawn & 63) == 0) {
            dl++;
            GeKeep sv = ly_map;
            float c0[3], x0[3], y0[3], f0[3], c1[3], x1[3], y1[3], f1[3];
            bool a = ly_camera(c0, x0, y0, f0);
            bool bk = ttt_cam_state() && ly_camera(c1, x1, y1, f1);
            ly_map = sv;
            printf("camara: mapa BT3 (%.1f %.1f %.1f) adelante (%.2f %.2f %.2f), camara del juego (%.1f %.1f %.1f) adelante (%.2f %.2f %.2f) %s\n", a ? c0[0] : 0, a ? c0[1] : 0, a ? c0[2] : 0, a ? f0[0] : 0, a ? f0[1] : 0, a ? f0[2] : 0,
                   bk ? c1[0] : 0, bk ? c1[1] : 0, bk ? c1[2] : 0, bk ? f1[0] : 0, bk ? f1[1] : 0, bk ? f1[2] : 0, a && bk ? "" : "(fallo)");
        }
    }
    if (why) return;
    ft(3);
    if (ly_obj_g2 && !ly_obj_done) { ly_obj_done = true; ly_cur = cur; obj_draw("en la marca F5"); ly_cur = nullptr; }
    ft(0);
    LyBlk pb = ly_block(*cur, 5);
    ly_pq.clear(); ly_pq_i = 0;
    if (pb.p) ptcl_build(pb, scr + LY_OBJ, scr + n);
    if (cpe) f5_n = ly_pq.size();
    ft(1);
    LyBlk b = ly_block(*cur, 2);
    if (b.p && surf_on()) surf_draw(b, scr + LY_SURF, scr + LY_OBJ);
    ft(2);
    if (ly_pq_i < ly_pq.size()) {   // particles left (no surfaces, or surfaces not drawn)
        psp_gpu_flush_by(PC_VDEST);
        static GeKeep keep;
        keep.save();
        ly_map.load();
        ly_state();
        ptcl_upto(0x7FFFFFFF);
        psp_gpu_flush_by(PC_VDEST);
        keep.load();
    }
    ly_pq.clear(); ly_pq_i = 0;
    ft(1);
}

static void psp_gpu_cmd_(int c, uint32_t arg);
void psp_gpu_cmd(int c, uint32_t arg) {
    if (!cp_on()) { psp_gpu_cmd_(c, arg); return; }
    uint64_t t0 = perf_clock();
    psp_gpu_cmd_(c, arg);
    uint64_t dt = perf_clock() - t0;
    uint64_t* o = cp_op[c & 255]; o[0] += dt; o[1]++;
    if (c == 0) { uint64_t* m = cp_mk[(arg >> 16) & 255]; m[0] += dt; m[1]++; }
    if (c == 0 && dt > 6000000) {   // a marker that took over ~2 ms: which one and what it did (TTT_CPU_PROBE)
        static int logs = 0;
        if (logs++ < 30) { printf("marca: cuadro %llu, 0x%02X tardo %.1f M ciclos | F5: objetos %.2f, particulas %.2f (%zu), superficies %.2f, resto %.2f | %llu subidas de textura\n", (unsigned long long)frames_drawn, arg >> 16, dt / 1e6, f5_p[0] / 1e6, f5_p[1] / 1e6, f5_n, f5_p[2] / 1e6, f5_p[3] / 1e6, (unsigned long long)(perf_count[PC_TEXSUB] - f5_tex)); fflush(stdout); }
    }
}
static void psp_gpu_cmd_(int c, uint32_t arg) {
    static bool init = false;
    tc_gen++;
    if ((unsigned)c < 256u && R[c] != arg) switch (c) { case 0x01: case 0x02: case 0x04: case 0x2A: case 0x2B: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: break; default: tc_rgen++; }
    if (!init) { init = true; R[0x48] = R[0x49] = 0x3F8000; R[0xD7] = 0xFFFF; tc_rgen++; }
    switch (c) {
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: case 0xD2:   // target change
    case 0xEA:                                               // block transfer
        if (R[c] != arg) psp_gpu_flush_by(PC_VDEST);
        if (c == 0xEA) tsm_epoch++;
        R[c] = arg; return;
    case 0x00: {   // port markers around each fighter draw (cel_wrap): aura colour, intensity, fighter
        uint32_t tag = arg >> 16;
        if ((tag >= 0xF0 && tag <= 0xF2) || (tag >= 0xF5 && tag <= 0xF7)) { layer_mark(arg); return; }
        if (tag == 0xE1) toon_rg = arg & 0xFFFF;
        else if (tag == 0xE2) toon_tint = (toon_rg >> 8) | (toon_rg & 0xFF) << 8 | ((arg >> 8) & 0xFF) << 16 | (arg & 0xFF) << 24;
        else if (tag == 0xE3) {
            toon_who = (int)(arg & 0xFF);
            toon_dmg1 = (arg >> 9) & 1;
            toon_eyes = (arg >> 10) & 1;
            toon_only_nrm = (arg >> 11) & 1; toon_none = (arg >> 13) & 1;
            toon_force = (arg >> 12) & 1;
            if (!((arg >> 8) & 1)) toon_tint &= 0xFEFFFFFFu; else toon_tint |= 0x01000000u;
            toon_epoch++;
            static int logs = 0;
            if (logs < 4) { logs++; printf("cel: marca de luchador %d en la lista, tinte 0x%08X\n", toon_who - 1, toon_tint); }
        }
        else if (tag == 0xE7) { pnl_x0 = (int)(arg & 0xFF) * 2; pnl_x1 = (int)((arg >> 8) & 0xFF) * 2; }
        else if (tag == 0xE8) {
            pnl_y0 = (int)(arg & 0xFF) * 2; pnl_y1 = (int)((arg >> 8) & 0xFF) * 2;
            float w = (float)std::max(2, pnl_x1 - pnl_x0), h = (float)std::max(2, pnl_y1 - pnl_y0);
            pnl_s = std::max(w / 480.0f, h / 272.0f);
            pnl_ox = ((pnl_x0 + pnl_x1) * 0.5f - 240.0f) / 240.0f; pnl_dy = (136.0f - (pnl_y0 + pnl_y1) * 0.5f) / 136.0f;
            pnl_on = true;
            pnl_face[0] = pnl_face[1] = 1e9f; pnl_face[2] = pnl_face[3] = -1e9f;
            memcpy(pnl_R, R, sizeof R);
            static int logs = 0;
            if (logs++ < 2) printf("gpu: panel 3D (%d, %d)-(%d, %d), escala %.3f\n", pnl_x0, pnl_y0, pnl_x1, pnl_y1, pnl_s);
            if (m3_mode) { psp_gpu_flush_by(PC_VDEST); int ke = scale_int(); m3_r[0] = pnl_x0; m3_r[1] = pnl_y0; m3_r[2] = pnl_x1; m3_r[3] = pnl_y1; glr_m3d_copy(0, pnl_x0 * ke, pnl_y0 * ke, pnl_x1 * ke, pnl_y1 * ke); }
        }
        else if (tag == 0xEE) {   // game room: model k into its row's render (clipped to the drawn part), hidden when that render was not drawn this frame
            psp_gpu_flush_by(PC_VDEST);
            int k = (int)(arg & 3);
            bool hid = (arg & 0x10) != 0;
            std::lock_guard<std::mutex> g(rm_mx);
            memcpy(pnl_R, R, sizeof R);
            pnl_on = true; pnl_s = 1.0f; pnl_ox = 0.0f; pnl_dy = 0.0f;
            pnl_face[0] = pnl_face[1] = 1e9f; pnl_face[2] = pnl_face[3] = -1e9f;
            const RoomQ& q = rm_at[k];
            bool ok = !hid && rm_s[k].live && rm_atf[k] == rm_frame && q.u1 > q.u0 && q.v1 > q.v0;
            static bool cfg = false;
            if (!cfg) { cfg = true; if (const char* e = getenv("TTT_ROOM3D_POS")) sscanf(e, "%f,%f", &rm_pos[0], &rm_pos[1]); }
            rm_k = k; rm_fit = ok; rm_show = ok; rm_zoom = false; rm_done = false; pnl_hide = !ok;
            pnl_x0 = 0; pnl_x1 = ok ? 480 : 2; pnl_y0 = ok ? (int)std::floor(q.cy0) : 0; pnl_y1 = ok ? (int)std::ceil(q.cy1) : 2;   // the whole width of the screen, the height of the row
            if (ok) {
                float sx = (q.x1 - q.x0) / (q.u1 - q.u0), sy = (q.y1 - q.y0) / (q.v1 - q.v0);
                rm_full[0] = q.x0 - q.u0 * sx; rm_full[1] = q.y0 - q.v0 * sy; rm_full[2] = q.tw * sx; rm_full[3] = q.th * sy;
                rm_tgt[0] = (q.x0 + q.x1) * 0.5f + rm_pos[0]; rm_tgt[1] = (q.y0 + q.y1) * 0.5f + rm_pos[1];
            }
        }
        else if (tag == 0xE9) {
            rm_fit = false; rm_zoom = false; rm_show = false; pnl_face_age++; if (pnl_on && pnl_face[2] - pnl_face[0] >= 8.0f && pnl_face[3] - pnl_face[1] >= 8.0f) { memcpy(pnl_face_last, pnl_face, sizeof pnl_face); pnl_face_age = 0; } if (pnl_on) { psp_gpu_flush_by(PC_VDEST); memcpy(R, pnl_R, sizeof R); }
            if (pnl_on && m3_mode) {   // the 3D of this frame kept apart, the screen as it was before it
                int ke = scale_int();
                glr_m3d_copy(1, m3_r[0] * ke, m3_r[1] * ke, m3_r[2] * ke, m3_r[3] * ke);
                glr_m3d_paste(0, m3_r[0] * ke, m3_r[1] * ke, m3_r[2] * ke, m3_r[3] * ke);
                m3_have = true;
            }
            m3_mode = false;
            pnl_on = false; pnl_hide = false; }
        else if (tag == 0xEA) { psp_gpu_flush_by(PC_VDEST); pnl_hide = (arg & 1) != 0; }
        else if (tag == 0xF4) { m3_mode = (arg & 1) != 0; if (!(arg & 1)) m3_have = false; }
        else if (tag == 0xF3) {   // the page: last frame's 3D where the map's render goes
            psp_gpu_flush_by(PC_VDEST);
            int ke = scale_int();
            static int logs = 0;
            bool ok = m3_have && glr_m3d_paste(1, m3_r[0] * ke, m3_r[1] * ke, m3_r[2] * ke, m3_r[3] * ke);
            if (logs++ < 2) printf("gpu: mapa 3D pegado en la pagina: %s (%d, %d)-(%d, %d)\n", ok ? "si" : "no", m3_r[0], m3_r[1], m3_r[2], m3_r[3]);
        }
        else if (tag == 0xF9) sz_dbg = (int)(arg & 0xFF);
        else if (tag == 0xFB) lt_drop = (arg & 1) != 0;   // technique list (charge_list.inc): the AGS 280 draw of the game is discarded, the port draws the rows
        else if (tag == 0xFA) bf_side = (arg & 1) ? (int)((arg >> 1) & 3) : -1;   // BT3 face (bt3_hud.inc): the game's portrait of bar N is captured, not drawn
        else if (tag == 0xF8) {   // port-drawn BT3 HUD (bt3_hud.inc): GE state kept before its sprites (arg 1) and given back after them (arg 0)
            static uint32_t hR[256], hv[4]; static bool hset = false;
            psp_gpu_flush_by(PC_VDEST);
            port_hud = (arg & 1) != 0; port_both = (arg & 2) != 0;
            if (arg & 1) { memcpy(hR, R, sizeof R); hv[0] = v_addr; hv[1] = i_addr; hv[2] = base; hv[3] = offset_addr; hset = true; }
            else if (hset) { memcpy(R, hR, sizeof R); tc_rgen++; v_addr = hv[0]; i_addr = hv[1]; base = hv[2]; offset_addr = hv[3]; load_clut(); hset = false; }
        }
        else if (tag == 0xEF) {   // bars behind fighters: GE state kept before the bars' packet (arg 1) and given back after it (arg 0)
            static uint32_t kR[256], kv[5]; static float kw[12], kvw[12], kp[16], kt[12], kd[20], kc[2]; static int ki[4]; static bool kset = false;
            psp_gpu_flush_by(PC_VDEST);
            ef_now = arg & 1;
            glr_bars_back((arg & 1) != 0);
            glr_alpha_keep((arg & 1) != 0);   // the bars use the alpha as their fill mask; the outline pass reads it after them
            if (arg & 1) {
                memcpy(kR, R, sizeof R); memcpy(kw, m_world, sizeof kw); memcpy(kvw, m_view, sizeof kvw); memcpy(kp, m_proj, sizeof kp); memcpy(kt, m_tgen, sizeof kt);
                ki[0] = i_world; ki[1] = i_view; ki[2] = i_proj; ki[3] = i_tgen; kv[0] = v_addr; kv[1] = i_addr; kv[2] = base; kv[3] = offset_addr; kv[4] = view_hash; kset = true;
                memcpy(kd, view_up, 12); memcpy(kd + 3, view_inv, 48); memcpy(kd + 15, proj_eye, 12); kc[0] = psp_cam_m[17]; kc[1] = psp_cam_m[18];
            } else if (kset) {
                memcpy(R, kR, sizeof R); tc_rgen++; memcpy(m_world, kw, sizeof kw); memcpy(m_view, kvw, sizeof kvw); memcpy(m_proj, kp, sizeof kp); memcpy(m_tgen, kt, sizeof kt);
                i_world = ki[0]; i_view = ki[1]; i_proj = ki[2]; i_tgen = ki[3]; v_addr = kv[0]; i_addr = kv[1]; base = kv[2]; offset_addr = kv[3]; view_hash = kv[4];
                memcpy(view_up, kd, 12); memcpy(view_inv, kd + 3, 48); memcpy(proj_eye, kd + 15, 12); psp_cam_m[17] = kc[0]; psp_cam_m[18] = kc[1];
                load_clut(); kset = false;
            }
        }
        else if (tag == 0xED) {
            static int logs = 0;
            if (logs < 40 && (toon_st_hint || toon_st_scan)) { logs++; printf("cel: modelo anterior, %u partes por puntero, %u busquedas en memoria (%.2f ms)\n", toon_st_hint, toon_st_scan, toon_st_us / 1000.0); }
            toon_st_hint = toon_st_scan = 0; toon_st_us = 0;
            toon_forget(); toon_hn = 0;
        }
        else if (tag == 0xEB) {
            psp_gpu_flush_by(PC_VDEST); cap_dir = fb().dir; cap_on = glr_enabled(); cap_fresh = true; cap_zdone = false; cap_pt_done = false; cap_zoom = false;
            cap_map = (arg & 1) != 0;
            if (cap_dg) { cap_dg_n = 0; cap_dg_post = -1; printf("render: diag inicio de la captura, pantalla %08X, GPU %d\n", cap_dir, (int)cap_on); }
            else cap_dg_post = -1;
            if (cap_map) { float sh = 480.0f * 152.0f / 300.0f; cap_sx0 = 0; cap_sx1 = 480; cap_sy0 = (272.0f - sh) * 0.5f; cap_sy1 = cap_sy0 + sh; }
        }
        else if (tag == 0xEC) { if (cap_dg) { printf("render: diag fin de la captura (%d primitivas registradas)\n", cap_dg_n); fflush(stdout); cap_dg_post = 0; } if (cap_on) { psp_gpu_flush_by(PC_VDEST); float ke = (float)scale_int(); bool fok = cap_face[2] - cap_face[0] >= 8.0f && cap_face[3] - cap_face[1] >= 8.0f; if (!fok && pnl_face_age <= 3 && pnl_face_last[2] - pnl_face_last[0] >= 8.0f) { memcpy(cap_face, pnl_face_last, sizeof cap_face); fok = true; } for (int k = 0; k < 4; k++) cap_out_face[k] = fok ? ((k & 1 ? cap_face[k] - cap_ry : cap_face[k] - cap_rx) * cap_kz * ke) : -1.0f; glr_cap_read(); cap_zoom = false; } else { std::lock_guard<std::mutex> g(cap_mx); cap_out.clear(); cap_done_tag = cap_tag; cap_ready = true; } cap_on = false; cap_map = false; }
        else if (tag == 0xE4) { toon_tint = 0; toon_who = 0; toon_aura = 0; toon_dmg1 = false; toon_eyes = false; toon_only_nrm = false; toon_none = false; toon_force = false; }
        else if (tag == 0xE5 || tag == 0xE6) { toon_aura = tag == 0xE5; static int al = 0; if (al < 2 && toon_aura) { al++; printf("cel: marca de aura en la lista\n"); } }   // a fighter's aura (wrapper on 0x088B5CD0): no effects light
        return;
    }
    case 0x01: v_addr = rel(arg); return;
    case 0x02: i_addr = rel(arg); return;
    case 0x04: primitive(arg); return;
    case 0x13: offset_addr = arg << 8; return;
    case 0xFB: {
        int p = (int)(arg & 15), hv = p >= 5 && p <= 12 ? ((p - 5) & 3) + 1 : 0, v = hv ? hv : (p <= 4 ? p : 0);
        psp_gpu_split_time(1); split_now = 0; split_clip = false;
        bool real = split_pass && !split_hud;
        if (real && (hv || split_pass != v)) alt_save(split_pass);
        if (real && (hv || !v)) { split_pass = 0; split_divider(); }
        split_pass = v; split_hud = hv ? (p >= 9 ? 2 : 1) : 0;
        if (v && !hv && !split_first) split_first = v;
        if (v) { split_params(v); if (!hv) alt_paste(); }
        return;
    }
    case 0xFC: alt_mask = (int)(arg & 0x1F); return;
    case 0xFD: hud_code = arg & 0x100 ? (int)(arg & 0xFF) : 0; hud_flip = false; if (hud_code) hud_setup(); return;
    case 0xFE: hud_bb[0] = (float)(int)(arg & 0xFFF) - 1024.0f; hud_bb[1] = (float)(int)((arg >> 12) & 0xFFF) - 1024.0f; return;
    case 0xFF: hud_bb[2] = (float)(int)(arg & 0xFFF) - 1024.0f; hud_bb[3] = (float)(int)((arg >> 12) & 0xFFF) - 1024.0f; return;
    case 0x2A: i_bone = arg & 0x7F; return;
    case 0x2B: if (i_bone < 96) { m_bone[i_bone / 12][i_bone % 12] = f24(arg); i_bone++; } return;
    case 0x3A: i_world = arg & 15; return;
    case 0x3B: if (i_world < 12) m_world[i_world++] = f24(arg); return;
    case 0x3C: i_view = arg & 15; return;
    case 0x3D:
        if (i_view < 12) m_view[i_view++] = f24(arg);
        if (i_view == 12) {
            uint32_t h = 2166136261u; for (int i = 0; i < 12; i++) { uint32_t u; memcpy(&u, &m_view[i], 4); h = (h ^ u) * 16777619u; }
            view_hash = h;
            float l = std::sqrt(m_view[3] * m_view[3] + m_view[4] * m_view[4] + m_view[5] * m_view[5]);
            if (l > 1e-6f) { view_up[0] = m_view[3] / l; view_up[1] = m_view[4] / l; view_up[2] = m_view[5] / l; }
            const float* m = m_view;   // row vectors: o = p R + t, R[i][j] = m[3i+j]
            float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
            if (std::fabs(det) > 1e-12f) {
                float* r = view_inv;
                r[0] = (m[4] * m[8] - m[5] * m[7]) / det; r[1] = -(m[1] * m[8] - m[2] * m[7]) / det; r[2] = (m[1] * m[5] - m[2] * m[4]) / det;
                r[3] = -(m[3] * m[8] - m[5] * m[6]) / det; r[4] = (m[0] * m[8] - m[2] * m[6]) / det; r[5] = -(m[0] * m[5] - m[2] * m[3]) / det;
                r[6] = (m[3] * m[7] - m[4] * m[6]) / det; r[7] = -(m[0] * m[7] - m[1] * m[6]) / det; r[8] = (m[0] * m[4] - m[1] * m[3]) / det;
                r[9] = m[9]; r[10] = m[10]; r[11] = m[11];
            }
        }
        return;
    case 0x3E: i_proj = arg & 15; return;
    case 0x3F:
        if (i_proj < 16) m_proj[i_proj++] = f24(arg);
        if (i_proj == 16 && m_proj[11] < -0.5f && m_proj[0] > 0.0f) { psp_cam_m[17] = m_proj[0]; psp_cam_m[18] = m_proj[5]; }
        if (i_proj == 16) {
            const float* m = m_proj;
            float a[3][4];
            const int c[3] = {0, 1, 3};
            for (int r = 0; r < 3; r++) { a[r][0] = m[c[r]]; a[r][1] = m[4 + c[r]]; a[r][2] = m[8 + c[r]]; a[r][3] = -m[12 + c[r]]; }
            float d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
            if (std::fabs(d) > 1e-12f)
                for (int k = 0; k < 3; k++) {
                    float b[3][3];
                    for (int r = 0; r < 3; r++) for (int j = 0; j < 3; j++) b[r][j] = j == k ? a[r][3] : a[r][j];
                    proj_eye[k] = (b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1]) - b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0]) + b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0])) / d;
                }
        }
        return;
    case 0x40: i_tgen = arg & 15; return;
    case 0x41: if (i_tgen < 12) m_tgen[i_tgen++] = f24(arg); return;
    case 0xC4: {
        // the palette may come from VRAM the queue has not painted yet
        uint32_t cd = (R[0xB0] & 0xFFFFFF) | ((R[0xB1] & 0x0F0000) << 8);
        if (cload_flush() && in_vram_addr(cd)) psp_gpu_flush_by(PC_VPAL);
        R[c] = arg; load_clut(); tsm_epoch++; return;
    }
    default: R[c] = arg; return;
    }
}

// uncompressed PNG (stored blocks) to depend on nothing
static uint32_t crc_table[256];
static void crc_init() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE* f, const char* type, const uint8_t* d, uint32_t n) {
    uint8_t hdr[8]; be32(hdr, n); memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc((const uint8_t*)type, 4);
    if (n) c = crc(d, n, c);
    uint8_t cr[4]; be32(cr, c ^ 0xFFFFFFFFu);
    fwrite(cr, 1, 4, f);
}

static void save_png(const char* path, const uint32_t* px, int w, int h) {
    static bool init = false;
    if (!init) { crc_init(); init = true; }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(signature, 1, 8, f);
    uint8_t ihdr[13]; be32(ihdr, w); be32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t row = 1 + (size_t)w * 4, raw_tex = row * h;
    uint8_t* raw = (uint8_t*)malloc(raw_tex);
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * w + x];
            uint8_t* p = raw + y * row + 1 + x * 4;
            p[0] = c; p[1] = c >> 8; p[2] = c >> 16; p[3] = 255;
        }
    }
    size_t nb = (raw_tex + 65534) / 65535;
    size_t zlen = 2 + raw_tex + nb * 5 + 4;
    uint8_t* z = (uint8_t*)malloc(zlen);
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_tex; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    while (pos < raw_tex) {
        size_t len = raw_tex - pos > 65535 ? 65535 : raw_tex - pos;
        z[o++] = (pos + len == raw_tex) ? 1 : 0;
        z[o++] = len & 0xFF; z[o++] = len >> 8; z[o++] = (~len) & 0xFF; z[o++] = ((~len) >> 8) & 0xFF;
        memcpy(z + o, raw + pos, len); o += len; pos += len;
    }
    be32(z + o, (b << 16) | a); o += 4;
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", nullptr, 0);
    fclose(f);
    free(raw); free(z);
}

void psp_gpu_capture(uint32_t dir, int width, int fmt);

// TTT_CAPTURES=1 re-enables the automatic debug dumps. Off by
// default: at high resolution each capture freezes the frame for hundreds of ms.
static int capt_auto_var = -1;
static int capt_auto() {
    if (capt_auto_var < 0) { const char* e = getenv("TTT_CAPTURES"); capt_auto_var = e ? atoi(e) : 0; }
    return capt_auto_var;
}

// smooth motion: end of a presented frame (tables advance, next frame decides whether it records)
static void sm_frame_end() {
    sm_mv.clear(); sm_mvsum = 0;
    sm_fno++; if (sm_fno < 2) sm_fno = 2;
    int mode = glr_enabled() ? sm_mode() : 0;
    bool battle = wat_stale < 30;
    bool was_on = sm_on;
    sm_on = mode != 0 && battle && psp_present_nvb() >= 2;
    sm_rec = false;
    static int sl = 0;
    if (sm_on != was_on && sl < 20) { sl++; printf("suave: %s (modo %d, batalla %d, espera %d vblanks)\n", sm_on ? "activo" : "inactivo", mode, (int)battle, psp_present_nvb()); fflush(stdout); }
    static auto t0 = std::chrono::steady_clock::now(); static int lines = 0; static bool was = false;
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (el < 1.0) return;
    t0 = std::chrono::steady_clock::now();
    static int dl = 0;
    if (mode && !sm_on && dl < 12) { dl++; printf("suave: sin activar: modo %d, GPU %d, batalla %d (agua estancada %d), espera %d vblanks\n", mode, (int)glr_enabled(), (int)battle, (int)wat_stale, psp_present_nvb()); fflush(stdout); }
    int wn = 0; double wa = 0, wm = 0;
    psp_window_mid_stats(&wn, &wa, &wm);
    if ((sm_st_f || sm_st_few) && lines < 200) {
        double fn = sm_st_f ? (double)sm_st_f : 1.0;
        lines++;
        printf("suave: modo %d, %ld imagenes, %ld intermedias, %ld cortes, HUD %ld, vertices con pareja %.0f%%, movimiento medio %.1f px, CPU %.2f ms por intermedia, busquedas %llu (%.0f%% halladas), ventana %d intermedias, retraso %.1f ms (max %.1f), cuadros sin escena %ld\n",
               mode, sm_st_f, sm_st_mid, sm_st_cut, sm_st_hud, 100.0 * sm_st_kn / fn, sm_st_mv / fn, sm_st_mid ? sm_st_us / 1000.0 / sm_st_mid : 0.0,
               (unsigned long long)sm_look, sm_look ? 100.0 * sm_hit / sm_look : 0.0, wn, wa, wm, sm_st_few);
        fflush(stdout);
        was = true;
    } else if (was && !sm_st_f && mode) { was = false; printf("suave: sin intermedias (batalla %d, espera %d vblanks)\n", (int)battle, psp_present_nvb()); fflush(stdout); }
    sm_st_f = sm_st_mid = sm_st_cut = sm_st_hud = sm_st_few = 0; sm_st_us = sm_st_kn = sm_st_mv = 0; sm_look = sm_hit = 0;
}

// TTT_MENU_PROBE=1: mean brightness of every presented frame; a frame under half the last bright one is saved as negro_NN.png
static bool glr_peek(uint32_t dir, int rows, std::vector<uint32_t>& px, int* w, int* h);
static void pk_measure(uint32_t dir, int width, int fmt) {
    static int on = -1, saved = 0, lines = 0, run = 0;
    static float ref = -1.0f;
    if (on < 0) { const char* e = getenv("TTT_MENU_PROBE"); on = e && *e == '1'; }
    if (!on) return;
    static std::vector<uint32_t> px;
    int w = 0, h = 0, rows = 27;
    uint32_t hd = high_map(dir);
    bool gl = glr_enabled() && glr_peek(hd, rows, px, &w, &h);
    if (!gl) {
        int ke = scale_int(); w = 480 * ke; h = 272 * ke;
        FB f; f.dir = hd; f.width = (width ? width : 512) * ke; f.fmt = fmt & 3;
        px.resize((size_t)w * rows);
        for (int r = 0; r < rows; r++) for (int x = 0; x < w; x++) px[(size_t)r * w + x] = read_pix(f, x, (2 * r + 1) * h / (2 * rows));
    }
    double sum = 0; int n = 0;
    for (int r = 0; r < rows; r++) for (int x = 0; x < w; x += 8) { uint32_t c = px[(size_t)r * w + x]; sum += 0.299 * (c & 0xFF) + 0.587 * ((c >> 8) & 0xFF) + 0.114 * ((c >> 16) & 0xFF); n++; }
    pk_lum = n ? (float)(sum / n) : -1.0f;
    bool dark = ref > 12.0f && pk_lum < ref * 0.5f;
    if (!dark) { if (run && lines < 300) { lines++; printf("sonda gpu: fin de cuadros oscuros (%d), luz %.0f\n", run, pk_lum); } run = 0; ref = pk_lum; return; }
    run++;
    if (lines < 300) { lines++; printf("sonda gpu: cuadro oscuro %d, luz %.0f (antes %.0f), %d tex %d plano %d borrado\n", run, pk_lum, ref, pk_l[0], pk_l[1], pk_l[2]); }
    if (run != 1 || saved >= 20) return;
    if (gl) { if (!glr_peek(hd, 0, px, &w, &h)) return; }
    std::vector<uint32_t> img(480 * 272);
    for (int y = 0; y < 272; y++) for (int x = 0; x < 480; x++) {
        int sx = x * w / 480, sy = y * h / 272;
        uint32_t c;
        if (gl) c = px[(size_t)(h - 1 - sy) * w + sx];
        else { FB f; f.dir = hd; f.width = (width ? width : 512) * scale_int(); f.fmt = fmt & 3; c = read_pix(f, sx, sy); }
        img[(size_t)y * 480 + x] = c | 0xFF000000u;
    }
    char nm[32];
    snprintf(nm, sizeof(nm), "negro_%02d.png", ++saved);
    save_png(nm, img.data(), 480, 272);
    printf("sonda gpu: imagen %s\n", nm);
}

static int gpu_black = 0;
void psp_gpu_blackout(bool on) { gpu_black = on ? 10 : 0; }   // lasts 10 presents unless renewed
static void glr_blank(uint32_t dir);

bool psp_gpu_to_window(uint32_t dir, int width, int fmt) {
    psp_gpu_flush();
    if (gpu_black > 0) {
        gpu_black--;
        uint32_t hd = high_map(dir);
        if (glr_enabled()) { glr_blank(hd); glr_blank(twin_dir(norm_dir(hd))); }
        else {
            int ke = scale_int();
            FB f; f.dir = hd; f.width = (width ? width : 512) * ke; f.fmt = fmt & 3;
            for (int y = 0; y < 272 * ke; y++) for (int x = 0; x < 480 * ke; x++) write_pix(f, x, y, 0xFF000000u);
        }
    }
    struct SmEnd { ~SmEnd() { sm_frame_end(); } } sm_end_;
    toon_frame();
    prims_last = prims_frame; prims_frame = 0;
    pk_l[0] = pk_tex; pk_l[1] = pk_flat; pk_l[2] = pk_clr; pk_tex = pk_flat = pk_clr = 0; pk_n++;
    pk_measure(dir, width, fmt);
    {
        std::lock_guard<std::mutex> g(rm_mx);
        for (int i = 0; i < 4; i++) { rm_rowl[i] = rm_row[i]; rm_row[i] = -1; }
        rm_frame++;
    }
    skin_frame++;
    if (split_first) { split_first = 0; split_report(); }
    else ge_report();
    gv_report();
    alt_frame++; alt_drew = 0;
    double asp = psp_window_aspect(), nat = 480.0 / 272.0;
    asp_x = asp > nat ? (float)(nat / asp) : 1.0f;
    asp_y = asp < nat ? (float)(asp / nat) : 1.0f;
    sc_frame++;
    if (psp_window_lod_toggle()) {
        lod_type();
        lod_var = lod_var ? 0 : 4;
        printf("LOD: malla de personajes lejanos %s\n", lod_var ? "activada" : "desactivada");
        fflush(stdout);
    }
    if (psp_window_scene_requested()) sc_requested = 3;
    if (sc_requested) { sc_requested--; sc_print(); }
    sc_n = 0; sc_act = -1; sc_lost = 0; sc_prim3d = sc_draw = sc_close = sc_cull = sc_spr = 0;
    PERF(PZ_PRES);
    // automatic capture: once there is 3D, one every 240 frames, 4 at most
    if (capt_auto()) {
        static uint64_t last_3d = 0; static int frames_3d = 0, made = 0;
        if (prims_3d_ok != last_3d) { last_3d = prims_3d_ok; frames_3d++; if (frames_3d % 240 == 60 && made < 4) { made++; psp_gpu_capture(dir, width, fmt); } }
    }
    if (glr_enabled()) {
        uint32_t hd = high_map(dir), nd = norm_dir(hd);
        if (nd != disp_dir[0]) { disp_dir[1] = disp_dir[0]; disp_dir[0] = nd; }
        int tw = 0, th = 0;
        unsigned t2 = dual_full() && twin_drawn + 1 == alt_frame ? glr_twin_tex(twin_dir(nd), &tw, &th) : 0;
        if (t2 && !dual_logged) { dual_logged = 1; printf("dual: pantalla 2 a resolucion completa, destino propio %dx%d (lienzo %dx%d por pantalla)\n", tw, th, 480 * scale_int(), 272 * scale_int()); fflush(stdout); }
        psp_window_texture_dual(t2);
        if (glr_present(hd)) return true;
    }
    int ke = scale_int(), aw = 480 * ke, ah = 272 * ke;
    uint32_t* px = psp_window_canvas(aw, ah);
    if (!px) return true;
    FB f; f.dir = high_map(dir); f.width = (width ? width : 512) * ke; f.fmt = fmt & 3;
    for (int y = 0; y < ah; y++)
        for (int x = 0; x < aw; x++) {
            uint32_t c = read_pix(f, x, y);
            // the window expects BGRA; the PSP framebuffer is RGBA
            px[(size_t)y * aw + x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF) | 0xFF000000u;
        }
    return true;
}

void psp_gpu_frame(uint32_t dir, int width, int fmt) {
    psp_gpu_flush();
    PERF(PZ_PRES);
    // called from sceDisplaySetFrameBuf; dumps one frame in 60 (30 files at most)
    frames_drawn++;
    toon_frame();
    if (!capt_auto()) return;
    if (frames_drawn % 60 != 1 || frames_drawn / 60 >= 80) return;
    int ke = scale_int(), aw = 480 * ke, ah = 272 * ke;
    static std::vector<uint32_t> px;
    px.resize((size_t)aw * ah);
    FB f; f.dir = high_map(dir); f.width = (width ? width : 512) * ke; f.fmt = fmt & 3;
    for (int y = 0; y < ah; y++)
        for (int x = 0; x < aw; x++) px[(size_t)y * aw + x] = read_pix(f, x, y);
    char nombre[64];
    snprintf(nombre, sizeof(nombre), "cuadro_%04llu.png", (unsigned long long)frames_drawn);
    save_png(nombre, px.data(), aw, ah);
    uint32_t sample[4] = {read_pix(f, 10, 10), read_pix(f, aw / 2, ah / 2), read_pix(f, aw - 10, ah - 12), read_pix(f, 100, 200)};
    printf("cuadro guardado: %s (fb 0x%08X ancho %d fmt %d) pixeles 0x%08X 0x%08X 0x%08X 0x%08X | ultimo FBP 0x%06X FBW 0x%06X PSM %u\n",
           nombre, dir, f.width, f.fmt, sample[0], sample[1], sample[2], sample[3], R[0x9C], R[0x9D], R[0xD2]);
}

static void print_mat(const char* name, const float* m, int n) {
    printf("  %s:", name);
    for (int i = 0; i < n; i++) printf("%s%.4f", (i % (n == 16 ? 4 : 3)) ? " " : "  ", m[i]);
    printf("\n");
}

void psp_gpu_capture(uint32_t dir, int width, int fmt) {
    psp_gpu_flush();
    if (glr_enabled()) glr_read(high_map(dir), (width ? width : 512) * scale_int(), fmt & 3);
    static int captures = 0;
    captures++;
    int ke = scale_int(), aw = 480 * ke, ah = 272 * ke;
    static std::vector<uint32_t> px;
    px.resize((size_t)aw * ah);
    FB f; f.dir = high_map(dir); f.width = (width ? width : 512) * ke; f.fmt = fmt & 3;
    for (int y = 0; y < ah; y++) for (int x = 0; x < aw; x++) px[(size_t)y * aw + x] = read_pix(f, x, y);
    char nombre[64];
    snprintf(nombre, sizeof(nombre), "captura_%02d.png", captures);
    save_png(nombre, px.data(), aw, ah);
    ZB z = zb();
    for (int y = 0; y < ah; y++) for (int x = 0; x < aw; x++) { uint32_t d = psp_l16(z.dir + (y * z.width + x) * 2) >> 8; px[(size_t)y * aw + x] = d | (d << 8) | (d << 16); }
    char nz[64];
    snprintf(nz, sizeof(nz), "captura_%02d_z.png", captures);
    save_png(nz, px.data(), aw, ah);
    printf("captura %s y %s | fb 0x%08X ancho %d fmt %d | FBP 0x%06X FBW 0x%06X ZBP 0x%06X ZBW 0x%06X | ztest %u fn %u zwrite %s minz %u maxz %u | cull %u/%u | luz %u [%u %u %u %u] | tmap %u shade %u | clear ultimo 0x%06X\n",
           nombre, nz, dir, f.width, f.fmt, R[0x9C], R[0x9D], R[0x9E], R[0x9F], R[0x23] & 1, R[0xDE] & 7, (R[0xE7] & 1) ? "no" : "si",
           R[0xD6] & 0xFFFF, R[0xD7] & 0xFFFF, R[0x1D] & 1, R[0x9B] & 1, R[0x17] & 1, R[0x18] & 1, R[0x19] & 1, R[0x1A] & 1, R[0x1B] & 1,
           R[0xC0] & 3, R[0x50] & 1, R[0xD3]);
    printf("  viewport esc (%.2f %.2f %.2f) centro (%.2f %.2f %.2f) offset (%.1f %.1f) region (%u,%u)-(%u,%u) scissor (%u,%u)-(%u,%u)\n",
           f24(R[0x42]), f24(R[0x43]), f24(R[0x44]), f24(R[0x45]), f24(R[0x46]), f24(R[0x47]),
           (R[0x4C] & 0xFFFF) / 16.0f, (R[0x4D] & 0xFFFF) / 16.0f, R[0x15] & 0x3FF, (R[0x15] >> 10) & 0x3FF, R[0x16] & 0x3FF, (R[0x16] >> 10) & 0x3FF,
           R[0xD4] & 0x3FF, (R[0xD4] >> 10) & 0x3FF, R[0xD5] & 0x3FF, (R[0xD5] >> 10) & 0x3FF);
    printf("  STENCIL on %u | test 0x%06X | op 0x%06X | maskA 0x%02X\n", R[0x24] & 1, R[0xDC], R[0xDD], R[0xE9] & 0xFF);
    printf("  TMAP modo %u fuente %u variante %d | TEXSCALE (%.3f %.3f) OFFSET (%.3f %.3f)\n",
           R[0xC0] & 3, (R[0xC0] >> 8) & 3, tmap_variant(), f24(R[0x48]), f24(R[0x49]), f24(R[0x4A]), f24(R[0x4B]));
    print_mat("tgen", m_tgen, 12);
    print_mat("mundo", m_world, 12); print_mat("vista", m_view, 12); print_mat("proy", m_proj, 16);
    for (int i = 0; i < uses_n; i++)
        printf("  %s 0x%08X %dx%d fmt %d swz %d stride %d | mezcla %u eq/fac 0x%06X alfa 0x%06X | %llu pixeles\n",
               uses[i].d2 == 1 ? "SPRITE" : (uses[i].d2 == 2 ? "tri2D" : "tri3D"), uses[i].t.dir, uses[i].t.w, uses[i].t.h, uses[i].t.fmt,
               uses[i].t.swz, uses[i].t.stride, uses[i].blend_mode & 1, uses[i].blend_mode >> 1, uses[i].prim,
               (unsigned long long)uses[i].px);
    goal_n = 0;
    {   // mark the 4 with the largest coverage to dump them live on the next draw
        bool done[32] = {false};
        for (int k = 0; k < 4; k++) {
            int best = -1;
            for (int i = 0; i < uses_n; i++) if (!done[i] && (best < 0 || uses[i].px > uses[best].px)) best = i;
            if (best < 0) break;
            done[best] = true;
            goal[goal_n++] = uses[best].t.dir;
        }
    }
    dump_tex = 14;
    uses_n = 0;
    printf("  primitivas con pesos desde el arranque: %llu con huesos planos, %llu con huesos llenos\n",
           (unsigned long long)skin_flat, (unsigned long long)skin_full);
    trace3d = 60; trace_n = 0; trace_ramp = 0; trace_ramp2d = 0; trace_vram = 0; vram_n = 0; alpha_dump = 0; spy_n = 0; spy_vn = 0;
    printf("  alfafb %d | borrados usados:\n", (int)keep_alpha());
    for (int i = 0; i < bo_n; i++)
        printf("    modo 0x%06X color 0x%08X -> %llu primitivas\n", bo_mode[i], bo_col[i], (unsigned long long)bo_cnt[i]);
    bo_n = 0;
    static const char* eqn[8] = {"suma", "resta", "resta inv", "min", "max", "ABSDIFF", "?6", "?7"};
    printf("  primitivas 3D que leen VRAM como textura:\n");
    for (int i = 0; i < v3_n; i++)
        printf("    tex 0x%08X | pantalla (%d,%d)-(%d,%d) | uv (%.1f..%.1f, %.1f..%.1f) | %llu primitivas, %llu pixeles\n",
               v3_tex[i], v3_x0[i], v3_y0[i], v3_x1[i], v3_y1[i], v3_u[i][0], v3_u[i][1], v3_v[i][0], v3_v[i][1],
               (unsigned long long)v3_cnt[i], (unsigned long long)v3_px[i]);
    v3_n = 0;
    printf("  primitivas 3D con TMAP generado:\n");
    for (int i = 0; i < tm_n; i++)
        printf("    tmap %u fuente %u | tex 0x%08X %ux%u fmt %u | color vert 0x%08X | mezcla %u/0x%06X | atest %u/0x%06X -> %llu primitivas\n",
               tm_v[i] & 3, (tm_v[i] >> 8) & 3, tm_tex[i], tm_wh[i] >> 16, (tm_wh[i] >> 4) & 0xFFF, tm_wh[i] & 15,
               tm_col[i], tm_bl[i] & 1, tm_bl[i] >> 1, tm_at[i] & 1, tm_at[i] >> 1, (unsigned long long)tm_cnt[i]);
    tm_n = 0;
    printf("  iluminacion en 3D:\n");
    for (int i = 0; i < lz_n; i++)
        printf("    luz %u activas %u%u%u%u | l0 tipo %u calc %u | l1 tipo %u calc %u | matupd 0x%02X | l0 pos (%.3f %.3f %.3f) -> %llu primitivas\n",
               lz_en[i] & 1, (lz_en[i] >> 1) & 1, (lz_en[i] >> 2) & 1, (lz_en[i] >> 3) & 1, (lz_en[i] >> 4) & 1,
               (lz_tipo[i] >> 8) & 3, lz_tipo[i] & 3, (lz_tipo[i] >> 20) & 3, (lz_tipo[i] >> 12) & 3,
               lz_mat[i] & 0xFF, lz_d[i][0], lz_d[i][1], lz_d[i][2], (unsigned long long)lz_cnt[i]);
    lz_n = 0;
    printf("  mezclas usadas en 3D:\n");
    for (int i = 0; i < mz_n; i++) {
        if (!mz_v[i]) { printf("    sin mezcla -> %llu primitivas\n", (unsigned long long)mz_cnt[i]); continue; }
        uint32_t b = mz_v[i] & 0x7FF;
        printf("    eq %u %s fs %u fd %u -> %llu primitivas\n", (b >> 8) & 7, eqn[(b >> 8) & 7], b & 15, (b >> 4) & 15, (unsigned long long)mz_cnt[i]);
    }
    mz_n = 0;
    printf("  filtros de textura en 3D (min/mag, niveles):\n");
    for (int i = 0; i < fi_n; i++)
        printf("    min %u mag %u niveles %u -> %llu primitivas\n", fi_f[i] & 7, (fi_f[i] >> 8) & 7, fi_lvl[i], (unsigned long long)fi_cnt[i]);
    fi_n = 0;
    printf("  estados de stencil usados en 3D:\n");
    for (int i = 0; i < st_n; i++) printf("    on %u test 0x%06X op 0x%06X -> %llu primitivas\n", st_en[i], st_test[i], st_op[i], (unsigned long long)st_cnt[i]);
    st_n = 0;
}

void psp_gpu_report() {
    glr_report();
    printf("GPU: funciones sin implementar que el juego enciende: niebla %llu (color 0x%06X f1 %.1f f2 %.1f), dither %llu, antialias %llu, test de color %llu, logica %llu, parches %llu\n",
           (unsigned long long)ge_fog, ge_fog_col, ge_fog_f1, ge_fog_f2,
           (unsigned long long)ge_dither, (unsigned long long)ge_aa, (unsigned long long)ge_ctest,
           (unsigned long long)ge_logop, (unsigned long long)ge_flat);
    printf("GPU: %llu sprites, %llu primitivas sin cubrir, %llu ordenes 3D (%llu triangulos/sprites 3D pintados, %llu detras de la camara, %llu por cull), %llu cuadros presentados\n",
           (unsigned long long)sprites, (unsigned long long)prims_other, (unsigned long long)prims_3d,
           (unsigned long long)prims_3d_ok, (unsigned long long)tris_out, (unsigned long long)tris_cull, (unsigned long long)frames_drawn);
}
