#pragma once
// item 33 part 2: renders generated from the 3D model (Graphics > Renders). The select draws each character and colour zoomed on
// the screen so that the big render box fills it (gpu.cpp cap_*, framing in render_frame.h); the big render and the face crops
// (mid, small, HUD 1, HUD 2, beam clash 1 and 3 boxes) are cut from that capture into assets/cache/renders/q<quality>/
// <sysid>_<colour>_<kind>.png; index.txt keeps the capture points. Nothing else is kept
#include <thread>
#include <list>
#include <memory>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>

#include "disc/render_frame.h"
#include "third_party/stb_image.h"

namespace gr {
struct Img { int w = 0, h = 0; std::vector<uint32_t> px; uint64_t uid = 0; };
static std::atomic<uint64_t> img_uid{1};
static const char* KIND[] = {"big", "mid", "small", "hud1", "hud2", "clash1", "clash2"};
enum { K_BIG, K_MID, K_SMALL, K_HUD1, K_HUD2, K_CLASH1, K_CLASH2, K_N };
static const int DMG = 64;   // colour + 64 = its damaged level 2 costume (big, HUD and clash only)
static const int BIG_W = 272, BIG_H = 176;
struct Src { float w, h, p[7]; float tw = 0; };   // tw: signature of the tweak the files were made with   // capture size; head x, y, waist x, y, mouth x, y (-1 none) in its pixels, head to waist distance over Goku's (world)
static std::mutex mx;
static std::map<uint64_t, Src> pts;
static std::atomic<bool> loaded[5] = {false, false, false, false, false};
static std::mutex ilm;
static std::atomic<int> busy{0};
static std::atomic<uint32_t> epoch{1};   // bumped when files change: the textures served from them are made again
static std::mutex fvm;
static std::map<std::string, uint32_t> fver;   // times each file was written here: decoded images stay valid while their file is unchanged
static std::string fver_key(const std::string& f) { std::lock_guard<std::mutex> g(fvm); auto it = fver.find(f); return "|" + std::to_string(it == fver.end() ? 0u : it->second) + "|" + f; }

static std::string dir(int q) { return disc_dir + "/cache/renders/q" + std::to_string(q); }
static std::string file(int q, int s, int c, int k) { return dir(q) + "/" + std::to_string(s) + "_" + std::to_string(c) + "_" + KIND[k] + ".png"; }
static std::string cap_file(int q, int s, int c) { return dir(q) + "/cap/" + std::to_string(s) + "_" + std::to_string(c) + ".png"; }
static uint64_t key(int q, int s, int c) { return (uint64_t)q << 40 | (uint64_t)(uint32_t)s << 8 | (uint32_t)c; }

static bool load(const std::string& f, Img& m) {
    std::vector<uint8_t> d; std::string err;
    FILE* fp = fopen(f.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    d.resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(d.data(), 1, n, fp) == (size_t)n;
    fclose(fp);
    if (!ok) return false;
    static int fast = -1;
    if (fast < 0) { const char* e = getenv("TTT_RENDER_STB"); fast = !(e && *e == '0'); }
    if (fast) {   // stb's table inflate: about twice as fast as png_decode
        int w = 0, h = 0, n = 0;
        if (uint8_t* p = stbi_load_from_memory(d.data(), (int)d.size(), &w, &h, &n, 4)) {
            m.w = w; m.h = h; m.px.resize((size_t)w * h);
            memcpy(m.px.data(), p, (size_t)w * h * 4);
            stbi_image_free(p);
            return true;
        }
    }
    return png_decode(d, &m.w, &m.h, m.px, &err);
}
static const Img* asset(const char* name) {   // port data next to the select3d animations
    static std::map<std::string, Img> cache;
    std::lock_guard<std::mutex> g(mx);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second.w ? &it->second : nullptr;
    Img& m = cache[name];
    if (!load(std::string("../../games/ttt/renders/") + name, m)) { printf("render: falta games/ttt/renders/%s\n", name); m = Img(); return nullptr; }
    return &m;
}

// area-filtered sample of the source rect (fx0, fy0)-(fx1, fy1) into a w x h image, premultiplied alpha
static Img resample(const Img& s, float fx0, float fy0, float fx1, float fy1, int w, int h) {
    Img o; o.w = w; o.h = h; o.px.assign((size_t)w * h, 0);
    float sx = (fx1 - fx0) / w, sy = (fy1 - fy0) / h;
    static int fast = -1;
    if (fast < 0) { const char* e = getenv("TTT_RENDER_FIT_FAST"); fast = !(e && *e == '0'); }
    bool mag = fast && sx <= 1.0f && sy <= 1.0f;   // magnifying: one bilinear sample
    int nx = mag ? 1 : std::max(1, std::min(8, (int)std::ceil(sx * 2))), ny = mag ? 1 : std::max(1, std::min(8, (int)std::ceil(sy * 2)));
    std::vector<float> pm((size_t)s.w * s.h * 4);   // premultiplied once
    for (size_t i = 0; i < (size_t)s.w * s.h; i++) {
        uint32_t p = s.px[i]; float a = (p >> 24) / 255.0f;
        pm[i * 4] = (p & 255) * a; pm[i * 4 + 1] = ((p >> 8) & 255) * a; pm[i * 4 + 2] = ((p >> 16) & 255) * a; pm[i * 4 + 3] = a;
    }
    static const float Z[4] = {0, 0, 0, 0};
    auto at = [&](int x, int y) -> const float* { return x < 0 || y < 0 || x >= s.w || y >= s.h ? Z : &pm[((size_t)y * s.w + x) * 4]; };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    float u = fx0 + (x + (i + 0.5f) / nx) * sx - 0.5f, v = fy0 + (y + (j + 0.5f) / ny) * sy - 0.5f;
                    int x0 = (int)std::floor(u), y0 = (int)std::floor(v);
                    float tx = u - x0, ty = v - y0;
                    const float *c00 = at(x0, y0), *c10 = at(x0 + 1, y0), *c01 = at(x0, y0 + 1), *c11 = at(x0 + 1, y0 + 1);
                    for (int q = 0; q < 4; q++) acc[q] += (c00[q] * (1 - tx) + c10[q] * tx) * (1 - ty) + (c01[q] * (1 - tx) + c11[q] * tx) * ty;
                }
            float n = (float)(nx * ny), a = acc[3] / n;
            if (a <= 0.0f) continue;
            auto ch = [&](float v) { int r = (int)(v / n / a + 0.5f); return (uint32_t)std::min(255, std::max(0, r)); };
            o.px[(size_t)y * w + x] = ch(acc[0]) | ch(acc[1]) << 8 | ch(acc[2]) << 16 | (uint32_t)std::min(255, (int)(a * 255.0f + 0.5f)) << 24;
        }
    return o;
}
static void over(Img& d, const Img& s, int ox, int oy, int x0, int y0, int x1, int y1) {   // s over d at (ox, oy), only inside (x0, y0)-(x1, y1)
    for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++) {
            int X = ox + x, Y = oy + y;
            if (X < x0 || Y < y0 || X >= x1 || Y >= y1) continue;
            uint32_t a = s.px[(size_t)y * s.w + x], b = d.px[(size_t)Y * d.w + X];
            float fa = (a >> 24) / 255.0f, fb = (b >> 24) / 255.0f, fo = fa + fb * (1 - fa);
            if (fo <= 0) continue;
            uint32_t r = 0;
            for (int c = 0; c < 3; c++) {
                float v = (((a >> (8 * c)) & 255) * fa + ((b >> (8 * c)) & 255) * fb * (1 - fa)) / fo;
                r |= (uint32_t)std::min(255, (int)(v + 0.5f)) << (8 * c);
            }
            d.px[(size_t)Y * d.w + X] = r | (uint32_t)std::min(255, (int)(fo * 255.0f + 0.5f)) << 24;
        }
}
static Img scaled(const Img& a, int q) { return q == 1 ? a : resample(a, 0, 0, (float)a.w, (float)a.h, a.w * q, a.h * q); }
static float span_of(const float* p) { return std::max(4.0f, p[3] - p[1]); }
static const int REF_SYS = 3;   // Goku: his values were tuned on him
static bool ref_ok = false;
static float ref[7];
// per character tweaks, games/ttt/renders/tweaks.txt: "<sysid or *> <zoom> <face lower> <face right> <big lower>", re-read when the file changes
static std::map<int, rf::Tweak> tweaks;
static rf::Tweak tweak_all;
static int64_t tweaks_date = -2;
static uint32_t tweaks_gen = 0;
static std::mutex twm;
static void tweaks_check() {
    static uint64_t last = 0;
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now == last && tweaks_date != -2) return;
    last = now;
    std::error_code ec;
    auto path = std::filesystem::u8path("../../games/ttt/renders/tweaks.txt");
    int64_t d = std::filesystem::exists(path, ec) ? (int64_t)std::filesystem::last_write_time(path, ec).time_since_epoch().count() : -1;
    if (d == tweaks_date) return;
    std::lock_guard<std::mutex> g(twm);
    tweaks_date = d; tweaks_gen++;
    tweaks.clear(); tweak_all = rf::Tweak();
    FILE* f = fopen("../../games/ttt/renders/tweaks.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char id[32]; rf::Tweak t;
        if (line[0] == '#' || sscanf(line, "%31s %f %f %f %f %f %f %f", id, &t.zoom, &t.dy, &t.dx, &t.bdy, &t.bzoom, &t.rup, &t.shead) < 2) continue;
        if (id[0] == '*') tweak_all = t; else if (isdigit((unsigned char)id[0])) tweaks[atoi(id)] = t;
    }
    fclose(f);
    printf("render: ajustes por personaje leidos, %d personajes, general zoom %.2f bajar %.2f\n", (int)tweaks.size(), tweak_all.zoom, tweak_all.dy);
}
static rf::Tweak tweak_of(int s) {
    tweaks_check();
    std::lock_guard<std::mutex> g(twm);
    auto it = tweaks.find(s);
    return it == tweaks.end() ? tweak_all : it->second;
}
// render editor (E.6): zoom and move per output, global (s = -1) and per character (c = -1 every colour) or colour; assets/cache/renders/edits.txt
struct Edit { float z = 1, dx = 0, dy = 0; };
static const int E_N = 6;   // big, mid, small, HUD 1, HUD 2, beam clash (both types)
static std::mutex edm;
static std::map<std::array<int, 3>, Edit> edits;
static bool edits_ok = false;
static int e_of(int k) { return k >= K_CLASH1 ? 5 : k; }
static std::string edits_file() { return disc_dir + "/cache/renders/edits.txt"; }
static void edits_parse(std::map<std::array<int, 3>, Edit>& out) {
    FILE* f = fopen(edits_file().c_str(), "r");
    if (!f) return;
    char a[16], b[16]; int k; Edit e;
    while (fscanf(f, "%15s %15s %d %f %f %f", a, b, &k, &e.z, &e.dx, &e.dy) == 6)
        if (k >= 0 && k < E_N) out[{a[0] == '*' ? -1 : atoi(a), b[0] == '*' ? -1 : atoi(b), k}] = e;
    fclose(f);
}
static void edits_load() {   // edm held
    if (edits_ok) return;
    edits_ok = true;
    edits_parse(edits);
    printf("render: ajustes del editor leidos, %d\n", (int)edits.size());
}
static Edit edit_in(const std::map<std::array<int, 3>, Edit>& m, int s, int c, int k) {   // global, then the colour's or else the character's
    Edit r;
    int ek = e_of(k);
    auto gi = m.find({-1, -1, ek});
    if (gi != m.end()) r = gi->second;
    auto it = m.find({s, c, ek});
    if (it == m.end()) it = m.find({s, -1, ek});
    if (it != m.end()) { r.z *= it->second.z; r.dx += it->second.dx; r.dy += it->second.dy; }
    return r;
}
static Edit edit_of(int s, int c, int k) {
    std::lock_guard<std::mutex> g(edm);
    edits_load();
    return edit_in(edits, s, c, k);
}
static void edit_box(const Edit& e, float* x0, float* y0, float* bw, float* bh) {   // z > 1 closer, dx > 0 subject to the right, dy > 0 lower
    float cx = *x0 + *bw * 0.5f, cy = *y0 + *bh * 0.5f;
    *bw /= std::max(0.2f, e.z); *bh /= std::max(0.2f, e.z);
    cx -= e.dx * *bw; cy -= e.dy * *bh;
    *x0 = cx - *bw * 0.5f; *y0 = cy - *bh * 0.5f;
}
static float tweak_sig(const rf::Tweak& t) { return t.zoom * 1000.0f + t.dy * 10.0f + t.dx + t.bdy * 3.7f + (t.bzoom - 1.0f) * 53.0f; }
static Img face_crop(const Img& src, const float* p, int k, int w, int h, const rf::Tweak& t, const Edit& ed) {
    float x0, y0, bw, bh;
    int fk = k == K_MID ? rf::MID : k == K_SMALL ? rf::SMALL : k == K_HUD1 ? rf::HUD1 : k == K_HUD2 ? rf::HUD2 : rf::CLASH;
    rf::face(fk, p[0], rf::eye_y(p[1], p[5], span_of(p), p[6]), span_of(p), p[6], w, h, &x0, &y0, &bw, &bh);   // p[5] = neck y, -1 none
    rf::tweak_box(t, &x0, &y0, &bw, &bh, w, h);
    edit_box(ed, &x0, &y0, &bw, &bh);
    return resample(src, x0, y0, x0 + bw, y0 + bh, w, h);
}
static Img big_crop(const Img& src, const float* p, int w, int h, const Edit& ed) {   // the capture is the big box plus its margin
    float x0, y0, bw, bh;
    rf::capture_box((float)src.w, (float)src.h, &x0, &y0, &bw, &bh);
    edit_box(ed, &x0, &y0, &bw, &bh);
    return resample(src, x0, y0, x0 + bw, y0 + bh, w, h);
}
static bool border(const Img& bg, int x, int y) { if (x < 0 || y < 0 || x >= bg.w || y >= bg.h) return false; uint32_t p = bg.px[(size_t)y * bg.w + x]; return (p >> 24) > 128 && (p & 255) < 24 && ((p >> 8) & 255) < 24 && ((p >> 16) & 255) < 24; }
// boxes of a background: opaque runs of its middle row (68x48 cells of the beam clash)
static std::vector<std::array<int, 4>> boxes(const Img& bg) {
    std::vector<std::array<int, 4>> v;
    int y = 0;
    for (int yy = 0; yy < bg.h; yy++) if ((bg.px[(size_t)yy * bg.w + 1] >> 24) > 0) y = yy; else if (y) break;
    int mid = y / 2;
    for (int x = 0; x < bg.w;) {
        if ((bg.px[(size_t)mid * bg.w + x] >> 24) == 0) { x++; continue; }
        int x0 = x; while (x < bg.w && (bg.px[(size_t)mid * bg.w + x] >> 24) > 0) x++;
        v.push_back({x0, 0, x, y + 1});
    }
    return v;
}
// background with the face inside each box (inset by its black border when it has one)
static Img boxed(const Img& bg0, const Img& src, const float* p, int k, int q, const rf::Tweak& t, const Edit& ed) {
    Img bg = scaled(bg0, q);
    auto bx = boxes(bg0);
    if (bx.empty()) bx.push_back({0, 0, bg0.w, bg0.h});
    for (auto& b : bx) {
        int in = border(bg0, b[0], (b[1] + b[3]) / 2) ? 1 : 0;
        int x0 = (b[0] + in) * q, y0 = (b[1] + in) * q, x1 = (b[2] - in) * q, y1 = (b[3] - in) * q;
        Img f = face_crop(src, p, k, x1 - x0, y1 - y0, t, ed);
        over(bg, f, x0, y0, x0, y0, x1, y1);
    }
    return bg;
}
static Img masked(const Img& mask, const Img& src, const float* p, int k, int q, const rf::Tweak& t, const Edit& ed) {
    Img m = scaled(mask, q), f = face_crop(src, p, k, m.w, m.h, t, ed);
    for (size_t i = 0; i < f.px.size(); i++) { uint32_t a = (f.px[i] >> 24) * (m.px[i] >> 24) / 255; f.px[i] = (f.px[i] & 0xFFFFFFu) | a << 24; }
    return f;
}
static bool save(const std::string& f, const Img& m) { bool ok = roster_png(f, m.w, m.h, m.px); std::lock_guard<std::mutex> g(fvm); fver[f]++; return ok; }
// every output of one render from its capture
static Img output(int k, int q, const Img& src, const float* p, const rf::Tweak& t, const Edit& ed) {
    static const char* AS[K_N] = {nullptr, "mid_bg.png", "small_bg.png", "hud_1_mask.png", "hud_2_mask.png", "hud_clash_type_1_bg.png", "hud_clash_type_2_bg.png"};
    if (k == K_BIG) return big_crop(src, p, BIG_W * q, BIG_H * q, ed);
    const Img* a = asset(AS[k]);
    if (!a) return Img();
    if (k != K_HUD1 && k != K_HUD2) return boxed(*a, src, p, k, q, t, ed);
    Img m = masked(*a, src, p, k, q, t, ed), o;   // the game's HUD portraits are 64x32, the image at the bottom rows
    o.w = 64 * q; o.h = 32 * q; o.px.assign((size_t)o.w * o.h, 0);
    int dy = std::max(0, o.h - m.h);
    for (int y = 0; y < std::min(m.h, o.h); y++) memcpy(&o.px[(size_t)(y + dy) * o.w], &m.px[(size_t)y * m.w], 4 * (size_t)std::min(m.w, o.w));
    return o;
}
static int derive(int q, int s, int c, const Img& src, const float* p, int mask = (1 << K_N) - 1) {
    int n = 0;
    rf::Tweak t = tweak_of(s);
    for (int k = 0; k < K_N; k++) {
        if (((k == K_MID || k == K_SMALL) && c > 0) || !(mask >> k & 1)) continue;   // the mid and small renders show colour 1
        Img m = output(k, q, src, p, t, edit_of(s, c, k));
        if (m.w) n += save(file(q, s, c, k), m);
    }
    return n;
}

static std::mutex imx;
static const int INDEX_VER = 6;   // the framing (render_frame.h) is part of the files: another version generates everything again
static void index_write(int q) {
    std::lock_guard<std::mutex> ig(imx);
    std::string f = dir(q) + "/index.txt", t = f + ".tmp";
    FILE* o = fopen(t.c_str(), "w");
    if (!o) return;
    fprintf(o, "version %d\n", INDEX_VER);
    {
        std::lock_guard<std::mutex> g(mx);
        for (auto& e : pts) if ((int)(e.first >> 40) == q) {
            const float* v = e.second.p;
            fprintf(o, "%d %d %.0f %.0f %.2f %.2f %.2f %.2f %.2f %.2f %.3f %.3f\n", (int)((e.first >> 8) & 0xFFFFFFFFu), (int)(e.first & 255), e.second.w, e.second.h, v[0], v[1], v[2], v[3], v[4], v[5], v[6], e.second.tw);
        }
    }
    fclose(o);
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(t), std::filesystem::u8path(f), ec);
}
static void index_load(int q) {
    if (loaded[q].load()) return;
    std::lock_guard<std::mutex> lg(ilm);
    if (loaded[q].load()) return;
    struct Done { int q; ~Done() { loaded[q] = true; } } done{q};
    FILE* f = fopen((dir(q) + "/index.txt").c_str(), "r");
    if (!f) return;
    char line[512];
    int n = 0, ver = 0;
    std::vector<std::pair<uint64_t, Src>> v;
    while (fgets(line, sizeof line, f)) {
        int s, c; Src e;
        if (!strncmp(line, "version ", 8)) ver = atoi(line + 8);
        else if (sscanf(line, "%d %d %f %f %f %f %f %f %f %f %f %f", &s, &c, &e.w, &e.h, &e.p[0], &e.p[1], &e.p[2], &e.p[3], &e.p[4], &e.p[5], &e.p[6], &e.tw) >= 11) { v.push_back({key(q, s, c), e}); if (s == REF_SYS && c == 0) { memcpy(ref, e.p, sizeof ref); ref_ok = true; } }
    }
    fclose(f);
    if (ver != INDEX_VER) { printf("render: indice q%d de otra version (%d): se generan de nuevo\n", q, ver); return; }
    { std::lock_guard<std::mutex> g(mx); for (auto& e : v) { pts[e.first] = e.second; n++; } }
    printf("render: indice q%d, %d renders\n", q, n);
}
}

bool psp_render_have(int s, int c, int q) {   // made, and with the tweak the file now gives this character
    gr::index_load(q);
    float sig = gr::tweak_sig(gr::tweak_of(s));
    std::lock_guard<std::mutex> g(gr::mx);
    auto it = gr::pts.find(gr::key(q, s, c));
    return it != gr::pts.end() && std::fabs(it->second.tw - sig) < 1e-3f;
}
uint32_t psp_render_tweaks_gen() { gr::tweaks_check(); std::lock_guard<std::mutex> g(gr::twm); return gr::tweaks_gen; }
float psp_sel3d_head(int s) { return gr::tweak_of(s).shead; }
void psp_render_tweak(int s, float* v) { rf::Tweak t = gr::tweak_of(s); v[0] = t.zoom; v[1] = t.dy; v[2] = t.dx; v[3] = t.bdy; v[4] = t.bzoom; v[5] = t.rup; }
int psp_render_busy() { return gr::busy.load(); }
// one capture: px is the screen rect (w x h) with the big box plus its margin; pts = head x, y, waist x, y in its pixels, then
// (pts[4], pts[5]) the mouth bone, -1 when the model has none, and (pts[8]) the head to waist distance over Goku's
void psp_render_save(int s, int c, int q, std::vector<uint32_t>&& px, int w, int h, const float* p) {
    gr::index_load(q);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(gr::dir(q)), ec);
    gr::Src e; e.w = (float)w; e.h = (float)h; e.tw = gr::tweak_sig(gr::tweak_of(s)); e.p[0] = p[0]; e.p[1] = p[1]; e.p[2] = p[2]; e.p[3] = p[3]; e.p[4] = p[4]; e.p[5] = p[5]; e.p[6] = p[8] > 0.1f ? p[8] : 1.0f;
    { std::lock_guard<std::mutex> g(gr::mx); gr::pts[gr::key(q, s, c)] = e; }
    if (s == gr::REF_SYS && c == 0) { memcpy(gr::ref, e.p, sizeof gr::ref); gr::ref_ok = true; }
    gr::busy++;
    auto img = std::make_shared<gr::Img>();
    img->w = w; img->h = h; img->px = std::move(px);
    std::thread([s, c, q, img, e] {
        auto t0 = std::chrono::steady_clock::now();
        int n = gr::derive(q, s, c, *img, e.p);
        std::error_code ec2;
        std::filesystem::create_directories(std::filesystem::u8path(gr::dir(q) + "/cap"), ec2);
        gr::save(gr::cap_file(q, s, c), *img);   // kept for the render editor
        gr::index_write(q);
        gr::epoch++;
        static std::atomic<int> logs{0}, wl{0};
        if (img->w < gr::BIG_W * q * 1.06f && wl++ < 1) { printf("render: la captura mide %dx%d, menos que el big x%d (%dx%d): sube la resolucion interna para mas detalle\n", img->w, img->h, q, gr::BIG_W * q, gr::BIG_H * q); fflush(stdout); }
        if (logs++ < 40) { printf("render: guardado personaje %d color %d q%d: %d archivos en %.0f ms, captura %dx%d, cabeza (%.0f, %.0f), cintura (%.0f, %.0f), cuello (%.0f, %.0f), ojos y %.0f, distancia relativa %.2f\n", s, c + 1, q, n,
                                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), img->w, img->h, e.p[0], e.p[1], e.p[2], e.p[3], e.p[4], e.p[5], rf::eye_y(e.p[1], e.p[5], gr::span_of(e.p), e.p[6]), e.p[6]); fflush(stdout); }
        gr::busy--;
    }).detach();
}
#ifndef RENDERS_HARNESS
// ---- the generated files served instead of the game's textures (Graphics > Renders on): big render per menu number in the
// colour shown, mid and small atlas pages with every cell redrawn, HUD 1, HUD 2 and beam clash per colour. Nothing is written
// over the game or the mods: the GPU asks for each texture's pixels here first, so turning it off gives back the originals
int psp_sel_colour_of(int menu);
void psp_sel_images_warm();
namespace gr {
struct Ov { int kind = -2, s = -1, c = 0, page = 0, menu = -1; };
static thread_local bool bg = false;
static std::atomic<bool> bg_icons_done{false};   // built by the select images worker (text_names.h)   // kind: -1 none, 0 big, 1 mid page, 2 small page, 3 HUD 1, 4 HUD 2, 5 beam clash
static std::mutex ovm;
static std::map<std::string, std::pair<int, int>> colkeys;   // colour container -> sysid, colour
static bool colkeys_ok = false;
static int on_q() {   // 0 off, else the quality
    static int v = 0, n = 0;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_GEN_RENDERS"); const char* f = getenv("TTT_GEN_RENDERS_Q"); int q = f && *f ? atoi(f) : 2; v = e && *e && atoi(e) ? (q >= 4 ? 4 : q >= 2 ? 2 : 1) : 0; }
    return v;
}
static int sys_of_menu(int n) {
    if (n >= 0 && n < roster::K_OLD) return roster::VANILLA[n];
    for (int s = 0; s < roster::ALL; s++) if (!roster::vanilla(s) && roster::menu_of(s) == n && roster::find(s)) return s;
    return -1;
}
static void colkeys_make() {
    colkeys_ok = true;
    auto add = [&](int s, size_t first) {
        auto it = col_list.find((uint32_t)first);
        if (it == col_list.end()) return;
        for (size_t k = 0; k < it->second.size(); k++) {
            if (it->second[k] >= disc_pk.size()) continue;
            const PkEntry& x = disc_pk[it->second[k]];
            std::string ck = x.folder || !ends_with(lower(x.fpath), ".pak") ? x.key : x.fpath.substr(0, x.fpath.size() - 4);   // a closed colour pak: its name without .pak
            if (!ck.empty()) colkeys[lower(ck)] = {s, (int)k};
        }
    };
    for (int s = 0; s < roster::ALL; s++) {
        int k = roster::group_vanilla(s);
        if (k >= 0) add(s, COL_MODEL - 1 + 5 * (size_t)k);
        else if (ro_first != (size_t)-1 && roster::find(s) && roster::rank(s) >= 0) add(s, ro_first + (roster::model_entry(roster::rank(s)) - roster::MODEL_BASE));
    }
    printf("render: %d contenedores de color conocidos para los HUD\n", (int)colkeys.size());
}
static Ov classify(uint32_t id) {
    TexPng& t = tex_reg[id - 1];
    { int k = __atomic_load_n(&t.gk, __ATOMIC_ACQUIRE); if (k != -2) return {k, t.gs, t.gc, t.gp, t.gm}; }
    std::lock_guard<std::mutex> g(ovm);
    if (t.gk != -2) return {t.gk, t.gs, t.gc, t.gp, t.gm};
    Ov o; o.kind = -1;
    std::string k = lower(t.pak), r = lower(link_real(t.fpath));   // the container's path and the real file (Shared targets of the pak links)
    auto ends = [](const std::string& x, const char* e) { size_t n = strlen(e); return x.size() >= n && x.compare(x.size() - n, n, e) == 0; };
    auto base = [](const std::string& x) { return x.substr(x.find_last_of('/') + 1); };
    auto has = [&](const char* e) { return k.find(e) != std::string::npos || r.find(e) != std::string::npos; };
    if (has("/char_select/renders/")) {
        for (const std::string& x : {base(k), base(r)}) {
            if (x.empty() || !isdigit((unsigned char)x[0])) continue;
            int e = atoi(x.c_str());
            if (e >= 184 && e < 184 + 0x46) o.menu = e - 184;
            for (auto& bg : ro_big) if ((int)bg.second[2] == e) o.menu = bg.first;
            if (o.menu >= 0) break;
        }
        if (o.menu >= 0) o.kind = 0;
    } else if (has("/char_renders_mid/char_renders_mid") && has(".rhg")) { o.kind = 1; o.page = atoi(t.name.c_str()); }   // also a language-suffixed Shared name
    else if (has("/char_select/char_renders_small") && has(".rhg")) { o.kind = 2; o.page = atoi(t.name.c_str()); }
    else if (ends(k, "/1_p/045_hud_1.rhg") || ends(k, "/1_p/047_hud_2.rhg") || ends(k, "/1_p/049_hud_clash.rhg")) {
        if (!colkeys_ok) colkeys_make();
        std::string ck = k.substr(0, k.find("/1_p/"));
        size_t dp = ck.find("#dmg");
        if (dp != std::string::npos) ck.erase(dp, 4);
        auto it = colkeys.find(ck);
        if (it != colkeys.end()) { o.s = it->second.first; o.c = it->second.second + (dp != std::string::npos ? DMG : 0); o.kind = ends(k, "045_hud_1.rhg") ? 3 : ends(k, "047_hud_2.rhg") ? 4 : 5; }
        static int logs = 0;
        if (it == colkeys.end() && logs++ < 4) printf("render: HUD sin personaje conocido: %s\n", t.pak.c_str());
    }
    static int hl = 0;
    if (k.find("/1_p") != std::string::npos && (k.find("hud") != std::string::npos || r.find("hud") != std::string::npos) && hl++ < 40)
        printf("render: textura de HUD %s :: %s (%s, %dx%d) -> tipo %d, personaje %d, color %d\n", t.pak.c_str(), t.name.c_str(), r.c_str(), t.width0, t.height0, o.kind, o.s, o.c + 1);
    static int logs = 0;
    if ((o.kind == 0 || o.kind == 1 || o.kind == 2 || (o.kind < 0 && (k.find("render") != std::string::npos || r.find("render") != std::string::npos))) && logs++ < 24)
        printf("render: textura %s :: %s (%s) -> tipo %d, numero %d, pagina %d\n", t.pak.c_str(), t.name.c_str(), r.c_str(), o.kind, o.menu, o.page);
    t.gs = o.s; t.gc = o.c; t.gp = o.page; t.gm = o.menu;
    __atomic_store_n(&t.gk, o.kind, __ATOMIC_RELEASE);
    return o;
}
// decoded images kept by size (TTT_RENDER_CACHE_MB, default 768; 0 = the old 48 entries), least recently used out first
static std::map<std::string, std::pair<std::shared_ptr<Img>, uint64_t>> cache;
static std::list<std::string> cache_order;
static size_t cache_bytes = 0;
static size_t cache_cap() {
    static long mb = -1;
    if (mb < 0) { const char* e = getenv("TTT_RENDER_CACHE_MB"); mb = e && *e ? atol(e) : 768; }
    return (size_t)mb << 20;
}
static size_t img_bytes(const Img& m) { return m.px.size() * 4 + 64; }
static void cache_touch(const std::string& k) { cache_order.remove(k); cache_order.push_back(k); }
static std::shared_ptr<Img> cache_get(const std::string& k) {
    std::lock_guard<std::mutex> g(mx);
    auto it = cache.find(k);
    if (it == cache.end()) return nullptr;
    if (cache_cap()) cache_touch(k);
    return it->second.first;
}
static void cache_put(const std::string& k, std::shared_ptr<Img> m) {
    std::lock_guard<std::mutex> g(mx);
    auto it = cache.find(k);
    if (it != cache.end()) { cache_bytes -= it->second.second; cache_order.remove(k); }
    uint64_t b = m ? img_bytes(*m) : 0;
    cache[k] = {m, b}; cache_bytes += b; cache_order.push_back(k);
    size_t cap = cache_cap();
    static int logs = 0;
    while (cache_order.size() > 1 && (cap ? cache_bytes > cap : cache_order.size() > 48)) {
        auto o = cache.find(cache_order.front());
        if (o != cache.end()) { cache_bytes -= o->second.second; cache.erase(o); }
        cache_order.pop_front();
        if (cap && logs++ < 4) { printf("render: cache de imagenes llena (%zu MB), se descarta la mas antigua\n", cap >> 20); fflush(stdout); }
    }
}
static void psp_alpha(Img& m) { for (auto& p : m.px) p = (p & 0x00FFFFFFu) | ((((p >> 24) + 1) >> 1) << 24); }
static std::shared_ptr<Img> raw_img(int q, int s, int c, int k) {   // the file as decoded (the prefetch fills these)
    std::string key = "raw" + fver_key(file(q, s, c, k));
    if (auto m = cache_get(key)) return m;
    auto m = std::make_shared<Img>();
    if (!load(file(q, s, c, k), *m)) return nullptr;
    m->uid = img_uid++;
    cache_put(key, m);
    return m;
}
static std::shared_ptr<Img> file_img(int q, int s, int c, int k, bool psp) {
    if (!psp) return raw_img(q, s, c, k);
    std::string key = "psp" + fver_key(file(q, s, c, k));
    if (auto m = cache_get(key)) return m;
    auto r = raw_img(q, s, c, k);
    if (!r) return nullptr;
    auto m = std::make_shared<Img>(*r);
    m->uid = img_uid++;
    psp_alpha(*m);
    cache_put(key, m);
    return m;
}
// the select and the game room change renders every few frames (Random): every colour 1 big, mid and small file is
// decoded once in the background when the renders are first served, so the game thread finds them in memory
// a big render stretched to the texture the game declares (game size x quality): one per image and size, made in the
// background for every colour 1 big once the first one shows the size (the Random of the game room asks many per second)
static std::string fit_key(const Img& m, int gw, int gh) { return "fit|" + std::to_string(m.uid) + "|" + std::to_string(gw) + "x" + std::to_string(gh); }
static std::shared_ptr<Img> fit_img(const std::shared_ptr<Img>& m, int gw, int gh) {
    std::string k = fit_key(*m, gw, gh);
    auto f = cache_get(k);
    if (!f) { f = std::make_shared<Img>(resample(*m, 0, 0, (float)m->w, (float)m->h, gw, gh)); f->uid = img_uid++; cache_put(k, f); }
    return f;
}
static std::atomic<uint32_t> pf_done{0};
static std::atomic<bool> pf_ready{false};
static std::atomic<uint64_t> fit_done{0};
static void prefit(int q, bool psp, int gw, int gh) {
    uint64_t want = (uint64_t)epoch.load() << 40 | (uint64_t)q << 36 | (uint64_t)psp << 35 | (uint64_t)gw << 16 | (uint64_t)gh;
    if (fit_done.load() == want) return;
    fit_done = want;
    std::vector<int> sys;
    { std::lock_guard<std::mutex> g(mx); for (auto& e : pts) if ((int)(e.first >> 40) == q && (e.first & 255) == 0) sys.push_back((int)((e.first >> 8) & 0xFFFFFFFFu)); }
    std::thread([q, psp, gw, gh, sys, want] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        for (int i = 0; i < 400 && !pf_ready.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto t0 = std::chrono::steady_clock::now();
        int n = 0;
        for (int s : sys) {
            if (fit_done.load() != want) return;
            if (auto m = file_img(q, s, 0, K_BIG, psp)) if (m->w != gw || m->h != gh) { fit_img(m, gw, gh); n++; }
        }
        printf("render: big ajustados a %dx%d en segundo plano, %d en %d ms\n", gw, gh, n,
               (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        fflush(stdout);
    }).detach();
}
static void prefetch(int q) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_RENDER_PREFETCH"); on = !(e && *e == '0') && cache_cap() != 0; }
    uint32_t want = epoch.load() << 3 | (uint32_t)q;
    if (!on || pf_done.load() == want) return;
    pf_done = want;
    index_load(q);
    std::vector<int> sys;
    { std::lock_guard<std::mutex> g(mx); for (auto& e : pts) if ((int)(e.first >> 40) == q && (e.first & 255) == 0) sys.push_back((int)((e.first >> 8) & 0xFFFFFFFFu)); }
    std::thread([q, sys, want] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        auto t0 = std::chrono::steady_clock::now();
        int n = 0;
        for (int s : sys) for (int k : {K_BIG, K_MID, K_SMALL}) {
            if (pf_done.load() != want) return;
            if (raw_img(q, s, 0, k)) n++;
        }
        size_t mb; { std::lock_guard<std::mutex> g(mx); mb = cache_bytes >> 20; }
        pf_ready = true;
        printf("render: precarga q%d, %d imagenes de %d personajes en %d ms (cache %zu MB)\n", q, n, (int)sys.size(),
               (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(), mb);
        fflush(stdout);
    }).detach();
}
static bool have(int s, int c, int q) { index_load(q); std::lock_guard<std::mutex> g(mx); return pts.count(key(q, s, c)) != 0; }   // served before the select ever loaded the index (Character Reference first)
// an atlas page with the generated cell of every menu number that draws from it (the AGS state of that number), q times the game's size
static std::shared_ptr<Img> page_img(uint32_t id, const Ov& o, int q) {
    const TexPng& t = tex_reg[id - 1];
    std::string key = std::to_string(epoch.load()) + "|page|" + std::to_string(o.kind) + "|" + std::to_string(o.page) + "|" + std::to_string(q);
    if (auto m = cache_get(key)) return m;
    static std::vector<uint8_t> ags_d[3];
    static std::vector<ags::State> ags_s[3];
    {
        static std::mutex im;
        std::lock_guard<std::mutex> g(im);
        if (ags_d[o.kind].empty()) {
            const char* nm = o.kind == 1 ? "015_char_renders_mid.ags" : "017_char_renders_small.ags";
            std::string f = disc_dir + "/cache/roster/en_" + nm;
            if (!is_file(f)) f = link_real(disc_dir + "/Shared/" + psp_shared_rel("Menus/char_select/char_select_en.pak") + "/1_interface/" + nm);
            if (!read_file(f, ags_d[o.kind], true) || !ags::parse(ags_d[o.kind], ags_s[o.kind])) { ags_d[o.kind].clear(); printf("render: no se pudo leer %s\n", f.c_str()); return nullptr; }
            printf("render: %s, %d estados\n", nm, (int)ags_s[o.kind].size());
        }
    }
    auto t0 = std::chrono::steady_clock::now();
    TexPng* b = tex_load(id);
    if (!b) return nullptr;
    auto m = std::make_shared<Img>();
    Img orig; orig.w = b->w; orig.h = b->h; orig.px = b->px;
    int W = std::max(1, t.width0) * q, H = std::max(1, t.height0) * q;
    *m = resample(orig, 0, 0, (float)orig.w, (float)orig.h, W, H);
    int cells = 0;
    for (int n = 0; n < (int)ags_s[o.kind].size(); n++) {
        int tx, tw, th; float r[4];
        if (!ags::rect(ags_d[o.kind], ags_s[o.kind][n], &tx, &tw, &th, r) || tx != o.page - 1 || tw <= 0 || th <= 0) continue;
        int s = sys_of_menu(n);
        if (s < 0 || !have(s, 0, q)) continue;
        auto g = file_img(q, s, 0, o.kind == 1 ? K_MID : K_SMALL, false);
        if (!g) continue;
        float sx = (float)W / tw, sy = (float)H / th;
        int x0 = (int)(r[0] * sx + 0.5f), y0 = (int)(r[1] * sy + 0.5f), x1 = (int)(r[2] * sx + 0.5f), y1 = (int)(r[3] * sy + 0.5f);
        if (x1 <= x0 || y1 <= y0) continue;
        Img cell = resample(*g, 0, 0, (float)g->w, (float)g->h, x1 - x0, y1 - y0);
        if (t.psp) psp_alpha(cell);
        for (int y = 0; y < cell.h; y++) for (int x = 0; x < cell.w; x++) if (x0 + x < W && y0 + y < H) m->px[(size_t)(y0 + y) * W + x0 + x] = cell.px[(size_t)y * cell.w + x];
        cells++;
    }
    static int logs = 0;
    if (logs++ < 16) printf("render: pagina %d de %s con %d celdas generadas (%dx%d), %.0f ms %s\n", o.page, o.kind == 1 ? "mid" : "small", cells, W, H,
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), bg ? "en segundo plano" : "en el hilo del juego");
    cache_put(key, m);
    return m;
}
static int big_colour(const Ov& o, int q, int* s) {
    *s = sys_of_menu(o.menu);
    int c = psp_sel_colour_of(o.menu);
    if (*s >= 0 && c >= DMG && !have(*s, c, q)) c -= DMG;   // damaged costume without its own render: the colour's
    return *s >= 0 && c > 0 && have(*s, c, q) ? c : 0;
}
}

static uint32_t rov_variant(uint32_t id) {
    int q = gr::on_q();
    if (!q) return 0;
    gr::Ov o = gr::classify(id);
    if (o.kind < 0) return 0;
    uint32_t v = 0x80000000u | gr::epoch.load() << 8 | (uint32_t)q << 5;
    if (o.kind == 0) { int s, bc = gr::big_colour(o, q, &s); v |= ((uint32_t)(bc % gr::DMG + 1) & 31) | (bc >= gr::DMG ? 0x40000000u : 0u); }
    return v;
}
extern std::atomic<int> psp_tex_pin_req;   // gpu.cpp: the next GL texture is a render big, kept across battles
std::atomic<int> psp_hud_raw{0};
static const float HUD_FACE_ROOM = 1.1f;   // BT3 face: mid framing zoomed out by this much   // gpu.cpp (BT3 face): the HUD portraits without the TTT shape, the BT3 panel cuts them
static gr::Img hud_face(const gr::Img& cap, const gr::Src& e, const gr::Img& mask, int q, int s, int c);
static std::shared_ptr<gr::Img> hud_raw(int q, int s, int c, int k, bool psp) {
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<gr::Img>> made;
    std::string key = std::to_string(gr::epoch.load()) + "|" + std::to_string(q) + "|" + std::to_string(s) + "|" + std::to_string(c) + "|" + std::to_string(k) + "|" + std::to_string((int)psp);
    std::lock_guard<std::mutex> g(m);
    auto it = made.find(key);
    if (it != made.end()) return it->second;
    gr::Src e;
    { std::lock_guard<std::mutex> g2(gr::mx); auto f = gr::pts.find(gr::key(q, s, c)); if (f == gr::pts.end()) return nullptr; e = f->second; }
    gr::Img cap;
    const gr::Img* a = gr::asset(k == gr::K_HUD1 ? "hud_1_mask.png" : "hud_2_mask.png");
    if (!a) return nullptr;
    bool from_big = false;
    if (!gr::load(gr::cap_file(q, s, c), cap) || !cap.w) {   // renders made before the captures were kept: the capture rebuilt from the big file
        auto b = gr::raw_img(q, s, c, gr::K_BIG);
        if (!b || !b->w || e.w < 2 || e.h < 2) { static int nl = 0; if (nl++ < 6) printf("render: retrato del HUD BT3 no disponible, personaje %d color %d sin captura ni big\n", s, c + 1); return nullptr; }
        cap.w = (int)e.w; cap.h = (int)e.h; cap.px.assign((size_t)cap.w * cap.h, 0);
        float x0, y0, bw, bh;
        rf::capture_box(e.w, e.h, &x0, &y0, &bw, &bh);
        gr::edit_box(gr::edit_of(s, c, gr::K_BIG), &x0, &y0, &bw, &bh);
        int iw = (int)lroundf(bw), ih = (int)lroundf(bh), ix = (int)lroundf(x0), iy = (int)lroundf(y0);
        if (iw > 0 && ih > 0 && iw <= 8192 && ih <= 8192) {
            gr::Img r = gr::resample(*b, 0, 0, (float)b->w, (float)b->h, iw, ih);
            for (int y = 0; y < ih; y++) for (int x = 0; x < iw; x++) {
                int cx = ix + x, cy = iy + y;
                if (cx >= 0 && cy >= 0 && cx < cap.w && cy < cap.h) cap.px[(size_t)cy * cap.w + cx] = r.px[(size_t)y * iw + x];
            }
        }
        from_big = true;
    }
    auto o = std::make_shared<gr::Img>(hud_face(cap, e, *a, q, s, c));
    if (psp) gr::psp_alpha(*o);
    o->uid = gr::img_uid++;
    made[key] = o;
    static int logs = 0;
    if (logs++ < 6) { printf("render: retrato del HUD BT3 con encuadre mid personaje %d color %d (%dx%d)%s\n", s, c + 1, o->w, o->h, from_big ? " desde el big" : ""); fflush(stdout); }
    return o;
}
static gr::Img hud_face_(const gr::Img& cap, const gr::Src& e, const gr::Img& mask, int q, int s, int c) {
    const gr::Img* a = &mask;
    int fw = a->w * q, fh = a->h * q;   // the mid render's framing (more room around the face than the HUD one), a little inside the BT3 face window
    float x0, y0, bw, bh, sp = gr::span_of(e.p);
    rf::face(rf::MID, e.p[0], rf::eye_y(e.p[1], e.p[5], sp, e.p[6]), sp, e.p[6], fw, fh, &x0, &y0, &bw, &bh);
    rf::tweak_box(gr::tweak_of(s), &x0, &y0, &bw, &bh, fw, fh);
    gr::edit_box(gr::edit_of(s, c, gr::K_MID), &x0, &y0, &bw, &bh);
    float cx = x0 + bw * 0.5f, cy = y0 + bh * 0.5f;
    bh *= HUD_FACE_ROOM; bw = bh * fw / fh;
    x0 = cx - bw * 0.44f; y0 = cy - bh * 0.5f;
    gr::Img f = gr::resample(cap, x0, y0, x0 + bw, y0 + bh, fw, fh);
    gr::Img o;
    o.w = 64 * q; o.h = 32 * q; o.px.assign((size_t)o.w * o.h, 0);
    int dy = std::max(0, o.h - f.h);
    for (int y = 0; y < std::min(f.h, o.h); y++) memcpy(&o.px[(size_t)(y + dy) * o.w], &f.px[(size_t)y * f.w], 4 * (size_t)std::min(f.w, o.w));
    return o;
}
static gr::Img hud_face(const gr::Img& cap, const gr::Src& e, const gr::Img& mask, int q, int s, int c) { return hud_face_(cap, e, mask, q, s, c); }
static bool rov_serve(uint32_t id, int* w, int* h, const uint32_t** px) {
    static thread_local std::shared_ptr<gr::Img> hold;
    int q = gr::on_q();
    if (!q) return false;
    gr::Ov o = gr::classify(id);
    if (o.kind < 0) return false;
    gr::prefetch(q);
    bool psp = tex_reg[id - 1].psp;
    std::shared_ptr<gr::Img> m;
    if (o.kind == 0) {
        int s, c = gr::big_colour(o, q, &s);
        if (s >= 0 && gr::have(s, c, q)) m = gr::file_img(q, s, c, gr::K_BIG, psp);
        static std::atomic<int> nl{0};
        if (!m && nl++ < 6) printf("render: big del numero %d sin render generado (personaje %d, color %d)\n", o.menu, s, c + 1);
    }
    else if (o.kind <= 2) { psp_sel_images_warm(); m = gr::page_img(id, o, q); }
    else {
        int oc = o.c;
        if (o.c >= gr::DMG && !gr::have(o.s, o.c, q)) o.c -= gr::DMG;   // damaged costume without its own render: the colour's
        if (oc >= gr::DMG) {
            static std::atomic<int> dl{0};
            if (dl++ < 6) { printf("render: HUD del traje danado, personaje %d color %d: %s\n", o.s, oc - gr::DMG + 1, o.c >= gr::DMG ? "render del traje danado" : gr::have(o.s, o.c, q) ? "render del color (sin render del traje danado)" : "sin render generado"); fflush(stdout); }
        }
    }
    if (o.kind >= 3 && gr::have(o.s, o.c, q)) {
        int k = o.kind == 3 ? gr::K_HUD1 : o.kind == 4 ? gr::K_HUD2 : tex_reg[id - 1].width0 >= 256 ? gr::K_CLASH2 : gr::K_CLASH1;
        m = psp_hud_raw.load() && (k == gr::K_HUD1 || k == gr::K_HUD2) ? hud_raw(q, o.s, o.c, k, psp) : nullptr;
        if (!m) m = gr::file_img(q, o.s, o.c, k, psp);
    }
    if (o.kind >= 3) {
        static std::atomic<int> hl{0};
        if (hl++ < 30) { printf("render: HUD pedido %s, personaje %d color %d tipo %d: %s\n", tex_reg[id - 1].pak.c_str(), o.s, o.c + 1, o.kind, m && m->w ? "servido el generado" : gr::have(o.s, o.c, q) ? "archivo del generado no leido" : "sin render generado"); fflush(stdout); }
    }
    if (!m || !m->w) return false;
    int gw = std::max(1, tex_reg[id - 1].width0) * q, gh = std::max(1, tex_reg[id - 1].height0) * q;
    if (m->w != gw || m->h != gh) {   // the game size times the quality (the texel fit of tex_png_buscar pads it to the GE size)
        static int pre = -1;
        if (pre < 0) { const char* e = getenv("TTT_RENDER_PREFETCH"); pre = !(e && *e == '0'); }
        if (o.kind == 0 && pre) gr::prefit(q, psp, gw, gh);
        static std::atomic<int> nl{0};
        if (!gr::cache_get(gr::fit_key(*m, gw, gh)) && nl++ < 8) { printf("render: big %dx%d ajustado ahora a %dx%d (no estaba listo)\n", m->w, m->h, gw, gh); fflush(stdout); }
        m = gr::fit_img(m, gw, gh);
    }
    hold = m;
    *w = m->w; *h = m->h; *px = m->px.data();
    if (o.kind == 0) psp_tex_pin_req.store(1);
    static std::atomic<int> logs{0};
    if (logs++ < 12) { printf("render: servido %s :: %s como render generado (%dx%d, juego %dx%d)\n", tex_reg[id - 1].pak.c_str(), tex_reg[id - 1].name.c_str(), m->w, m->h, tex_reg[id - 1].width0, tex_reg[id - 1].height0); fflush(stdout); }
    return true;
}
// game room 3D (item 33): the menu number of a big render texture the GPU is about to draw, -1 for any other texture
extern int (*psp_tex_bigmenu)(uint32_t addr, int* w, int* h);
static int rov_bigmenu(uint32_t addr, int* w, int* h) {
    if (addr < 0x08000000u || addr >= 0x10000000u) return -1;
    const uint8_t* d = psp_p(addr);
    if (memcmp(d, TEX_MARKER, 8)) return -1;
    uint32_t id, chk;
    memcpy(&id, d + 8, 4); memcpy(&chk, d + 12, 4);
    if (chk != ~id || !id || id > tex_reg.size()) return -1;
    gr::Ov o = gr::classify(id);
    if (o.kind != 0) return -1;
    *w = tex_reg[id - 1].width0; *h = tex_reg[id - 1].height0;
    return o.menu;
}
static const bool rov_reg = (psp_tex_bigmenu = rov_bigmenu, true);

// ---- render editor (ui/render_editor.h): the saved captures cut again with the zoom and move of each output
static std::atomic<int> ed_req{0}, ed_done{0}, ed_total{0};
void psp_render_editor_request() { ed_req = 1; }
bool psp_render_editor_take() { if (!ed_req.load() || gr::busy.load() > 0) return false; ed_req = 0; return true; }   // after the last capture is saved
int psp_render_quality() { const char* e = getenv("TTT_GEN_RENDERS_Q"); int q = e && *e ? atoi(e) : 2; return q >= 4 ? 4 : q >= 2 ? 2 : 1; }
int psp_render_list(int q, std::vector<int>& sc) {   // sysid, colour pairs with a capture, by sysid
    gr::index_load(q);
    sc.clear();
    std::vector<std::pair<int, int>> v;
    { std::lock_guard<std::mutex> g(gr::mx); for (auto& e : gr::pts) if ((int)(e.first >> 40) == q) v.push_back({(int)((e.first >> 8) & 0xFFFFFFFFu), (int)(e.first & 255)}); }
    std::sort(v.begin(), v.end());
    for (auto& x : v) if (is_file(gr::cap_file(q, x.first, x.second))) { sc.push_back(x.first); sc.push_back(x.second); }
    return (int)sc.size() / 2;
}
std::string psp_render_name(int s) {   // the colour folder of the character, readable
    { std::lock_guard<std::mutex> g(gr::ovm); if (!gr::colkeys_ok) gr::colkeys_make(); }
    std::string best;
    for (auto& e : gr::colkeys) if (e.second.first == s && e.second.second == 0) { best = e.first; break; }
    if (best.compare(0, 11, "characters/") == 0) best = best.substr(11);
    size_t c = best.rfind("/color_");
    if (c != std::string::npos) best.resize(c);
    std::string out;
    bool up = true, skip = true;
    for (size_t i = 0; i < best.size(); i++) {
        char ch = best[i];
        if (skip && isdigit((unsigned char)ch)) continue;
        if (ch == '/' || ch == '_') { if (skip && ch == '_') { skip = false; continue; } out += ch == '/' ? " - " : " "; up = true; skip = ch == '/'; continue; }
        skip = false;
        out += up ? (char)toupper((unsigned char)ch) : ch; up = false;
    }
    return out.empty() ? "Character " + std::to_string(s) : out;
}
bool psp_render_edit(int s, int c, int k, float* v, bool set) {   // v = zoom, dx, dy; false when not stored (defaults returned)
    std::lock_guard<std::mutex> g(gr::edm);
    gr::edits_load();
    std::array<int, 3> key = {s, c, k};
    if (set) {
        if (std::fabs(v[0] - 1) < 1e-4f && std::fabs(v[1]) < 1e-4f && std::fabs(v[2]) < 1e-4f) gr::edits.erase(key);
        else gr::edits[key] = {v[0], v[1], v[2]};
        return true;
    }
    auto it = gr::edits.find(key);
    if (it == gr::edits.end()) { v[0] = 1; v[1] = 0; v[2] = 0; return false; }
    v[0] = it->second.z; v[1] = it->second.dx; v[2] = it->second.dy;
    return true;
}
void psp_render_edits_reload() { std::lock_guard<std::mutex> g(gr::edm); gr::edits.clear(); gr::edits_ok = false; }
void psp_render_edits_save() {
    std::lock_guard<std::mutex> g(gr::edm);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache/renders"), ec);
    FILE* f = fopen(gr::edits_file().c_str(), "w");
    if (!f) return;
    for (auto& e : gr::edits) {
        std::string a = e.first[0] < 0 ? "*" : std::to_string(e.first[0]), b = e.first[1] < 0 ? "*" : std::to_string(e.first[1]);
        fprintf(f, "%s %s %d %.4f %.4f %.4f\n", a.c_str(), b.c_str(), e.first[2], e.second.z, e.second.dx, e.second.dy);
    }
    fclose(f);
}
static bool ed_capture(int q, int s, int c, std::shared_ptr<gr::Img>& out, gr::Src& src) {
    static int ls = -1, lc = -1, lq = -1;
    static std::shared_ptr<gr::Img> last;
    { std::lock_guard<std::mutex> g(gr::mx); auto it = gr::pts.find(gr::key(q, s, c)); if (it == gr::pts.end()) return false; src = it->second; }
    if (!(ls == s && lc == c && lq == q && last)) {
        auto m = std::make_shared<gr::Img>();
        if (!gr::load(gr::cap_file(q, s, c), *m)) return false;
        last = m; ls = s; lc = c; lq = q;
    }
    out = last;
    return true;
}
bool psp_render_big_gen(int s, int c, std::vector<uint32_t>& px, int* w, int* h) {   // the generated big render, Renders on
    int q = gr::on_q();
    if (!q || s < 0) return false;
    if (c >= gr::DMG && !gr::have(s, c, q)) c -= gr::DMG;
    if (!gr::have(s, c, q)) return false;
    auto m = gr::file_img(q, s, c, gr::K_BIG, false);
    if (!m || m->w < 2 || m->h < 2) return false;
    px = m->px; *w = m->w; *h = m->h;
    return true;
}
bool psp_render_preview(int q, int s, int c, int k, std::vector<uint32_t>& px, int* w, int* h) {   // k = 0 big .. 5 beam clash (type 1)
    std::shared_ptr<gr::Img> img; gr::Src e;
    if (!ed_capture(q, s, c, img, e)) return false;
    int gk = k >= 5 ? gr::K_CLASH1 : k;
    gr::Img m = gr::output(gk, q, *img, e.p, gr::tweak_of(s), gr::edit_of(s, c, gk));
    if (!m.w) return false;
    px = std::move(m.px); *w = m.w; *h = m.h;
    return true;
}
// the part of the big render the game room shows (its AGS is not the select's), learned from the game's own draw (gpu.cpp)
extern void (*psp_room_cut)(float u0, float v0, float u1, float v1, float aspect);
static std::mutex room_cut_mx;
static float room_cut_v[5] = {0, 0, 0, 0, 0};
static bool room_cut_read = false;
static void room_cut_load() {   // room_cut_mx held
    if (room_cut_read) return;
    room_cut_read = true;
    if (FILE* f = fopen((disc_dir + "/cache/renders/room_cut.txt").c_str(), "r")) { if (fscanf(f, "%f %f %f %f %f", &room_cut_v[0], &room_cut_v[1], &room_cut_v[2], &room_cut_v[3], &room_cut_v[4]) != 5) room_cut_v[4] = 0; fclose(f); }
}
static void room_cut_set(float u0, float v0, float u1, float v1, float aspect) {
    std::lock_guard<std::mutex> g(room_cut_mx);
    room_cut_load();
    float n[5] = {u0, v0, u1, v1, aspect};
    bool same = true;
    for (int i = 0; i < 5; i++) same = same && fabsf(n[i] - room_cut_v[i]) < 0.004f;
    if (same || u1 - u0 < 0.05f || v1 - v0 < 0.05f || aspect <= 0) return;
    memcpy(room_cut_v, n, sizeof n);
    std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache/renders"), ec);
    if (FILE* f = fopen((disc_dir + "/cache/renders/room_cut.txt").c_str(), "w")) { fprintf(f, "%f %f %f %f %f\n", u0, v0, u1, v1, aspect); fclose(f); }
    printf("render: recorte del big en la sala de juego: u %.3f-%.3f, v %.3f-%.3f, proporcion %.2f\n", u0, u1, v0, v1, aspect); fflush(stdout);
}
static const bool room_cut_reg = (psp_room_cut = room_cut_set, true);
bool psp_sz_face_cut(const std::vector<uint32_t>& src, int w, int h, int side, float shift, std::vector<uint32_t>& out, int* fs);
bool psp_bt3_face_cut(const std::vector<uint32_t>& fc, int pw, int ph, const float* g, std::vector<uint32_t>& out, int* ow, int* oh);
float psp_render_big_dx(int s, int c) { return gr::edit_of(s, c, gr::K_BIG).dx; }   // the editor's horizontal move of the big: the SZ portrait follows it
// where the BT3 bars draw the portrait (screen rect and its texture part), learned from a battle (bt3_hud.inc)
static float bt3_geo_v[8] = {-1, 63, 3, 35, 0, 1, 0, 1};   // measured on his machine (s144), replaced by what a battle reports
static bool bt3_geo_read = false;
static void bt3_geo_load() {   // room_cut_mx held
    if (bt3_geo_read) return;
    bt3_geo_read = true;
    if (FILE* f = fopen((disc_dir + "/cache/renders/bt3_face.txt").c_str(), "r")) { float* v = bt3_geo_v; if (fscanf(f, "%f %f %f %f %f %f %f %f", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7) != 8) v[1] = v[0]; fclose(f); }
}
void psp_render_bt3_geo(const float* g) {
    std::lock_guard<std::mutex> lk(room_cut_mx);
    bt3_geo_load();
    bool same = true;
    for (int i = 0; i < 8; i++) same = same && fabsf(g[i] - bt3_geo_v[i]) < 0.01f;
    if (same || g[1] - g[0] < 4 || g[3] - g[2] < 4) return;
    memcpy(bt3_geo_v, g, sizeof bt3_geo_v);
    std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache/renders"), ec);
    if (FILE* f = fopen((disc_dir + "/cache/renders/bt3_face.txt").c_str(), "w")) { fprintf(f, "%f %f %f %f %f %f %f %f\n", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]); fclose(f); }
    printf("render: retrato de las barras BT3 en pantalla (%.2f, %.2f)-(%.2f, %.2f), textura u %.3f-%.3f v %.3f-%.3f\n", g[0], g[2], g[1], g[3], g[4], g[5], g[6], g[7]); fflush(stdout);
}
// the game room's part of the big render, read from the character's room AGS (char_select entry 112 + menu number; new characters use a copy of Goku's):
// every textured element is a strip of the big texture (clip points, texture pixels) drawn with its group's colour alpha (the faded ends)
struct RoomSlice { float x0, y0, x1, y1, a; };
static bool room_slices(int sys, std::vector<RoomSlice>& out, float* tw, float* th) {
    int m = roster::rank_vanilla(sys);
    int want = 112 + (m < 0 ? 0 : m);
    std::string rel = psp_shared_rel("Menus/char_select/renders_ags"), f;
    std::vector<DirEntry> v;
    list_dir_fs(disc_dir + "/Shared/" + rel, v);
    for (auto& e : v) {
        if (e.dir || !isdigit((unsigned char)e.name[0]) || atoi(e.name.c_str()) != want || !ends_with(lower(e.name), ".ags")) continue;
        f = psp_cr_file("Shared/" + rel + "/" + e.name);
        if (f.empty()) f = link_real(disc_dir + "/Shared/" + rel + "/" + e.name);
        break;
    }
    std::vector<uint8_t> d; std::vector<ags::State> S;
    if (f.empty() || !read_file(f, d, true) || !ags::parse(d, S) || S.empty()) return false;
    const ags::State& st = S[0];
    uint32_t h = 0x20, ng = ags::rd(d, h + 12), pinfo = st.p0 - 32 * ng, n1 = ags::rd(d, h + 28), nc = ags::rd(d, h + 44);
    uint32_t pc = ags::al16(ags::al16(st.p0 + 8 * st.n0) + 8 * n1 + 16 * ags::rd(d, h + 36));
    out.clear();
    for (auto& r : st.refs) {
        if (!ags::textured(d, r)) continue;
        uint32_t sz = ags::rd(d, r.pos + 8), g = ags::rd(d, r.body);
        *tw = (float)(sz & 0xFFFF); *th = (float)(sz >> 16);
        RoomSlice q = {1e9f, 1e9f, -1e9f, -1e9f, 1.0f};
        for (int c = 0; c < r.clips; c++) {
            uint32_t i = ags::rd(d, r.clip_pos + 32 * c);
            if (i >= st.n0) continue;
            float xy[2]; memcpy(xy, d.data() + st.p0 + 8 * i, 8);
            q.x0 = std::min(q.x0, xy[0]); q.y0 = std::min(q.y0, xy[1]); q.x1 = std::max(q.x1, xy[0]); q.y1 = std::max(q.y1, xy[1]);
        }
        if (g < ng) { uint32_t ci = ags::rd(d, pinfo + 32 * g + 12); if (ci < nc && pc + 16 * ci + 16 <= d.size()) memcpy(&q.a, d.data() + pc + 16 * ci + 12, 4); }
        if (q.x1 > q.x0 && q.y1 > q.y0 && *tw > 0 && *th > 0) out.push_back(q);
    }
    static int logs = 0;
    if (logs++ < 4 && !out.empty()) { float x0 = 1e9f, x1 = -1e9f; for (auto& q : out) { x0 = std::min(x0, q.x0); x1 = std::max(x1, q.x1); } printf("render: sala del personaje %d (%s): %d franjas, x %.0f-%.0f, y %.0f-%.0f de %.0fx%.0f\n", sys, f.c_str(), (int)out.size(), x0, x1, out[0].y0, out[0].y1, *tw, *th); fflush(stdout); }
    return !out.empty();
}
// read-only previews of the editor: 0 SZ HUD portrait (from the big), 1 BT3 HUD portrait (mid framing), 2 game room (from the big)
// 1 = made, 2 = made without the BT3 panel's cut (not known yet), 0 = no capture, -1 = that HUD mod is not installed, -2 = the game room's cut is not known yet
int psp_render_preview_extra(int q, int s, int c, int which, std::vector<uint32_t>& px, int* w, int* h) {
    if (which == 0 && psp_cr_file("SZHud/layout.txt").empty()) return -1;
    if (which == 1 && psp_cr_file("BT3Hud/common/gauge_06.png").empty()) return -1;
    std::shared_ptr<gr::Img> img; gr::Src e;
    if (!ed_capture(q, s, c, img, e)) return 0;
    if (which == 1) {
        const gr::Img* a = gr::asset("hud_2_mask.png");
        if (!a) return 0;
        gr::Img m = hud_face(*img, e, *a, q, s, c);
        float g[8];
        { std::lock_guard<std::mutex> lk(room_cut_mx); bt3_geo_load(); memcpy(g, bt3_geo_v, sizeof g); }
        std::vector<uint32_t> o; int cw = 0, ch = 0;
        if (g[1] - g[0] >= 4 && psp_bt3_face_cut(m.px, m.w, m.h, g, o, &cw, &ch)) { px = std::move(o); *w = cw; *h = ch; return 1; }
        px = std::move(m.px); *w = m.w; *h = m.h;
        return 2;
    }
    gr::Img b = gr::output(gr::K_BIG, q, *img, e.p, gr::tweak_of(s), gr::edit_of(s, c, gr::K_BIG));
    if (!b.w) return 0;
    if (which == 0) {
        std::vector<uint32_t> o; int fs = 0;
        if (!psp_sz_face_cut(b.px, b.w, b.h, 0, gr::edit_of(s, c, gr::K_BIG).dx, o, &fs)) return -1;
        o.resize((size_t)fs * fs);
        px = std::move(o); *w = *h = fs;
        return 1;
    }
    std::vector<RoomSlice> sl; float tw = 0, th = 0;
    if (room_slices(s, sl, &tw, &th)) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (auto& q : sl) { x0 = std::min(x0, q.x0); y0 = std::min(y0, q.y0); x1 = std::max(x1, q.x1); y1 = std::max(y1, q.y1); }
        float kx = b.w / tw, ky = b.h / th;
        int ow = std::max(2, (int)lroundf((x1 - x0) * kx)), oh = std::max(2, (int)lroundf((y1 - y0) * ky));
        gr::Img r = gr::resample(b, x0 * kx, y0 * ky, x1 * kx, y1 * ky, ow, oh);
        std::vector<float> al((size_t)ow, 0.0f);
        for (auto& q : sl) for (int x = std::max(0, (int)floorf((q.x0 - x0) * kx)); x < std::min(ow, (int)ceilf((q.x1 - x0) * kx)); x++) al[x] = std::max(al[x], std::min(1.0f, std::max(0.0f, q.a)));
        for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) { uint32_t& p = r.px[(size_t)y * ow + x]; p = (p & 0xFFFFFFu) | (uint32_t)lroundf((p >> 24) * al[x]) << 24; }
        px = std::move(r.px); *w = ow; *h = oh;
        return 1;
    }
    float v[5];
    { std::lock_guard<std::mutex> g(room_cut_mx); room_cut_load(); memcpy(v, room_cut_v, sizeof v); }
    if (v[4] <= 0) return -2;
    int x0 = std::max(0, (int)lroundf(v[0] * b.w)), x1 = std::min(b.w, (int)lroundf(v[2] * b.w)), y0 = std::max(0, (int)lroundf(v[1] * b.h)), y1 = std::min(b.h, (int)lroundf(v[3] * b.h));
    if (x1 - x0 < 2 || y1 - y0 < 2) return -2;
    int ow = x1 - x0, oh = std::max(1, (int)lroundf(ow / v[4]));
    gr::Img r = gr::resample(b, (float)x0, (float)y0, (float)x1, (float)y1, ow, oh);
    px = std::move(r.px); *w = r.w; *h = r.h;
    return 1;
}
int psp_render_apply_progress(int* total) { *total = ed_total.load(); return ed_done.load(); }
void psp_render_apply(int q) {   // the captures whose cuts changed since the last save of edits.txt, cut again (own thread)
    std::vector<int> all, sc, mk;
    int na = psp_render_list(q, all);
    std::map<std::array<int, 3>, gr::Edit> old;
    gr::edits_parse(old);
    for (int i = 0; i < na; i++) {
        int s = all[2 * i], c = all[2 * i + 1], m = 0;
        for (int k = 0; k < gr::K_N; k++) {
            gr::Edit a = gr::edit_in(old, s, c, k), b = gr::edit_of(s, c, k);
            if (a.z != b.z || a.dx != b.dx || a.dy != b.dy) m |= 1 << k;
        }
        if (m) { sc.push_back(s); sc.push_back(c); mk.push_back(m); }
    }
    int n = (int)mk.size();
    ed_total = n; ed_done = 0;
    psp_render_edits_save();
    std::thread([q, sc, mk, n, na] {
        auto t0 = std::chrono::steady_clock::now();
        int files = 0;
        for (int i = 0; i < n; i++) {
            gr::Src e;
            int s = sc[2 * i], c = sc[2 * i + 1];
            { std::lock_guard<std::mutex> g(gr::mx); auto it = gr::pts.find(gr::key(q, s, c)); if (it == gr::pts.end()) { ed_done++; continue; } e = it->second; }
            gr::Img m;
            if (gr::load(gr::cap_file(q, s, c), m)) files += gr::derive(q, s, c, m, e.p, mk[i]);
            ed_done++;
        }
        if (n) gr::epoch++;
        printf("render: editor aplicado, %d de %d renders con cambios, %d archivos en %.1f s\n", n, na, files, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()); fflush(stdout);
    }).detach();
}

// transformation wheel portraits (core/hooks/tr_wheel.inc): 1_battle_ui/299_tr_wheel.rhg/02.png, 12 cells of 56x56 on a 64 grid (4x3),
// each the mid render of the target cut in a circle (generated mid render first, else the select's mid atlas cell)
extern volatile int tw_cell[12];
extern volatile uint32_t tw_cell_epoch;
static bool tw_is(uint32_t id) {
    static std::mutex m;
    static std::map<uint32_t, bool> seen;
    std::lock_guard<std::mutex> g(m);
    auto it = seen.find(id);
    if (it != seen.end()) return it->second;
    std::string f = lower(tex_reg[id - 1].fpath);
    for (auto& ch : f) if (ch == '\\') ch = '/';
    bool r = f.find("299_tr_wheel.rhg/") != std::string::npos && f.size() >= 6 && f.compare(f.size() - 6, 6, "02.png") == 0;
    seen[id] = r;
    if (r) printf("rueda: textura de retratos %s\n", tex_reg[id - 1].fpath.c_str());
    return r;
}
static uint32_t tw_variant(uint32_t id) { return tw_is(id) ? (tw_cell_epoch * 0x85EBCA6Bu) ^ (gr::epoch.load() * 0xC2B2AE35u) : 0; }
static std::shared_ptr<gr::Img> tw_mid_orig(int s) {
    static std::vector<uint8_t> d;
    static std::vector<ags::State> st;
    static std::map<int, uint32_t> page_tex;
    static bool ready = false;
    if (!ready) {
        ready = true;
        std::string f = disc_dir + "/cache/roster/en_015_char_renders_mid.ags";
        if (!is_file(f)) f = link_real(disc_dir + "/Shared/" + psp_shared_rel("Menus/char_select/char_select_en.pak") + "/1_interface/015_char_renders_mid.ags");
        if (!read_file(f, d, true) || !ags::parse(d, st)) { printf("rueda: no se pudo leer %s\n", f.c_str()); st.clear(); }
        for (uint32_t id = 1; id <= tex_reg.size(); id++) {
            std::string k = lower(tex_reg[id - 1].pak) + "|" + lower(link_real(tex_reg[id - 1].fpath));
            if (k.find("char_renders_mid") == std::string::npos || k.find(".rhg") == std::string::npos) continue;
            int pg = atoi(tex_reg[id - 1].name.c_str());
            if (!page_tex.count(pg)) page_tex[pg] = id;
        }
        printf("rueda: renders mid originales, %d estados, %d paginas\n", (int)st.size(), (int)page_tex.size());
    }
    int n = -1;
    for (int k = 0; k < roster::K_OLD; k++) if (roster::VANILLA[k] == s) n = k;
    if (n < 0) n = roster::menu_of(s);
    if (n < 0 || n >= (int)st.size()) return nullptr;
    int tx, tw, th; float r[4];
    if (!ags::rect(d, st[n], &tx, &tw, &th, r) || tw <= 0 || th <= 0) return nullptr;
    auto it = page_tex.find(tx + 1);
    if (it == page_tex.end()) return nullptr;
    TexPng* b = tex_load(it->second);
    if (!b || !b->w) return nullptr;
    gr::Img o; o.w = b->w; o.h = b->h; o.px = b->px;
    float sx = (float)b->w / tw, sy = (float)b->h / th;
    auto m = std::make_shared<gr::Img>(gr::resample(o, r[0] * sx, r[1] * sy, r[2] * sx, r[3] * sy, (int)((r[2] - r[0]) * sx), (int)((r[3] - r[1]) * sy)));
    if (b->psp) for (auto& p : m->px) { uint32_t a = (p >> 24) * 2; p = (p & 0x00FFFFFFu) | ((a > 255 ? 255 : a) << 24); }
    return m;
}
// item 81 (BT3 style select): the small render cell of a menu number, the generated one first (Graphics > Renders), else the atlas cell
bool psp_sel_small(int menu, std::vector<uint32_t>& px, int* w, int* h) {
    static std::mutex m;
    static std::vector<uint8_t> d;
    static std::vector<ags::State> st;
    static std::map<int, uint32_t> page_tex;
    static bool ready = false;
    {
        std::lock_guard<std::mutex> g(m);
        if (!ready) {
            ready = true;
            std::string f = disc_dir + "/cache/roster/en_017_char_renders_small.ags";
            if (!is_file(f)) f = link_real(disc_dir + "/Shared/" + psp_shared_rel("Menus/char_select/char_select_en.pak") + "/1_interface/017_char_renders_small.ags");
            if (!read_file(f, d, true) || !ags::parse(d, st)) { printf("sel81: no se pudo leer %s\n", f.c_str()); st.clear(); }
            for (uint32_t id = 1; id <= tex_reg.size(); id++) {
                std::string k = lower(tex_reg[id - 1].pak) + "|" + lower(link_real(tex_reg[id - 1].fpath));
                if (k.find("char_renders_small") == std::string::npos || k.find(".rhg") == std::string::npos) continue;
                int pg = atoi(tex_reg[id - 1].name.c_str());
                if (!page_tex.count(pg)) page_tex[pg] = id;
            }
            printf("sel81: renders pequenos, %d estados, %d paginas\n", (int)st.size(), (int)page_tex.size());
        }
    }
    int s = gr::sys_of_menu(menu), q = gr::on_q();
    if (s >= 0 && q > 0 && gr::have(s, 0, q))
        if (auto g = gr::file_img(q, s, 0, gr::K_SMALL, false)) { px = g->px; *w = g->w; *h = g->h; return true; }
    if (menu < 0 || menu >= (int)st.size()) return false;
    int tx, tw, th; float r[4];
    if (!ags::rect(d, st[menu], &tx, &tw, &th, r) || tw <= 0 || th <= 0) return false;
    auto it = page_tex.find(tx + 1);
    if (it == page_tex.end()) return false;
    TexPng* b = tex_load(it->second);
    if (!b || !b->w) return false;
    gr::Img o; o.w = b->w; o.h = b->h; o.px = b->px;
    float sx = (float)b->w / tw, sy = (float)b->h / th;
    gr::Img c = gr::resample(o, r[0] * sx, r[1] * sy, r[2] * sx, r[3] * sy, std::max(1, (int)((r[2] - r[0]) * sx)), std::max(1, (int)((r[3] - r[1]) * sy)));
    if (b->psp) for (auto& p : c.px) { uint32_t a = (p >> 24) * 2; p = (p & 0x00FFFFFFu) | ((a > 255 ? 255 : a) << 24); }
    px = c.px; *w = c.w; *h = c.h;
    return true;
}
static std::shared_ptr<gr::Img> tw_compose(uint32_t id, int* nserved) {
    const int S = 4, W = 256 * S, H = 192 * S, D = 56 * S;
    auto out = std::make_shared<gr::Img>();
    out->w = W; out->h = H; out->px.assign((size_t)W * H, 0);
    int q = gr::on_q(), served = 0;
    for (int i = 0; i < 12; i++) {
        int s = tw_cell[i];
        if (s < 0) continue;
        std::shared_ptr<gr::Img> src;
        for (int qq : {q, 4, 2, 1}) if (qq && gr::have(s, 0, qq) && (src = gr::raw_img(qq, s, 0, gr::K_MID))) break;
        if (!src) src = tw_mid_orig(s);
        if (!src || !src->w || !src->h) continue;
        int side = std::min(src->w, src->h);
        float fx = (src->w - side) * 0.5f, fy = (src->h - side) * 0.35f;
        gr::Img cell = gr::resample(*src, fx, fy, fx + side, fy + side, D, D);
        int ox = (4 + 64 * (i % 4)) * S, oy = (4 + 64 * (i / 4)) * S;
        float rc = D * 0.5f, rr = 26.5f * S;
        for (int y = 0; y < D; y++) for (int x = 0; x < D; x++) {
            float dx = x + 0.5f - rc, dy = y + 0.5f - rc, e = rr - sqrtf(dx * dx + dy * dy);
            if (e <= 0) continue;
            uint32_t p = cell.px[(size_t)y * D + x];
            if (e < 1.0f) p = (p & 0x00FFFFFFu) | ((uint32_t)((p >> 24) * e) << 24);
            out->px[(size_t)(oy + y) * W + ox + x] = p;
        }
        served++;
    }
    if (tex_reg[id - 1].psp) gr::psp_alpha(*out);
    *nserved = served;
    return out;
}
// the wheel atlas is composed in the background as soon as the cells are known (the wheel hook calls tw_prepare every frame)
static std::shared_ptr<gr::Img> tw_img; static uint32_t tw_made = 0, tw_job = 0; static std::mutex tw_m; static std::condition_variable tw_cv;
static uint32_t tw_find_id() {
    static int cached = -2;
    if (cached == -2) { cached = -1; for (uint32_t id = 1; id <= tex_reg.size(); id++) if (tw_is(id)) { cached = (int)id; break; } }
    return cached > 0 ? (uint32_t)cached : 0;
}
static void tw_store(uint32_t id, uint32_t key, std::shared_ptr<gr::Img> out, int served, bool bg) {
    std::lock_guard<std::mutex> g(tw_m);
    if (key == tw_variant(id)) { tw_img = out; tw_made = key; }
    if (tw_job == key) tw_job = 0;
    tw_cv.notify_all();
    static int logs = 0;
    if (logs++ < 20) { printf("rueda: retratos compuestos, %d de 12%s\n", served, bg ? " (en segundo plano)" : ""); fflush(stdout); }
}
void tw_prepare() {
    uint32_t id = tw_find_id();
    if (!id || tw_cell_epoch == 0) return;
    uint32_t key = tw_variant(id);
    {
        std::lock_guard<std::mutex> g(tw_m);
        if ((tw_img && tw_made == key) || tw_job == key) return;
        tw_job = key;
    }
    std::thread([id, key] { int n = 0; auto out = tw_compose(id, &n); tw_store(id, key, out, n, true); }).detach();
}
static bool tw_serve(uint32_t id, int* w, int* h, const uint32_t** px) {
    if (!tw_is(id)) return false;
    uint32_t key = tw_variant(id);
    std::unique_lock<std::mutex> lk(tw_m);
    if (tw_job == key) tw_cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return tw_job != key; });
    if (!tw_img || tw_made != key) {
        lk.unlock();
        PERF_SUB("retratos de la rueda");
        int n = 0; auto out = tw_compose(id, &n);
        tw_store(id, key, out, n, false);
        lk.lock();
        if (!tw_img || tw_made != key) { tw_img = out; tw_made = key; }
    }
    *w = tw_img->w; *h = tw_img->h; *px = tw_img->px.data();
    return true;
}
#endif

// map select 3D, Random: its big render (map_renders_big.rhg/11) becomes the app's "?" (games/ttt/map3d/random_mark.png)
// on a clear 300x152 canvas while the 3D of Random's map is behind it
extern volatile int m3d_q_on;
static const int RQ_D = 68, RQ_CX = 150, RQ_CY = 80;   // mark size and centre in the 300x152 render
static bool rq_is(uint32_t id) {
    static std::mutex m;
    static std::map<uint32_t, bool> seen;
    std::lock_guard<std::mutex> g(m);
    auto it = seen.find(id);
    if (it != seen.end()) return it->second;
    std::string f = lower(tex_reg[id - 1].fpath);
    for (auto& ch : f) if (ch == '\\') ch = '/';
    size_t k = f.find("map_renders_big.rhg/");
    bool r = k != std::string::npos && f.compare(k + 20, 2, "11") == 0 && (f[k + 22] == '.' || f[k + 22] == '@');
    seen[id] = r;
    if (r) printf("map3d: textura del render de Random %s\n", tex_reg[id - 1].fpath.c_str());
    return r;
}
static uint32_t rq_variant(uint32_t id) { return m3d_q_on && rq_is(id) ? 0x5A17E3C1u : 0; }
static bool rq_serve(uint32_t id, int* w, int* h, const uint32_t** px) {
    if (!m3d_q_on || !rq_is(id)) return false;
    static std::shared_ptr<gr::Img> img;
    static int st = 0;
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    if (!st) {
        st = -1;
        std::vector<uint8_t> f; std::vector<uint32_t> ip; int iw = 0, ih = 0; std::string err;
        FILE* fp = fopen("../../games/ttt/map3d/random_mark.png", "rb");
        if (fp) { fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET); f.resize(n > 0 ? (size_t)n : 0); if (fread(f.data(), 1, f.size(), fp) != f.size()) f.clear(); fclose(fp); }
        if (!f.empty() && png_decode(f, &iw, &ih, ip, &err) && iw > 0 && ih > 0) {
            const int S = 4, W = 300 * S, H = 152 * S, D = RQ_D * S;
            gr::Img src; src.w = iw; src.h = ih; src.px = ip;
            gr::Img q = gr::resample(src, 0, 0, (float)iw, (float)ih, D, D);
            auto out = std::make_shared<gr::Img>();
            out->w = W; out->h = H; out->px.assign((size_t)W * H, 0);
            int ox = RQ_CX * S - D / 2, oy = RQ_CY * S - D / 2;
            for (int y = 0; y < D; y++) for (int x = 0; x < D; x++) {
                int X = ox + x, Y = oy + y;
                if (X >= 0 && Y >= 0 && X < W && Y < H) out->px[(size_t)Y * W + X] = q.px[(size_t)y * D + x];
            }
            if (tex_reg[id - 1].psp) gr::psp_alpha(*out);
            img = out; st = 1;
        }
        printf("map3d: signo de Random %s (games/ttt/map3d/random_mark.png %dx%d)\n", st > 0 ? "compuesto" : "no encontrado", iw, ih); fflush(stdout);
    }
    if (st < 0) return false;
    *w = img->w; *h = img->h; *px = img->px.data();
    return true;
}

// map renders from the 3D (map3d.inc): one capture per entry camera of a map at its first frame, cache/renders/maps/q<q>/cap/;
// the chosen one (chosen.txt, default the first) gives <map>_big.png (300x152) and <map>_small.png (64x44, a cut of the big)
namespace gm {
static std::atomic<uint32_t> epoch{1};
static std::mutex fm;
static std::atomic<int> done{0}, total{0};
static std::string dir(int q) { return disc_dir + "/cache/renders/maps/q" + std::to_string(q); }
static std::string cap_file(int q, const std::string& k, int i) { return dir(q) + "/cap/" + k + "_" + std::to_string(i) + ".png"; }
static std::string file(int q, const std::string& k, const char* kind) { return dir(q) + "/" + k + "_" + kind + ".png"; }
static int count() { return stages::OLD + (int)stages::defs().maps.size(); }
static std::string key_of(int i) {
    if (i < stages::OLD) return i >= 0 ? "o" + std::to_string(i) : "";
    auto& d = stages::defs();
    size_t j = (size_t)(i - stages::OLD);
    if (j >= d.maps.size()) return "";
    std::string k = d.maps[j].mod + "_" + d.maps[j].id;
    for (auto& ch : k) if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') ch = '_';
    return k;
}
static std::string name_of(int i) { return i < stages::OLD ? stages::NAMES[i] : stages::defs().maps[(size_t)(i - stages::OLD)].id; }
static std::map<std::string, int> chosen_read(int q) {
    std::map<std::string, int> m;
    FILE* f = fopen((dir(q) + "/chosen.txt").c_str(), "r");
    if (!f) return m;
    char k[256]; int c;
    while (fscanf(f, "%255s %d", k, &c) == 2) m[k] = c;
    fclose(f);
    return m;
}
static void chosen_set(int q, const std::string& k, int c) {
    std::lock_guard<std::mutex> g(fm);
    auto m = chosen_read(q);
    if (c < 0) m.erase(k); else m[k] = c;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir(q)), ec);
    FILE* f = fopen((dir(q) + "/chosen.txt").c_str(), "w");
    if (!f) return;
    for (auto& e : m) fprintf(f, "%s %d\n", e.first.c_str(), e.second);
    fclose(f);
}
static int chosen(int q, const std::string& k) {   // the chosen capture, else the first one there is
    { std::lock_guard<std::mutex> g(fm); auto m = chosen_read(q); auto it = m.find(k); if (it != m.end() && is_file(cap_file(q, k, it->second))) return it->second; }
    for (int i = 0; i < 3; i++) if (is_file(cap_file(q, k, i))) return i;
    return -1;
}
static void small_box(int w, int h, float* b) {   // 64:44 of the whole height, centred
    float H = (float)h, W = H * 64.0f / 44.0f;
    if (W > w) { W = (float)w; H = W * 44.0f / 64.0f; }
    b[0] = (w - W) * 0.5f; b[1] = (h - H) * 0.5f; b[2] = b[0] + W; b[3] = b[1] + H;
}
static int derive(int q, const std::string& k, const gr::Img& cap) {
    gr::Img big = gr::resample(cap, 0, 0, (float)cap.w, (float)cap.h, 300 * q, 152 * q);
    float b[4]; small_box(cap.w, cap.h, b);
    gr::Img sm = gr::resample(cap, b[0], b[1], b[2], b[3], 64 * q, 44 * q);
    int n = gr::save(file(q, k, "big"), big) + gr::save(file(q, k, "small"), sm);
    epoch++;
    return n;
}
}
std::string psp_map_render_key(int i) { return gm::key_of(i); }
bool psp_map_render_have(const std::string& k, int q) {
    if (is_file(gm::file(q, k, "big"))) return true;
    for (int i = 0; i < 3; i++) if (is_file(gm::cap_file(q, k, i))) return true;
    return false;
}
bool psp_map_render_fix(const std::string& k, int q) {   // captures without big and small (made before they existed): made from the chosen one
    if (is_file(gm::file(q, k, "big")) && is_file(gm::file(q, k, "small"))) return false;
    int c = gm::chosen(q, k);
    if (c < 0) return false;
    gr::busy++;
    std::thread([k, q, c] {
        gr::Img m;
        int n = gr::load(gm::cap_file(q, k, c), m) && m.w ? gm::derive(q, k, m) : 0;
        printf("render: mapa %s sin big ni small: hechos de la camara %d (%d archivos)\n", k.c_str(), c, n); fflush(stdout);
        gr::busy--;
    }).detach();
    return true;
}
void psp_map_render_begin(const std::string& k, int q) {
    std::error_code ec;
    for (int i = 0; i < 3; i++) std::filesystem::remove(std::filesystem::u8path(gm::cap_file(q, k, i)), ec);
    gm::chosen_set(q, k, -1);
}
void psp_map_render_save(const std::string& k, int i, int q, std::vector<uint32_t>&& px, int w, int h) {
    gr::busy++;
    auto img = std::make_shared<gr::Img>();
    img->w = w; img->h = h; img->px = std::move(px);
    std::thread([k, i, q, img] {
        auto t0 = std::chrono::steady_clock::now();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(gm::dir(q) + "/cap"), ec);
        bool ok = gr::save(gm::cap_file(q, k, i), *img);
        int n = i == 0 ? gm::derive(q, k, *img) : 0;   // the first camera serves until one is chosen
        static std::atomic<int> logs{0};
        if (logs++ < 60) { printf("render: mapa %s camara %d %s (%dx%d)%s en %.0f ms\n", k.c_str(), i, ok ? "guardada" : "NO guardada", img->w, img->h, n ? ", big y small hechos" : "", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); fflush(stdout); }
        gr::busy--;
    }).detach();
}
// map chooser (ui/map_chooser.h)
static std::atomic<int> mc_req{0};
void psp_map_chooser_request() { mc_req = 1; }
bool psp_map_chooser_take() { if (!mc_req.load() || gr::busy.load() > 0) return false; mc_req = 0; return true; }
int psp_map_render_list(int q, std::vector<std::string>& keys, std::vector<std::string>& names, std::vector<int>& mask, std::vector<int>& pick) {
    keys.clear(); names.clear(); mask.clear(); pick.clear();
    for (int i = 0; i < gm::count(); i++) {
        std::string k = gm::key_of(i);
        int m = 0;
        for (int c = 0; c < 3; c++) if (is_file(gm::cap_file(q, k, c))) m |= 1 << c;
        if (!m) continue;
        keys.push_back(k); names.push_back(gm::name_of(i)); mask.push_back(m); pick.push_back(gm::chosen(q, k));
    }
    return (int)keys.size();
}
bool psp_map_render_preview(int q, const std::string& k, int i, int w, int h, std::vector<uint32_t>& px) {
    gr::Img m;
    if (!gr::load(gm::cap_file(q, k, i), m) || !m.w) return false;
    gr::Img r = gr::resample(m, 0, 0, (float)m.w, (float)m.h, w, h);
    px.swap(r.px);
    return true;
}
int psp_map_render_choose_progress(int* total) { *total = gm::total.load(); return gm::done.load(); }
void psp_map_render_choose(int q, std::vector<std::pair<std::string, int>> picks) {   // own thread: the others deleted, big and small from the chosen
    gm::total = (int)picks.size(); gm::done = 0;
    gr::busy++;
    std::thread([q, picks] {
        auto t0 = std::chrono::steady_clock::now();
        int files = 0;
        int made = 0;
        for (auto& p : picks) {
            int src;
            { std::lock_guard<std::mutex> g(gm::fm); auto cm = gm::chosen_read(q); auto it = cm.find(p.first); src = it != cm.end() ? it->second : 0; }
            bool same = src == p.second && is_file(gm::file(q, p.first, "big")) && is_file(gm::file(q, p.first, "small"));   // big and small already made from this camera
            gr::Img m;
            if (same || (gr::load(gm::cap_file(q, p.first, p.second), m) && m.w)) {
                if (!same) { files += gm::derive(q, p.first, m); made++; }
                std::error_code ec;
                for (int c = 0; c < 3; c++) if (c != p.second) std::filesystem::remove(std::filesystem::u8path(gm::cap_file(q, p.first, c)), ec);
                gm::chosen_set(q, p.first, p.second);
            }
            gm::done++;
        }
        printf("render: eleccion de mapas aplicada, %d de %d mapas con cambios, %d archivos en %.1f s\n", made, (int)picks.size(), files, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()); fflush(stdout);
        gr::busy--;
    }).detach();
}

// serving: map_renders_big.rhg texture t = map t (Random 10 keeps its own, new maps from 11); map_selector_bg.rhg texture 10 = the
// originals' thumbnail atlas (cells from map_selector_bg.ags states 6..15), new maps' thumbnails from st_small_base
namespace gm {
struct Mq { int kind = -1, map = -1; };   // kind 0 big, 1 thumbnail atlas, 2 one thumbnail
static int on_q() {
    static int v = 0, n = 0;
    if ((n++ & 63) == 0) { const char* e = getenv("TTT_GEN_MAP_RENDERS"); v = e && *e && atoi(e) > 0 ? psp_render_quality() : 0; }
    return v;
}
static std::atomic<uint64_t> cl_us{0}, cl_n{0};
static Mq classify(uint32_t id) {
    TexPng& t = tex_reg[id - 1];
    { int k = __atomic_load_n(&t.mqk, __ATOMIC_ACQUIRE); if (k != -2) return {k, t.mqm}; }
    auto t0 = std::chrono::steady_clock::now();
    Mq o;
    for (const std::string& x0 : {t.fpath, link_real(t.fpath)}) {
        std::string f = lower(x0);
        for (auto& ch : f) if (ch == '\\') ch = '/';
        for (int b = 0; b < 2 && o.kind < 0; b++) {
            const char* nm = b ? "map_selector_bg.rhg/" : "map_renders_big.rhg/";
            size_t p = f.find(nm);
            if (p == std::string::npos || !isdigit((unsigned char)f[p + strlen(nm)])) continue;
            int tx = atoi(f.c_str() + p + strlen(nm)) - 1;
            if (!b) { if (tx >= 0 && tx < 10) o = {0, tx}; else if (tx >= 11) o = {0, stages::OLD + tx - 11}; }
            else { int sb = st_small_base >= 0 ? st_small_base : 11; if (tx == 10) o = {1, -1}; else if (tx >= sb) o = {2, stages::OLD + tx - sb}; }
        }
        auto& d = stages::defs();
        for (size_t j = 0; j < d.maps.size() && o.kind < 0; j++) {   // a new map's own PNG (mods/<mod>/Stages/<id>/big.png | small.png)
            std::string st = "/stages/" + lower(d.maps[j].id) + "/";
            size_t p = f.find(st);
            if (p == std::string::npos) continue;
            if (f.compare(p + st.size(), 3, "big") == 0) o = {0, stages::OLD + (int)j};
            else if (f.compare(p + st.size(), 5, "small") == 0) o = {2, stages::OLD + (int)j};
        }
        if (o.kind >= 0) break;
    }
    if (o.kind >= 0 && o.map >= count()) o.kind = -1;
    t.mqm = o.map;
    __atomic_store_n(&t.mqk, o.kind, __ATOMIC_RELEASE);
    cl_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (++cl_n % 2000 == 0) { printf("render: mapas, %llu texturas clasificadas en %llu ms\n", (unsigned long long)cl_n.load(), (unsigned long long)(cl_us.load() / 1000)); fflush(stdout); }
    static int logs = 0;
    if (o.kind >= 0 && logs++ < 30) printf("render: textura de mapa %s -> tipo %d, mapa %d\n", t.fpath.c_str(), o.kind, o.map);
    return o;
}
static std::shared_ptr<gr::Img> img(int q, const std::string& k, const char* kind, bool psp, int w, int h) {
    std::string ck = "map|" + std::to_string(epoch.load()) + "|" + k + "|" + kind + "|" + std::to_string(psp) + "|" + std::to_string(w) + "x" + std::to_string(h);
    if (auto m = gr::cache_get(ck)) return m;
    gr::Img f;
    if (!gr::load(file(q, k, kind), f) || !f.w) return nullptr;
    auto m = std::make_shared<gr::Img>(f.w == w && f.h == h ? f : gr::resample(f, 0, 0, (float)f.w, (float)f.h, w, h));
    m->uid = gr::img_uid++;
    if (psp) gr::psp_alpha(*m);
    gr::cache_put(ck, m);
    return m;
}
static std::shared_ptr<gr::Img> atlas(uint32_t id, int q) {
    const TexPng& t = tex_reg[id - 1];
    int W = std::max(1, t.width0) * q, H = std::max(1, t.height0) * q;
    std::string ck = "map|" + std::to_string(epoch.load()) + "|atlas|" + std::to_string(id) + "|" + std::to_string(q);
    if (auto m = gr::cache_get(ck)) return m;
    static std::vector<uint8_t> d;
    static std::vector<ags::State> st;
    {
        static std::mutex im;
        std::lock_guard<std::mutex> g(im);
        if (d.empty()) {
            std::string f = link_real(disc_dir + "/Shared/" + psp_shared_rel("Menus/map_select") + "/map_selector_bg.ags");
            if (!read_file(f, d, true) || !ags::parse(d, st)) { d.clear(); printf("render: no se pudo leer %s\n", f.c_str()); return nullptr; }
        }
    }
    TexPng* b = tex_load(id);
    if (!b) return nullptr;
    gr::Img orig; orig.w = b->w; orig.h = b->h; orig.px = b->px;
    auto m = std::make_shared<gr::Img>(gr::resample(orig, 0, 0, (float)orig.w, (float)orig.h, W, H));
    m->uid = gr::img_uid++;
    int cells = 0;
    for (int i = 0; i < stages::OLD && 6 + i < (int)st.size(); i++) {
        int tx, tw, th; float r[4];
        if (!ags::rect(d, st[6 + i], &tx, &tw, &th, r) || tx != 10 || tw <= 0 || th <= 0) continue;
        float sx = (float)W / tw, sy = (float)H / th;
        int x0 = (int)(r[0] * sx + 0.5f), y0 = (int)(r[1] * sy + 0.5f), x1 = (int)(r[2] * sx + 0.5f), y1 = (int)(r[3] * sy + 0.5f);
        if (x1 <= x0 || y1 <= y0) continue;
        auto g = img(q, key_of(i), "small", t.psp, x1 - x0, y1 - y0);
        if (!g) continue;
        for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++) if (x0 + x < W && y0 + y < H) m->px[(size_t)(y0 + y) * W + x0 + x] = g->px[(size_t)y * g->w + x];
        cells++;
    }
    static int logs = 0;
    if (logs++ < 6) printf("render: atlas de miniaturas de mapas con %d celdas generadas (%dx%d)\n", cells, W, H);
    gr::cache_put(ck, m);
    return m;
}
}
static uint32_t mq_variant(uint32_t id) {
    int q = gm::on_q();
    if (!q || gm::classify(id).kind < 0) return 0;
    return 0x6D000000u ^ gm::epoch.load() * 0x9E3779B1u ^ (uint32_t)q;
}
static bool mq_serve(uint32_t id, int* w, int* h, const uint32_t** px) {
    static thread_local std::shared_ptr<gr::Img> hold;
    int q = gm::on_q();
    if (!q) return false;
    gm::Mq o = gm::classify(id);
    if (o.kind < 0) return false;
    const TexPng& t = tex_reg[id - 1];
    int gw = std::max(1, t.width0) * q, gh = std::max(1, t.height0) * q;
    auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<gr::Img> m = o.kind == 1 ? gm::atlas(id, q) : gm::img(q, gm::key_of(o.map), o.kind ? "small" : "big", t.psp, gw, gh);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> sl{0};
    if (ms > 4.0 && sl++ < 12) { printf("render: textura de mapa %s preparada en %.0f ms\n", t.fpath.c_str(), ms); fflush(stdout); }
    if (!m || !m->w) return false;
    hold = m;
    *w = m->w; *h = m->h; *px = m->px.data();
    static std::atomic<int> logs{0};
    if (logs++ < 16) { printf("render: servido %s como render de mapa generado (tipo %d, mapa %d, %dx%d)\n", t.fpath.c_str(), o.kind, o.map, m->w, m->h); fflush(stdout); }
    return true;
}

// item 81: the generated big render of a map (Graphics > Renders of maps), empty when there is none
bool psp_sel_map_big(int map, std::vector<uint32_t>& px, int* w, int* h) {
    int q = gm::on_q();
    std::string k = q > 0 ? gm::key_of(map) : std::string();
    gr::Img m;
    if (k.empty() || !gr::load(gm::file(q, k, "big"), m) || !m.w) return false;
    px = m.px; *w = m.w; *h = m.h;
    return true;
}
