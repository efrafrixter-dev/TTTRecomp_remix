// item 33: the character select's big render becomes the 3D model of that character on the battle map (TTT_SELECT_3D).
// The select page (class 0x089F708C, the object that loads the 0xA7 big renders, holders at +0x1A8 + 4n) is found when it
// loads them (pair loader, ra 0x089F8D08, fp = the page); the render drawn (0x089D1B94 on the instance of holder n) says
// which character is shown. The Character Reference viewer's battle modules run it (owner 2); the frame end draws the
// stage and the model inside the render panel (GPU panel markers 0xE7..0xE9) and then the name and form label again.
void psp_disc_hold(uint32_t idx, bool front, bool fast = true);
void psp_disc_hold_battle();
void psp_sel_images_warm();
void psp_render_editor_request();
static uint32_t s3d_obj = 0, s3d_vt = 0, s3d_fall = 0, s3d_idle = 0;
static int s3d_n = -1, s3d_rn = -1, s3d_seen = 0, s3d_frame = 0, s3d_phase = 0, s3d_last_obj = -1, s3d_map = -1, s3d_last_n = -1;
static bool s3d_files = false, s3d_hit = false;
static uint64_t s3d_req = 0;
static int S3D_X0 = 0, S3D_X1 = 274, S3D_Y0 = 50, S3D_Y1 = 176;   // render panel of the select screen (480x272), 2 px steps
bool psp_sel_layout3d();
static bool s3d_full = false;   // the select layout for the 3D view (games/ttt/select3d/layout): the whole left side, the body from head to feet
static float s3d_full_t0 = 0.285f, s3d_full_top = 0.02f, s3d_full_bot = 0.04f, s3d_full_head = 1.3f;   // TTT_SEL3D_CAM_FULL
// camera: placed once per character from its idle pose (waist to head), eased only from one character to the next
static float s3d_cam[4] = {0, 0, 0, 0}, s3d_want[4] = {0, 0, 0, 0};
static const float S3D_YAW = 0.3f;
static float s3d_yaw = S3D_YAW;   // right stick or Q / E turn the camera around the model (TTT_SEL3D_ROTATE=0 off)
float psp_input_rotate();
static bool s3d_cam_ok = false;
static uint64_t s3d_cam_t = 0;
// fall: speed, speed while the model is still above the panel, mode (0 always, 2 never), height above the landing that counts as in view
static float s3d_fspeed = 1.5f, s3d_base_sp = 1.0f;
static int s3d_fmode = 0;
struct S3dSwitch { int sys = -1, n = -1, f0 = 0, created = -1, landed = -1; uint64_t t0 = 0, ms = 0; bool hit = false; };
static S3dSwitch s3d_sw;

static bool s3d_enabled() {
    static int v = 0, n = 0;
    if ((n++ & 63) == 0) { const char* e = getenv("TTT_SELECT_3D"); v = e && *e ? atoi(e) : 0; }
    return v > 0;
}
// generated renders (item 33 part 2, Graphics > Renders): every character and colour drawn once without a stage into the GPU capture
void psp_cap_request(uint32_t tag, float rw, const float* tweak, const float* head, const float* waist, const float* mouth);
void psp_cap_diag(bool on);
uint32_t psp_render_tweaks_gen();
void psp_render_tweak(int s, float* v);
float psp_sel3d_head(int s);
static bool s3d_head_env = false;
bool psp_cap_take(uint32_t tag, std::vector<uint32_t>& px, int* w, int* h, float* pts);
void psp_render_save(int s, int c, int q, std::vector<uint32_t>&& px, int w, int h, const float* p);
bool psp_render_have(int s, int c, int q);
struct GenJob { int sys, col, dmg; };
static const int GEN_DMG = 64;   // render colour of a damaged level 2 costume = its colour + 64 (renders.h)
void psp_window_busy(int kind, int done, int total);
static std::vector<GenJob> gen_jobs;
static size_t gen_i = 0;
static bool gen_run = false;
static int gen_state = 0, gen_go_seen = 0, gen_tries = 0, gen_jf = 0, gen_q = 2;
static uint32_t gen_tag = 0, gen_page = 0;
static uint64_t gen_t0 = 0, gen_tj = 0;
static const int GEN_SETTLE = 8;
static bool cel_force_now = false, s3d_newobj = false;   // capture of a generated render: the fighter marker asks for cel (and so the absolute outline)
static int gen_env(const char* k, int d) { const char* e = getenv(k); return e && *e ? atoi(e) : d; }
static int s3d_sys_of_menu(int n) {
    if (n >= 0 && n < roster::K_OLD) return roster::VANILLA[n];
    if (n < roster::FIRST_NEW) return -1;
    for (int s = 0, r = 0; s < roster::ALL; s++) if (!roster::vanilla(s)) { if (r == n - roster::FIRST_NEW) return roster::find(s) || cr_game_char(s) >= 0 ? s : -1; r++; }
    return -1;
}
static bool s3d_alive() { return s3d_obj && psp_l32(s3d_obj + 0x40) == s3d_vt; }
bool psp_sel3d_open() { return s3d_alive(); }   // the GPU keeps the battle screen effects off while the select page lives

// colour paks kept in memory while the select is open: going back to a character shows it at once
struct S3dPak { uint32_t idx = 0, buf = 0; uint64_t used = 0; uint32_t drop = 0; bool dropped = false; };
static S3dPak s3d_cache[6];
static uint64_t s3d_use = 0;
// TTT_SEL3D_SETTLE: frames a pak waits after its object ends before another object (or a free) may take it
static uint32_t s3d_settle() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_SEL3D_SETTLE"); v = e && *e ? atoi(e) : 12; } return (uint32_t)v; }
static bool s3d_busy(const S3dPak& e) { return e.buf == cv.buf[0] || e.buf == cv.buf[1] || (e.dropped && cv_frame - e.drop < s3d_settle()); }
static bool s3d_keep(uint32_t buf) { for (auto& e : s3d_cache) if (e.buf == buf) return true; return false; }
static void s3d_dropmark(uint32_t buf) { for (auto& e : s3d_cache) if (e.buf == buf) { e.drop = cv_frame; e.dropped = true; } }
static uint32_t s3d_cached(uint32_t idx) {
    const S3dPak* busy = nullptr;
    for (auto& e : s3d_cache) if (e.buf && e.idx == idx) {
        if (s3d_settle() && s3d_busy(e)) { busy = &e; continue; }
        e.used = ++s3d_use; e.dropped = false; s3d_hit = true; return e.buf;
    }
    static int logs = 0;
    if (busy && logs++ < 20) printf("select3d: pak %u en cache aun en uso (cuadro %u, soltado en %u), se lee otra copia\n", idx, cv_frame, busy->drop);
    return 0;
}
static const Ctx* s3d_ctx = nullptr;
static void s3d_put(uint32_t idx, uint32_t buf) {
    S3dPak* v = nullptr;
    for (auto& e : s3d_cache) {
        if (!e.buf) { v = &e; break; }
        if (e.buf == cv.buf[0] || e.buf == cv.buf[1] || (s3d_settle() && s3d_busy(e))) continue;
        if (!v || e.used < v->used) v = &e;
    }
    if (!v) return;
    if (v->buf && s3d_ctx) { uint32_t old = v->buf; v->buf = 0; cv_free(*s3d_ctx, old); }
    *v = S3dPak(); v->idx = idx; v->buf = buf; v->used = ++s3d_use;
}
static void s3d_cache_free(const Ctx& c) {
    for (auto& e : s3d_cache) { if (e.buf && e.buf != cv.buf[0] && e.buf != cv.buf[1]) mpm_call(c, 0x089BFDB0u, e.buf); e = S3dPak(); }
}

// soft change from the last pose of the landing to the idle: the bone matrices blend for S3D_BLEND frames
static const int S3D_BLEND = 4;
static float s3d_pose[256][32];
static int s3d_bone[256], s3d_nb = 0, s3d_blend = 0;
static uint32_t s3d_snap_o = 0, s3d_snap_md = 0;   // the pose belongs to this object and model: never blended into another one
static uint32_t s3d_mtx(uint32_t o) {
    uint32_t mb = psp_l32(o + 0x16B0);
    return mb && mb < 0x10000000u ? psp_l32(mb + 4 * psp_l32(o + 0x16B4) + 0xC) : 0;
}
static float s3d_off = 0.0f;   // vertical offset given to the fall so the feet land on the floor (world Y down)
static int s3d_fall_f0 = -1, s3d_fall_len = 0;
static void s3d_snap(uint32_t o) {
    uint32_t hdr = psp_l32(o + 0x14), bt = hdr ? psp_l32(hdr + 0x50) : 0, nb = hdr ? psp_l32(hdr + 8) : 0, tb = s3d_mtx(o);
    if (bt && bt < 0x08000000u) bt += hdr;
    s3d_nb = 0; s3d_snap_o = o; s3d_snap_md = hdr;
    if (!bt || !tb || tb >= 0x10000000u) return;
    for (uint32_t i = 0; i < nb && i < 256; i++) {
        int id = psp_l16(bt + 0xA0 * i + 0xA) & 0xFF;
        if (id >= 0x77) continue;   // the table holds 0x3B80 bytes: 119 ids (EBOOT_TTT 120.1)
        s3d_bone[s3d_nb] = id;
        for (int k = 0; k < 32; k++) s3d_pose[s3d_nb][k] = cv_rf(tb + (uint32_t)id * 128 + 4 * (uint32_t)k);   // both matrices of the bone: the draw emits the two (vertices blend between them)
        s3d_nb++;
    }
}
// first frame of a model: its idle pose frames the camera (waist 0x03 to head 0x2F), the model is not drawn that frame
static float s3d_cam_t0 = 0.243f, s3d_cam_top = 0.10f, s3d_cam_bot = 0.06f, s3d_cam_min = 0.42f, s3d_cam_up = 0.06f, s3d_cam_side = 0.04f;
static bool s3d_probed = false, s3d_hide = false;
static int s3d_ptries = 0, s3d_land_f = -1;
static void s3d_mark_list(Ctx& c, uint32_t a, uint32_t b);
static void s3d_objmark(Ctx& c, bool before) { if (s3d_hide && cv_owner() == 2) s3d_mark_list(c, before ? 0x00EA0001u : 0x00EA0000u, 0); }
struct S3dCam { float v[4]; int feet[6] = {0}, nf = 0; float g = 0.0f; };   // camera, and the foot bones with their floor level in the idle pose
static std::map<int, S3dCam> s3d_known;   // camera measured on each character's idle: the next time it starts there
// bone position in the pose: bind position through the skinning matrix (row vectors). TTT models keep their vertices in
// normalized units, so the matrix carries a uniform scale (about the model height; 1 in converted BT3 models): the
// rotation part is divided by that scale, the translation is in world units
static bool s3d_bone_pos(uint32_t o, uint32_t tb, int id, float* out, float* scale = nullptr) {
    uint32_t hdr = psp_l32(o + 0x14), bt = hdr ? psp_l32(hdr + 0x50) : 0, nb = hdr ? psp_l32(hdr + 8) : 0;
    if (bt && bt < 0x08000000u) bt += hdr;
    for (uint32_t i = 0; bt && i < nb && i < 256; i++) {
        if ((psp_l16(bt + 0xA0 * i + 0xA) & 0xFF) != (uint32_t)id) continue;
        float p[3] = {cv_rf(bt + 0xA0 * i + 0x10), cv_rf(bt + 0xA0 * i + 0x14), cv_rf(bt + 0xA0 * i + 0x18)}, m[16];
        uint32_t mm = tb + (uint32_t)id * 128;
        for (int k = 0; k < 16; k++) m[k] = cv_rf(mm + 4 * (uint32_t)k);
        float sc[3];   // the scale of each input axis (the vertices are normalized by the model box): its row's norm
        for (int r = 0; r < 3; r++) { sc[r] = sqrtf(m[4 * r] * m[4 * r] + m[4 * r + 1] * m[4 * r + 1] + m[4 * r + 2] * m[4 * r + 2]); if (!std::isfinite(sc[r]) || sc[r] < 1e-3f) return false; }
        for (int col = 0; col < 3; col++) out[col] = p[0] * m[col] / sc[0] + p[1] * m[4 + col] / sc[1] + p[2] * m[8 + col] / sc[2] + m[12 + col];
        if (scale) { scale[0] = sc[0]; scale[1] = sc[1]; scale[2] = sc[2]; }
        static int dumps = 0;
        if (dumps < 4 && id == 0x2F) {
            dumps++;
            printf("select3d: matriz cabeza de %d: base (%.2f, %.2f, %.2f) |", cv.sys, p[0], p[1], p[2]);
            for (int k = 0; k < 16; k++) printf(" %.3f", m[k]);
            printf("\n"); fflush(stdout);
        }
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
    return false;
}
// a bone's bind position carried by another bone's posed matrix (a face bone rides on the head: its own matrix may be unused)
static bool s3d_bone_pos_as(uint32_t o, uint32_t tb, int id, int mtx_id, float* out) {
    uint32_t hdr = psp_l32(o + 0x14), bt = hdr ? psp_l32(hdr + 0x50) : 0, nb = hdr ? psp_l32(hdr + 8) : 0;
    if (bt && bt < 0x08000000u) bt += hdr;
    float p[3], ph[3]; bool fp = false, fh = false;
    for (uint32_t i = 0; bt && i < nb && i < 256; i++) {
        int bid = psp_l16(bt + 0xA0 * i + 0xA) & 0xFF;
        if (bid == id) { for (int k = 0; k < 3; k++) p[k] = cv_rf(bt + 0xA0 * i + 0x10 + 4 * (uint32_t)k); fp = true; }
        if (bid == mtx_id) { for (int k = 0; k < 3; k++) ph[k] = cv_rf(bt + 0xA0 * i + 0x10 + 4 * (uint32_t)k); fh = true; }
    }
    if (!fp || !fh) return false;
    float m[16], sc[3];
    uint32_t mm = tb + (uint32_t)mtx_id * 128;
    for (int k = 0; k < 16; k++) m[k] = cv_rf(mm + 4 * (uint32_t)k);
    for (int r = 0; r < 3; r++) { sc[r] = sqrtf(m[4 * r] * m[4 * r] + m[4 * r + 1] * m[4 * r + 1] + m[4 * r + 2] * m[4 * r + 2]); if (!std::isfinite(sc[r]) || sc[r] < 1e-3f) return false; }
    for (int col = 0; col < 3; col++) out[col] = p[0] * m[col] / sc[0] + p[1] * m[4 + col] / sc[1] + p[2] * m[8 + col] / sc[2] + m[12 + col];
    (void)ph;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
// the bones of the head area a model has (ids 0x2C..0x36) and their bind position against the head bone: the measurement of the face
static void s3d_head_bones(uint32_t o, int sys) {
    static std::set<int> said;
    if (!said.insert(sys).second || said.size() > 80) return;
    uint32_t hdr = psp_l32(o + 0x14), bt = hdr ? psp_l32(hdr + 0x50) : 0, nb = hdr ? psp_l32(hdr + 8) : 0;
    if (bt && bt < 0x08000000u) bt += hdr;
    float hy = 0, hx = 0, hz = 0; bool fh = false;
    for (uint32_t i = 0; bt && i < nb && i < 256; i++) if ((psp_l16(bt + 0xA0 * i + 0xA) & 0xFF) == 0x2F) { hx = cv_rf(bt + 0xA0 * i + 0x10); hy = cv_rf(bt + 0xA0 * i + 0x14); hz = cv_rf(bt + 0xA0 * i + 0x18); fh = true; }
    char b[400]; int n = snprintf(b, sizeof b, "render: huesos de cabeza del personaje %d (%u huesos):", sys, nb);
    for (uint32_t i = 0; bt && i < nb && i < 256 && n < 360; i++) {
        int id = psp_l16(bt + 0xA0 * i + 0xA) & 0xFF;
        if (id < 0x2C || id > 0x36) continue;
        n += snprintf(b + n, sizeof b - n, " %02X(%+.2f %+.2f %+.2f)", id, cv_rf(bt + 0xA0 * i + 0x10) - hx, cv_rf(bt + 0xA0 * i + 0x14) - hy, cv_rf(bt + 0xA0 * i + 0x18) - hz);
    }
    printf("%s%s\n", b, fh ? "" : " sin 2F"); fflush(stdout);
}
static bool s3d_bind_pos(uint32_t o, int id, float* out) {
    uint32_t hdr = psp_l32(o + 0x14), bt = hdr ? psp_l32(hdr + 0x50) : 0, nb = hdr ? psp_l32(hdr + 8) : 0;
    if (bt && bt < 0x08000000u) bt += hdr;
    for (uint32_t i = 0; bt && i < nb && i < 256; i++)
        if ((psp_l16(bt + 0xA0 * i + 0xA) & 0xFF) == (uint32_t)id) { for (int k = 0; k < 3; k++) out[k] = cv_rf(bt + 0xA0 * i + 0x10 + 4 * (uint32_t)k); return true; }
    return false;
}
// head bone near the top edge and waist bone near the bottom edge of the panel
static bool s3d_valid(const float* hd, const float* ws, float h) { return hd[1] < ws[1] - 0.1f * h && hd[1] > -1.6f * h && ws[1] < -0.05f * h && ws[1] > -1.2f * h; }
static void s3d_set_want(const float* hd, const float* ws, float h) {
    if (s3d_full) {   // highest bone (h) to the floor (y 0, up is -Y); heads and crests above the bones (Frieza): one head to waist span above the head bone
        float hr = s3d_full_head, th = s3d_head_env || gen_run ? 0.0f : psp_sel3d_head(cv.sys);
        if (th > 0.0f) hr = th;   // per character (tweaks.txt column 8), not in captures
        float span = ws[1] - hd[1], top = std::min(-h * (1.0f + s3d_full_top), span > 0.5f ? hd[1] - span * hr : 0.0f), bot = h * s3d_full_bot;
        static int logs = 0;
        if (span > 0.5f && logs++ < 60) { printf("select3d: personaje %d, cuerpo completo: hueso mas alto %.1f, cabeza %.1f, cintura %.1f, arriba %.1f\n", cv.sys, -h, hd[1], ws[1], top); fflush(stdout); }
        s3d_want[0] = (hd[0] + ws[0]) * 0.5f; s3d_want[1] = (top + bot) * 0.5f; s3d_want[2] = (hd[2] + ws[2]) * 0.5f;
        s3d_want[3] = (bot - top) * 0.5f / std::max(0.05f, s3d_full_t0);
        return;
    }
    // the head bone sits mid face: the top goes a part of the height above it; hunched models keep a minimum span
    float span = ws[1] - hd[1], top = hd[1] - s3d_cam_top * h, bot = ws[1] + s3d_cam_bot * span;
    if (bot - top < s3d_cam_min * h) bot = top + s3d_cam_min * h;
    float half = (bot - top) * 0.5f;
    s3d_want[0] = (hd[0] + ws[0]) * 0.5f; s3d_want[1] = (top + bot) * 0.5f - s3d_cam_up * h; s3d_want[2] = (hd[2] + ws[2]) * 0.5f;
    s3d_want[3] = std::max(0.2f * h, half / std::max(0.05f, s3d_cam_t0));
}
// the idle pose, read while the object is drawn clipped to one pixel (the game fills the matrices of what it draws): up to
// 10 frames with only the map visible, then the fall starts with the final camera. Kept per character for the next time
// foot bones: the lowest bones of the idle pose (not the root nor bones at the model's origin); their lowest point is the floor of the fall
static void s3d_feet_of(uint32_t o, uint32_t tb, float h, S3dCam& k) {
    uint32_t hdr = psp_l32(o + 0x14), bt = hdr ? psp_l32(hdr + 0x50) : 0, nb = hdr ? psp_l32(hdr + 8) : 0;
    if (bt && bt < 0x08000000u) bt += hdr;
    std::vector<std::pair<float, int>> lo;
    for (uint32_t i = 0; bt && i < nb && i < 256; i++) {
        int id = psp_l16(bt + 0xA0 * i + 0xA) & 0xFF;
        if (!id || id == 0x11 || id == 0x1F || id == 0x2D || (id >= 0x34 && id <= 0x36)) continue;
        float p[3];
        if (!s3d_bone_pos(o, tb, id, p)) continue;
        if (std::fabs(p[1]) < 0.03f * h && sqrtf(p[0] * p[0] + p[2] * p[2]) < 0.03f * h) continue;
        lo.push_back({p[1], id});
    }
    k.nf = 0;
    if (lo.empty()) return;
    std::sort(lo.begin(), lo.end(), [](const std::pair<float, int>& a, const std::pair<float, int>& b) { return a.first > b.first; });
    for (auto& x : lo) if (k.nf < 6 && x.first >= lo[0].first - 0.06f * h) k.feet[k.nf++] = x.second;
    k.g = lo[0].first;
    static int logs = 0;
    if (logs++ < 24) { printf("select3d: personaje %d, pies:", cv.sys); for (int i = 0; i < k.nf; i++) printf(" 0x%02X", k.feet[i]); printf(", suelo %.2f (altura %.1f)\n", k.g, h); fflush(stdout); }
}
static void s3d_probe(uint32_t o, uint32_t tb) {
    float h = cv.h > 1.0f ? cv.h : 19.0f, hd[3] = {0, 0, 0}, ws[3] = {0, 0, 0}, sc[3] = {0, 0, 0};
    bool ok = s3d_bone_pos(o, tb, 0x2F, hd, sc) && s3d_bone_pos(o, tb, 0x03, ws) && s3d_valid(hd, ws, h);
    s3d_hide = true;
    if (!ok && ++s3d_ptries < 10) return;
    s3d_probed = true; s3d_hide = false;
    if (ok) { s3d_set_want(hd, ws, h); memcpy(s3d_known[cv.sys].v, s3d_want, sizeof s3d_want); s3d_feet_of(o, tb, h, s3d_known[cv.sys]); }
    static int logs = 0;
    if (logs++ < 24) { printf("select3d: personaje %d, reposo %s: cabeza (%.1f, %.1f, %.1f), cintura (%.1f, %.1f, %.1f), escalas %.1f %.1f %.1f, altura %.1f, centro y %.1f, distancia %.1f, %d cuadros\n", cv.sys,
                               ok ? "medido" : "no valido", hd[0], hd[1], hd[2], ws[0], ws[1], ws[2], sc[0], sc[1], sc[2], h, s3d_want[1], s3d_want[3], s3d_ptries + 1); fflush(stdout); }
}
// 3x3 part of a bone matrix (rows scaled per axis) -> unit quaternion (x, y, z, w) and the row scales; the inverse is in s3d_body
static bool s3d_dec(const float* m, float* q, float* sc) {
    float r[9];
    for (int row = 0; row < 3; row++) {
        sc[row] = sqrtf(m[4 * row] * m[4 * row] + m[4 * row + 1] * m[4 * row + 1] + m[4 * row + 2] * m[4 * row + 2]);
        if (!std::isfinite(sc[row]) || sc[row] < 1e-6f) return false;
        for (int col = 0; col < 3; col++) r[3 * row + col] = m[4 * row + col] / sc[row];
    }
    float det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
    if (det < 0) for (int col = 0; col < 3; col++) r[col] = -r[col];   // mirrored bone: the flip stays in the part before the rotation
    float tr = r[0] + r[4] + r[8];
    if (tr > 0) { float s = sqrtf(tr + 1.0f) * 2; q[3] = 0.25f * s; q[0] = (r[5] - r[7]) / s; q[1] = (r[6] - r[2]) / s; q[2] = (r[1] - r[3]) / s; }
    else if (r[0] > r[4] && r[0] > r[8]) { float s = sqrtf(1.0f + r[0] - r[4] - r[8]) * 2; q[3] = (r[5] - r[7]) / s; q[0] = 0.25f * s; q[1] = (r[3] + r[1]) / s; q[2] = (r[6] + r[2]) / s; }
    else if (r[4] > r[8]) { float s = sqrtf(1.0f + r[4] - r[0] - r[8]) * 2; q[3] = (r[6] - r[2]) / s; q[0] = (r[3] + r[1]) / s; q[1] = 0.25f * s; q[2] = (r[7] + r[5]) / s; }
    else { float s = sqrtf(1.0f + r[8] - r[0] - r[4]) * 2; q[3] = (r[1] - r[3]) / s; q[0] = (r[6] + r[2]) / s; q[1] = (r[7] + r[5]) / s; q[2] = 0.25f * s; }
    return std::isfinite(q[0] + q[1] + q[2] + q[3]);
}
static uint32_t s3d_seq = 0, s3d_seq_done = 0;   // one pose edit per drawn frame (the body draw can run more than once)
static void s3d_predraw(uint32_t o) {
    if (cv_owner() != 2) return;
    s3d_seq++;
    uint32_t tb = s3d_mtx(o);
    bool bad = !tb || tb >= 0x10000000u;
    if (s3d_phase == 0 && !s3d_probed) {
        if (!bad) s3d_probe(o, tb);
        else { s3d_hide = true; if (++s3d_ptries >= 10) { s3d_probed = true; s3d_hide = false; } }
    }
}
// body draw of the select model (0x0880CAF4, the bone matrices of this frame are already built): the fall's feet and the blend to idle
static void s3d_body(uint32_t o) {
    if (cv_owner() != 2 || cv.cur < 0 || o != cv_objp(cv.obj[cv.cur]) || s3d_seq == s3d_seq_done) return;
    s3d_seq_done = s3d_seq;
    uint32_t tb = s3d_mtx(o);
    if (!tb || tb >= 0x10000000u) return;
    uint32_t hdr = psp_l32(o + 0x14), nb = hdr && hdr < 0x10000000u ? psp_l32(hdr + 8) : 0;
    if (s3d_phase == 1) {   // falling: the feet never go under the floor, and in the last part of the fall they are taken down onto it
        s3d_off = 0.0f;
        auto kn = s3d_known.find(cv.sys);
        if (kn != s3d_known.end() && kn->second.nf && nb) {
            float fy = -1e9f, p[3];
            for (int i = 0; i < kn->second.nf; i++) if (s3d_bone_pos(o, tb, kn->second.feet[i], p)) fy = std::max(fy, p[1]);
            if (fy > -1e8f) {
                float g = kn->second.g, w = 0.0f;
                if (s3d_fall_len > 8 && s3d_fall_f0 >= 0) { float t = ((float)(s3d_frame - s3d_fall_f0) - 0.55f * s3d_fall_len) / (0.45f * s3d_fall_len); t = std::min(1.0f, std::max(0.0f, t)); w = t * t * (3.0f - 2.0f * t); }
                s3d_off = fy > g ? g - fy : w * (g - fy);
                uint32_t bt2 = psp_l32(hdr + 0x50);
                if (bt2 && bt2 < 0x08000000u) bt2 += hdr;
                if (s3d_off != 0.0f && bt2) for (uint32_t b = 0; b < nb && b < 256; b++) for (uint32_t h2 = 0; h2 < 128; h2 += 64) {   // every bone id of the model (physics bones have ids above the count)
                    uint32_t bid = psp_l16(bt2 + 0xA0 * b + 0xA) & 0xFFu, a = tb + bid * 128 + h2 + 4 * 13; if (bid < 0x77) cv_wf(a, cv_rf(a) + s3d_off);
                }
            }
        }
        return;
    }
    if (s3d_blend <= 0 || !s3d_nb) return;
    if (o != s3d_snap_o || hdr != s3d_snap_md) {
        static int logs = 0;
        if (logs++ < 20) printf("select3d: mezcla de pose descartada: el objeto cambio (0x%08X -> 0x%08X, modelo 0x%08X -> 0x%08X)\n", s3d_snap_o, o, s3d_snap_md, hdr);
        s3d_blend = 0; return;
    }
    float t = 1.0f - (float)s3d_blend / (float)(S3D_BLEND + 1);
    t = t * t * (3.0f - 2.0f * t);
    // by the skeleton: each bone's rotation blends as a quaternion and its joint keeps its offset from the parent (blended in the parent's
    // frame), so bones keep their length and the parts stay attached; the root's position blends in a line
    uint32_t bt = psp_l32(hdr + 0x50);
    if (bt && bt < 0x08000000u) bt += hdr;
    if (!bt || nb > 256) { s3d_blend--; return; }
    static float R0[256][9], R1[256][9], RB[256][9], P0[256][3], P1[256][3], PB[256][3];
    static bool okb[256];
    int snap_of[256], par[256], st[64], sp = 0;
    for (int i = 0; i < 256; i++) snap_of[i] = -1;
    for (int i = 0; i < s3d_nb; i++) snap_of[s3d_bone[i] & 255] = i;
    auto rot = [](const float* q, float* r) {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y + z * w); r[2] = 2 * (x * z - y * w);
        r[3] = 2 * (x * y - z * w); r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z + x * w);
        r[6] = 2 * (x * z + y * w); r[7] = 2 * (y * z - x * w); r[8] = 1 - 2 * (x * x + y * y);
    };
    auto mul = [](const float* v, const float* r, float* o) { for (int c = 0; c < 3; c++) o[c] = v[0] * r[c] + v[1] * r[3 + c] + v[2] * r[6 + c]; };
    auto mulT = [](const float* v, const float* r, float* o) { for (int c = 0; c < 3; c++) o[c] = v[0] * r[3 * c] + v[1] * r[3 * c + 1] + v[2] * r[3 * c + 2]; };
    // a bone matrix is X * R: R the rotation, X whatever comes before it (the model box scale, a bone's own scale or shear, as in hair);
    // R blends as a quaternion, X in a line, so both ends of the blend are exact for any matrix
    auto nrm = [](const float* m, float* o) { for (int r = 0; r < 3; r++) { float n = sqrtf(m[4 * r] * m[4 * r] + m[4 * r + 1] * m[4 * r + 1] + m[4 * r + 2] * m[4 * r + 2]); n = n > 1e-9f ? 1.0f / n : 0.0f; for (int c = 0; c < 3; c++) o[3 * r + c] = m[4 * r + c] * n; } };
    auto unit = [](float* q) { float n = 1.0f / sqrtf(std::max(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1e-12f)); for (int k = 0; k < 4; k++) q[k] *= n; };
    // out 4x4 rows 0..2 = lerp(M0 R0^T, M1 R1^T) Rb; returns the row-normalized result in nb9
    auto build = [&](const float* m0, const float* r0, const float* m1, const float* r1, const float* rb, float* o12, float* nb9) {
        for (int r = 0; r < 3; r++) {
            float x[3];
            for (int c = 0; c < 3; c++) {
                float a0 = m0[4 * r] * r0[3 * c] + m0[4 * r + 1] * r0[3 * c + 1] + m0[4 * r + 2] * r0[3 * c + 2];
                float a1 = m1[4 * r] * r1[3 * c] + m1[4 * r + 1] * r1[3 * c + 1] + m1[4 * r + 2] * r1[3 * c + 2];
                x[c] = a0 + (a1 - a0) * t;
            }
            for (int c = 0; c < 3; c++) o12[4 * r + c] = x[0] * rb[c] + x[1] * rb[3 + c] + x[2] * rb[6 + c];
        }
        nrm(o12, nb9);
    };
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t rec = bt + 0xA0 * i;
        int id = psp_l16(rec + 0xA) & 0xFF;
        par[i] = sp > 0 ? st[sp - 1] : -1;
        if (sp < 64) st[sp] = (int)i;
        int ind = (int)psp_l32(rec + 4) & 0xFFFF;
        if (ind > 0) { sp -= ind; if (sp < -1) sp = i == 0 ? -1 : 0; }
        sp++;
        if (sp < 0) sp = 0;
        okb[i] = false;
        int k0 = id < 0x77 ? snap_of[id] : -1;   // by id: physics bones (hair, cloth) have ids above the bone count
        if (k0 < 0) continue;
        uint32_t a = tb + (uint32_t)id * 128;
        const float* m0 = s3d_pose[k0];
        float m[16], q0[4], q1[4], s0[3], s1[3], bp[3], v[3], n0[9], n1[9], o[12], nn[9];
        for (int k = 0; k < 16; k++) m[k] = cv_rf(a + 4 * (uint32_t)k);
        for (int k = 0; k < 3; k++) bp[k] = cv_rf(rec + 0x10 + 4 * (uint32_t)k);
        if (!s3d_dec(m0, q0, s0) || !s3d_dec(m, q1, s1)) continue;
        unit(q0); unit(q1);
        rot(q0, R0[i]); rot(q1, R1[i]);
        nrm(m0, n0); nrm(m, n1);
        mul(bp, n0, v); for (int k = 0; k < 3; k++) P0[i][k] = v[k] + m0[12 + k];
        mul(bp, n1, v); for (int k = 0; k < 3; k++) P1[i][k] = v[k] + m[12 + k];
        if (q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3] < 0) for (int k = 0; k < 4; k++) q1[k] = -q1[k];
        float q[4];
        for (int k = 0; k < 4; k++) q[k] = q0[k] + (q1[k] - q0[k]) * t;
        unit(q);
        rot(q, RB[i]);
        int pr = par[i];
        if (pr >= 0 && okb[pr]) {
            float d0[3], d1[3], w0[3], w1[3], d[3];
            for (int k = 0; k < 3; k++) { w0[k] = P0[i][k] - P0[pr][k]; w1[k] = P1[i][k] - P1[pr][k]; }
            mulT(w0, R0[pr], d0); mulT(w1, R1[pr], d1);
            for (int k = 0; k < 3; k++) d[k] = d0[k] + (d1[k] - d0[k]) * t;
            mul(d, RB[pr], v);
            for (int k = 0; k < 3; k++) PB[i][k] = PB[pr][k] + v[k];
        } else for (int k = 0; k < 3; k++) PB[i][k] = P0[i][k] + (P1[i][k] - P0[i][k]) * t;
        okb[i] = true;
        build(m0, R0[i], m, R1[i], RB[i], o, nn);
        mul(bp, nn, v);
        for (int row = 0; row < 3; row++) for (int col = 0; col < 3; col++) cv_wf(a + 4 * (uint32_t)(4 * row + col), o[4 * row + col]);
        for (int col = 0; col < 3; col++) cv_wf(a + 4 * (uint32_t)(12 + col), PB[i][col] - v[col]);
        // second matrix of the bone (the other end of its weighted vertices): its own rotation blended, held at the same joint
        float m2[16], p0[4], p1[4], t0[3], t1[3], ra[9], rb[9], rm[9], qa[3], qb[3];
        for (int k = 0; k < 16; k++) m2[k] = cv_rf(a + 64 + 4 * (uint32_t)k);
        if (!s3d_dec(m0 + 16, p0, t0) || !s3d_dec(m2, p1, t1)) continue;
        unit(p0); unit(p1);
        rot(p0, ra); rot(p1, rb);
        nrm(m0 + 16, n0); nrm(m2, n1);
        mul(bp, n0, v); for (int k = 0; k < 3; k++) qa[k] = v[k] + m0[28 + k] - P0[i][k];
        mul(bp, n1, v); for (int k = 0; k < 3; k++) qb[k] = v[k] + m2[12 + k] - P1[i][k];
        if (p0[0] * p1[0] + p0[1] * p1[1] + p0[2] * p1[2] + p0[3] * p1[3] < 0) for (int k = 0; k < 4; k++) p1[k] = -p1[k];
        float pq[4];
        for (int k = 0; k < 4; k++) pq[k] = p0[k] + (p1[k] - p0[k]) * t;
        unit(pq);
        rot(pq, rm);
        build(m0 + 16, ra, m2, rb, rm, o, nn);
        mul(bp, nn, v);
        for (int row = 0; row < 3; row++) for (int col = 0; col < 3; col++) cv_wf(a + 64 + 4 * (uint32_t)(4 * row + col), o[4 * row + col]);
        for (int col = 0; col < 3; col++) cv_wf(a + 64 + 4 * (uint32_t)(12 + col), PB[i][col] + qa[col] + (qb[col] - qa[col]) * t - v[col]);
    }
    s3d_blend--;
}

static uint32_t s3d_file(const char* name) {
    std::string f = std::string("../../games/ttt/select3d/") + name;
    FILE* fp = fopen(f.c_str(), "rb");
    if (!fp) { printf("select3d: falta games/ttt/select3d/%s\n", name); return 0; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint32_t a = n > 4 && n < 0x200000 ? psp_reserve((uint32_t)n + 64) : 0;
    if (a && fread(psp_p(a), 1, (size_t)n, fp) != (size_t)n) a = 0;
    fclose(fp);
    printf("select3d: %s %s (%ld bytes)\n", name, a ? "cargado" : "no valido", n);
    return a;
}

// pair loader of the select page (wrapped with the roster's bind)
static bool s3d_pair(Ctx& c, void (*orig)(Ctx&)) {
    if (!ro_bind(c, orig)) orig(c);
    uint32_t page = c.r[30];
    if (page && page != s3d_obj) {
        s3d_obj = page; s3d_vt = psp_l32(page + 0x40); s3d_n = s3d_rn = -1;
        printf("select3d: pagina de seleccion 0x%08X (renders grandes cargados)\n", page); fflush(stdout);
    }
    return true;
}

// instance draw: the big render of character n is being drawn this frame
// returns true when the render is not drawn (the 3D of that character is on screen)
static bool s3d_inst(Ctx& c) {
    if (!s3d_alive()) return false;
    uint32_t inst = c.r[4];
    for (int n = 0; n < 0xA7; n++) {
        uint32_t h = psp_l32(s3d_obj + 0x1A8 + 4 * (uint32_t)n);
        if (h && psp_l32(h) == inst) { s3d_rn = n; s3d_seen = s3d_frame; return cv_owner() == 2 && cv.visible && cv.cur >= 0; }
    }
    return false;
}

static void s3d_cfg() {
    if (const char* e = getenv("TTT_SEL3D_FALL")) sscanf(e, "%f,%d", &s3d_fspeed, &s3d_fmode);
    if (const char* e = getenv("TTT_SEL3D_CAM")) sscanf(e, "%f,%f,%f,%f,%f,%f", &s3d_cam_t0, &s3d_cam_top, &s3d_cam_bot, &s3d_cam_min, &s3d_cam_up, &s3d_cam_side);
    s3d_fspeed = std::max(0.25f, s3d_fspeed);
    s3d_full = psp_sel_layout3d();
    if (s3d_full) { S3D_X0 = 0; S3D_X1 = 274; S3D_Y0 = 52; S3D_Y1 = 248; }   // below the player bar (y 32..51), above the navbar (y 249)
    if (const char* e = getenv("TTT_SEL3D_CAM_FULL")) s3d_head_env = sscanf(e, "%f,%f,%f,%f", &s3d_full_t0, &s3d_full_top, &s3d_full_bot, &s3d_full_head) >= 4;
    if (s3d_full) { printf("select3d: diseno para la vista 3D, panel (%d, %d)-(%d, %d), cuerpo completo %.3f, %.2f, %.2f\n", S3D_X0, S3D_Y0, S3D_X1, S3D_Y1, s3d_full_t0, s3d_full_top, s3d_full_bot); fflush(stdout); }
    printf("select3d: caida modo %d, velocidad %.2f; camara %.2f, %.2f, %.2f\n", s3d_fmode, s3d_fspeed, s3d_cam_t0, s3d_cam_top, s3d_cam_bot); fflush(stdout);
}
// one line per change of character: frames from the cursor change to the object, to the head in the panel, to the landing
static void s3d_report(const char* why) {
    if (s3d_sw.sys < 0) return;
    static int logs = 0;
    auto rel = [](int f) { return f >= 0 ? f - s3d_sw.f0 : -1; };
    if (logs++ < 40) { printf("select3d: personaje %d (menu %d): objeto a los %d cuadros del cursor (%llu ms%s), aterriza a los %d%s\n", s3d_sw.sys, s3d_sw.n, rel(s3d_sw.created),
                              (unsigned long long)s3d_sw.ms, s3d_sw.hit ? ", cache" : "", rel(s3d_sw.landed), why); fflush(stdout); }
    s3d_sw = S3dSwitch();
}
static uint32_t s3d_idx(const Ctx& c, int s) { return mpm_call(c, 0x089C6CA0u, (uint32_t)s, 0) & 0xFFFFu; }
// colour 1 of every character of the grid kept built in RAM, nearest to the cursor first
static void s3d_hold_all(const Ctx& c) {
    std::vector<std::pair<int, uint32_t>> v;
    for (int n = 0; n < 0xA7; n++) { int s = s3d_sys_of_menu(n); if (s >= 0) v.push_back({abs(n - std::max(0, s3d_n)), s3d_idx(c, s)}); }
    std::stable_sort(v.begin(), v.end(), [](const std::pair<int, uint32_t>& x, const std::pair<int, uint32_t>& y) { return x.first < y.first; });
    for (auto& e : v) psp_disc_hold(e.second, false);
    psp_disc_hold_battle();
    psp_sel_images_warm();
    printf("select3d: %d personajes en cola para memoria\n", (int)v.size()); fflush(stdout);
}
static void s3d_hold_near(const Ctx& c) {   // the other colours of the character under the cursor go right after the neighbours
    static int cols = -1;
    if (cols < 0) { const char* e = getenv("TTT_SEL3D_COLOURS"); cols = e && *e ? atoi(e) : 1; }
    int cs = s3d_sys_of_menu(s3d_n);
    if (cols && cs >= 0) for (int k = psp_col_count(cs) - 1; k >= 1; k--) psp_disc_hold(mpm_call(c, 0x089C6CA0u, (uint32_t)cs, (uint32_t)k) & 0xFFFFu, true, false);
    for (int dn : {4, -4, 2, -2, 1, -1, 0}) { int ns = s3d_sys_of_menu(s3d_n + dn); if (ns >= 0) psp_disc_hold(s3d_idx(c, ns), true); }
}
// camera of a character, once: bind position of the head (accepted between 0.5 and 1.1 heights), target a bit below it, distance from the height
// provisional camera while it falls: the one measured before for this character, else from the bind positions, else from the height
static void s3d_frame_of(uint32_t o) {
    float h = cv_height(o), hd[3], ws[3];
    cv.h = h;
    auto it = s3d_known.find(cv.sys);
    if (it != s3d_known.end()) { memcpy(s3d_want, it->second.v, sizeof s3d_want); s3d_probed = true; s3d_hide = false; return; }
    if (s3d_bind_pos(o, 0x2F, hd) && s3d_bind_pos(o, 0x03, ws) && s3d_valid(hd, ws, h)) { s3d_set_want(hd, ws, h); return; }
    s3d_want[0] = 0.0f; s3d_want[1] = -0.62f * h; s3d_want[2] = 0.0f; s3d_want[3] = 0.42f * h / std::max(0.05f, s3d_cam_t0);
    if (s3d_full) { float z[3] = {0, 0, 0}; s3d_set_want(z, z, h); }
}
// cursor of the select (what the page compares with the render shown, 0x089F8624: [page+0xC] vtable +0xA0 with [page+0x68], +4 = menu number)
static int s3d_colour = 0, s3d_dmg = 0;   // colour and costume (0 normal, 1 level 1, 2 level 2) of the player under the selector
static bool s3d_recolour = false;   // colour under the colour selector (record +0xC), 0 outside it
static int s3d_cursor(const Ctx& c) {
    uint32_t own = psp_l32(s3d_obj + 0xC), vt = own && own < 0x10000000u ? psp_l32(own + 0x40) : 0;
    if (!vt || vt >= 0x10000000u) return -1;
    uint32_t off = (uint32_t)(int32_t)(int16_t)psp_l16(vt + 0xA0), fn = psp_l32(vt + 0xA4);
    if (fn < 0x08804000u || fn >= 0x0A000000u) return -1;
    uint32_t r = mpm_call(c, fn, own + off, psp_l32(s3d_obj + 0x68));
    int n = r && r < 0x10000000u ? (int)psp_l32(r + 4) : -1;
    static int keep_n = -1, keep_c = 0, keep_d = 0;   // the colour and costume chosen stay while the cursor stays on that character
    uint32_t pl = psp_l32(s3d_obj + 0x68);
    if (n >= 0 && psp_time_us() - cs_shown_t < 100000) {
        int k = (int)psp_l32(r + 0xC);
        keep_n = n; keep_c = k >= 0 && k < 20 ? k : 0; keep_d = pl < 4 ? cs_sel[pl] : 0;
    } else if (n != keep_n) keep_n = -1, keep_c = 0, keep_d = 0;
    s3d_colour = keep_c; s3d_dmg = keep_d;
    return n >= 0 && n < 0xA7 ? n : -1;
}

// jobs when the select opens (or Regenerate was pressed): every character and colour without a render of this quality
static void gen_check(const Ctx& c) {
    static int last_cfg = -1;
    if (gen_run || (s3d_frame % 30 && gen_page == s3d_obj)) return;
    int on = gen_env("TTT_GEN_RENDERS", 0), go = gen_env("TTT_GEN_RENDERS_GO", 0), q = gen_env("TTT_GEN_RENDERS_Q", 2), cfg = on | q << 1 | (int)(psp_render_tweaks_gen() << 4);
    bool force = go != gen_go_seen, page = gen_page != s3d_obj, changed = cfg != last_cfg;
    gen_go_seen = go; gen_page = s3d_obj; last_cfg = cfg;
    if (!on || (!force && !page && !changed)) return;
    gen_q = q >= 4 ? 4 : q >= 2 ? 2 : 1;
    gen_jobs.clear(); gen_i = 0;
    std::set<int> seen;
    int all = 0, dmg_all = 0, dmg_n = 0, lim = gen_env("TTT_GEN_LIMIT", 0);
    for (int n = 0; n < 0xA7; n++) {
        int s = s3d_sys_of_menu(n);
        if (s < 0 || !seen.insert(s).second) continue;
        int nc = std::max(1, psp_col_count(s));
        for (int k = 0; k < nc; k++) {
            all++;
            if ((force || !psp_render_have(s, k, gen_q)) && (!lim || (int)gen_jobs.size() < lim)) gen_jobs.push_back({s, k, 0});
            uint32_t d = 0;
            if (!psp_disc_dmg(mpm_call(c, 0x089C6CA0u, (uint32_t)s, (uint32_t)k) & 0xFFFFu, &d)) continue;
            all++; dmg_all++;
            if ((force || !psp_render_have(s, k + GEN_DMG, gen_q)) && (!lim || (int)gen_jobs.size() < lim)) { gen_jobs.push_back({s, k, 2}); dmg_n++; }
        }
    }
    printf("render: %d de %d renders por generar (calidad x%d%s), trajes danados nivel 2: %d de %d\n", (int)gen_jobs.size(), all, gen_q, force ? ", regenerar" : "", dmg_n, dmg_all); fflush(stdout);
    if (gen_jobs.empty()) return;
    gen_run = true; gen_state = 0; gen_tries = 0; gen_t0 = gen_tj = psp_time_us(); gen_jf = s3d_frame;
    if (cv.on) { cv_term(c); s3d_cache_free(c); }
    s3d_last_obj = -1; s3d_last_n = -1;
}
static void gen_end(const Ctx& c, const char* why) {
    printf("render: generacion %s, %d renders en %.1f s\n", why, (int)gen_i, (psp_time_us() - gen_t0) / 1e6); fflush(stdout);
    gen_run = false; gen_jobs.clear(); gen_i = 0; gen_state = 0;
    psp_window_busy(0, 0, 0);
    if (cv.on && !cv_busy()) { cv_term(c); s3d_cache_free(c); }
    s3d_last_obj = -1; s3d_last_n = -1; s3d_n = -1;
}
// a settled job is captured at the next draw; the result goes to the disc side (PNG files written by its own thread)
static void gen_tick(const Ctx& c) {
    if (gen_i >= gen_jobs.size()) { gen_end(c, "terminada"); psp_render_editor_request(); return; }
    const GenJob& j = gen_jobs[gen_i];
    psp_window_busy(1, (int)gen_i, (int)gen_jobs.size());
    if (gen_state != 3 && s3d_frame - gen_jf > 900) {
        printf("render: personaje %d color %d%s sin modelo listo en 900 cuadros, se salta\n", j.sys, j.col + 1, j.dmg ? " danado" : ""); fflush(stdout);
        gen_i++; gen_state = 0; gen_jf = s3d_frame; return;
    }
    if (gen_state == 0 && s3d_phase == 2 && cv.sys == j.sys && cv.colour == j.col && cv.dmg == j.dmg && cv.cur >= 0 && !cv_busy() && s3d_land_f > gen_jf && s3d_frame - s3d_land_f >= GEN_SETTLE) gen_state = 1;   // the model of this job (a colour change loads a new one)
    if (gen_state == 3) {
        std::vector<uint32_t> px; int w = 0, h = 0; float pts[9];
        if (psp_cap_take(gen_tag, px, &w, &h, pts)) {
            if (!w) { gen_end(c, "cancelada: la captura necesita Graphics > System > GPU rendering"); return; }
            psp_render_save(j.sys, j.col + (j.dmg ? GEN_DMG : 0), gen_q, std::move(px), w, h, pts);
            static int logs = 0;
            if (logs++ < 40 || gen_i + 1 == gen_jobs.size()) { printf("render: generado personaje %d color %d%s (%d de %d) en %llu ms\n", j.sys, j.col + 1, j.dmg ? " danado" : "", (int)gen_i + 1, (int)gen_jobs.size(), (unsigned long long)((psp_time_us() - gen_tj) / 1000)); fflush(stdout); }
            gen_i++; gen_state = 0; gen_tries = 0; gen_tj = psp_time_us(); gen_jf = s3d_frame;
            if (gen_i < gen_jobs.size()) {
                uint32_t idx = mpm_call(c, 0x089C6CA0u, (uint32_t)gen_jobs[gen_i].sys, (uint32_t)gen_jobs[gen_i].col) & 0xFFFFu, d = 0;
                psp_disc_hold(gen_jobs[gen_i].dmg && psp_disc_dmg(idx, &d) ? d : idx, true);
            }
        } else if (++gen_tries > 30) { gen_state = 1; gen_tries = 0; printf("render: captura sin respuesta, se repite (personaje %d color %d%s)\n", j.sys, j.col + 1, j.dmg ? " danado" : ""); fflush(stdout); }
    }
}

// once per frame from the pad read: open, follow the character shown, close when the select page is gone
int psp_sel_colour_of(int menu) { return s3d_alive() && menu == s3d_n ? s3d_colour + (s3d_dmg == 2 ? GEN_DMG : 0) : 0; }   // colour of the big render a number shows (generated renders)
int psp_tn_char_want();
void psp_tn_char_names(const std::vector<std::string>& v);
// names as text: the original characters' names from the game's text (group name ids, system txt1), once per language
static void tn_fill(const Ctx& c) {
    static int wait = 0;
    if (wait > 0) { wait--; return; }
    if (!psp_tn_char_want()) return;
    std::vector<std::string> v;
    int got = 0;
    for (int n = 0; n < roster::K_OLD; n++) {
        int k = (int)psp_l8(0x08A69720u + roster::VANILLA[n]);
        uint32_t id = k < roster::K_OLD && roster::name_orig()[k] ? roster::name_orig()[k] : psp_l32(0x08A70E90u + 4u * (uint32_t)k);
        uint32_t p = id ? mpm_call(c, 0x0883EE74u, id, 0xFFFFFFFFu) : 0;
        std::string u;
        for (int i = 0; p && p < 0x10000000u && i < 64; i++) {
            uint32_t ch = psp_l16(p + 2u * (uint32_t)i);
            if (!ch) break;
            if (ch < 0x80) u += (char)ch;
            else if (ch < 0x800) { u += (char)(0xC0 | (ch >> 6)); u += (char)(0x80 | (ch & 63)); }
            else { u += (char)(0xE0 | (ch >> 12)); u += (char)(0x80 | ((ch >> 6) & 63)); u += (char)(0x80 | (ch & 63)); }
        }
        got += !u.empty();
        v.push_back(u);
    }
    if (got < roster::K_OLD / 2) { wait = 60; return; }   // the system text is not loaded yet
    psp_tn_char_names(v);
}
// select voice (item 79): one of the two Character Reference samples of the character chosen, when its colour selector opens
// item 80: teleport entrance (TTT_SEL3D_ENTRY=1): the game's own vanish lines module, run in the select view. Manager 0x08A7CE68
// (update 0x088D673C, post 0x088D6810, draw 0x088D6E8C), request 0x088D4CE0(entity slot, kind 0 vanish / 1 appear), work
// [0x08B40854] (0 outside battle: the port's own work is put there only during its calls), texture common entry 0x266
static int s3d_entry() { const char* e = getenv("TTT_SEL3D_ENTRY"); return e && *e ? atoi(e) : 0; }
static uint32_t tp_w = 0;
static uint64_t tp_t = 0;
static bool tp_failed = false;
static uint32_t tp_g(uint32_t a) { return a && a < 0x10000000u ? psp_l32(a) : 0; }
static bool tp_ensure(const Ctx& c) {
    if (tp_w || tp_failed) return tp_w != 0;
    uint32_t pk = tp_g(0x08A7F280u) ? tp_g(tp_g(0x08A7F280u)) : 0;
    if (!pk) { tp_failed = true; printf("tp: sin paquete comun de batalla: no hay efecto\n"); fflush(stdout); return false; }
    uint32_t m = psp_reserve(0x60 + 0x50 + 0x4B00 + 0x48 + 64);
    if (!m) { tp_failed = true; printf("tp: sin memoria para el efecto\n"); fflush(stdout); return false; }
    m = (m + 15) & ~15u;
    memset(psp_p(m), 0, 0x60 + 0x50 + 0x4B00 + 0x48);
    uint32_t rq = m + 0x60, ln = rq + 0x50, tx = ln + 0x4B00;
    psp_s32(m, rq); psp_s32(m + 4, ln); psp_s32(m + 0x50, tx);
    uint32_t data = mpm_call(c, 0x0887BB40u, 0x266);
    mpm_call(c, 0x08902C10u, tx, data);
    tp_w = m;
    printf("tp: efecto del juego listo: trabajo 0x%08X, textura entrada 0x266 en 0x%08X, %u texturas\n", m, data, psp_l32(tx + 0x40)); fflush(stdout);
    return true;
}
// the module finds its object in the entity table 0x08B61178 (8 slots); the viewer's objects are put in slots 6 and 7 only during its calls
struct TpAlias { uint32_t o = 0; int id = -1; };
static TpAlias tp_al[2];
static uint32_t tp_alive(int k) { return tp_al[k].o && cv_objp(tp_al[k].id) == tp_al[k].o ? tp_al[k].o : 0; }
static uint32_t tp_call(const Ctx& c, uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0) {
    uint32_t keep = psp_l32(0x08B40854u), k6 = psp_l32(0x08B61178u + 24), k7 = psp_l32(0x08B61178u + 28);
    psp_s32(0x08B40854u, tp_w); psp_s32(0x08B61178u + 24, tp_alive(0)); psp_s32(0x08B61178u + 28, tp_alive(1));
    uint32_t r = mpm_call(c, fn, a0, a1);
    psp_s32(0x08B40854u, keep); psp_s32(0x08B61178u + 24, k6); psp_s32(0x08B61178u + 28, k7);
    return r;
}
static int tp_age[4] = {0, 0, 0, 0}, tp_hid = -1, tp_show = -1;
static uint32_t tp_hid_o = 0;
static uint64_t tp_hid_t = 0;
static void tp_slots(char* b, size_t n) {
    uint32_t rq = psp_l32(tp_w);
    int k = 0;
    for (uint32_t i = 0; i < 4; i++) k += snprintf(b + k, n - (size_t)k, "%s[%u 0x%X %u %u]", i ? " " : "", psp_l32(rq + 0x14 * i), psp_l32(rq + 0x14 * i + 0x10), psp_l32(rq + 0x14 * i + 4), psp_l32(rq + 0x14 * i + 8));
}
static void tp_req(const Ctx& c, int vid, int kind) {
    uint32_t o = cv_objp(vid);
    if (!o || !tp_ensure(c)) { static int logs = 0; if (logs++ < 10) { printf("tp: peticion sin objeto (visor %d)\n", vid); fflush(stdout); } return; }
    int k = tp_al[0].o == o ? 0 : tp_al[1].o == o ? 1 : -1;
    if (k < 0) {
        uint32_t rq = psp_l32(tp_w);
        bool used[2] = {false, false};
        for (uint32_t i = 0; i < 4; i++) { uint32_t q = psp_l32(rq + 0x14 * i); if (psp_l32(rq + 0x14 * i + 0x10) && (q == 6 || q == 7)) used[q - 6] = true; }
        k = !used[0] ? 0 : !used[1] ? 1 : (tp_al[0].o == tp_hid_o ? 1 : 0);
        tp_al[k].o = o; tp_al[k].id = vid;
    }
    int id = 6 + k;
    uint32_t r = tp_call(c, 0x088D4CE0u, (uint32_t)id, (uint32_t)kind);
    tp_t = psp_time_us();
    if (kind == 1) {
        if (tp_show < 0) { const char* e = getenv("TTT_SEL3D_TP_SHOW"); tp_show = e && *e ? atoi(e) : 2; }
        if (r && tp_show > 0) { tp_hid = id; tp_hid_o = o; tp_hid_t = tp_t; s3d_hide = true; }
    }
    static int logs = 0;
    if (logs++ < 40) {
        char b[160]; tp_slots(b, sizeof b);
        printf("tp: peticion %s, objeto %d (visor %d), personaje %d, resultado %u, ranuras %s, lineas %u, pausa 0x%08X\n", kind ? "aparecer" : "desaparecer", id, vid, cv.sys, r, b, psp_l32(tp_w + 8), psp_l32(0x08B48A20u + 0x5C60));
        fflush(stdout);
    }
}
static void tp_tick(const Ctx& c) {
    if (!tp_w) return;
    uint32_t rq = psp_l32(tp_w), busy = psp_l32(tp_w + 8);
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t f = psp_l32(rq + 0x14 * i + 0x10), id = psp_l32(rq + 0x14 * i);
        if (f && (id < 6 || id > 7 || !tp_alive((int)id - 6) || tp_age[i] > 12)) {
            static int logs = 0;
            if (logs++ < 20) { printf("tp: ranura %u liberada (objeto %u, banderas 0x%X, %d pasos, entidad 0x%08X)\n", i, id, f, tp_age[i], id == 6 || id == 7 ? tp_alive((int)id - 6) : 0); fflush(stdout); }
            psp_s32(rq + 0x14 * i + 0x10, 0); f = 0;
        }
        if (!f) tp_age[i] = 0;
        busy |= f;
    }
    uint64_t now = psp_time_us();
    if (tp_hid >= 0) {   // the new model stays hidden until its appear sequence reaches the body lines (TTT_SEL3D_TP_SHOW steps)
        bool show = now - tp_hid_t > 400000 || !cv.visible || cv.cur < 0 || cv_objp(cv.obj[cv.cur]) != tp_hid_o;
        bool found = false;
        for (uint32_t i = 0; i < 4; i++) if ((int)psp_l32(rq + 0x14 * i) == tp_hid && (psp_l32(rq + 0x14 * i + 0x10) & 2)) { found = true; if ((int)psp_l32(rq + 0x14 * i + 8) >= tp_show) show = true; }
        if (!found) show = true;
        if (show) { if (cv.visible && cv.cur >= 0 && cv_objp(cv.obj[cv.cur]) == tp_hid_o) s3d_hide = false; tp_hid = -1; }
    }
    if (!busy) return;
    for (int g = 0; now - tp_t >= 33333 && g < 2; g++) {
        tp_t += 33333;
        tp_call(c, 0x088D673Cu);
        if (tp_g(0x08A7D7D8u)) tp_call(c, 0x088D6810u);
        for (uint32_t i = 0; i < 4; i++) if (psp_l32(rq + 0x14 * i + 0x10)) tp_age[i]++;
    }
    if (now - tp_t > 100000) tp_t = now;
}
static void tp_draw(Ctx& c) {
    static uint32_t was = 0;
    uint32_t nl = tp_w ? psp_l32(tp_w + 8) : 0;
    if (nl && !was) { static int logs = 0; if (logs++ < 40) { printf("tp: dibujo del juego: %u lineas\n", nl); fflush(stdout); } }
    was = nl;
    if (!nl) return;
    uint32_t list = mpm_call(c, 0x08838054u, 0);
    tp_call(c, 0x088D6E8Cu);
    if (list) { mpm_call(c, 0x08838120u, list); mpm_call(c, 0x08837F54u, 0x08B4E690u, list, 0); }
}

static int sv_on() { static int v = -1, n = 0; if ((n++ & 255) == 0 || v < 0) { const char* e = getenv("TTT_SEL_VOICE"); v = e && *e ? atoi(e) : 1; } return v; }
static int sv_sample_id(int s) {   // the list id of the Character Reference samples: a renamed original keeps its new id
    for (auto& r : roster::defs().renames) if (r.sysid == s && r.newid >= 0 && r.newid != s) return r.newid < 161 ? r.newid : -1;
    return s >= 0 && s < 161 ? s : -1;
}
static void sv_tick(const Ctx& c) {
    if (sv_req >= 0) {
        int s = sv_req, b = sv_sample_id(s);
        sv_req = -1;
        if (cr_on() && sv_on() && b >= 0 && cv_owner() != 1) {
            std::string n = cv_sample_name(b, rand() % 2);
            if (psp_cr_voice(n) < 0) n = cv_sample_name(b, 0);
            if (psp_cr_voice(n) >= 0) { cr_vplay_file(n, 2); cr_vsys = s; }
            static int logs = 0;
            if (logs++ < 20) { printf("seleccion: voz %s del personaje %d%s\n", n.c_str(), s, psp_cr_voice(n) >= 0 ? "" : " (no esta en el mod)"); fflush(stdout); }
        }
    }
    if (cr_on() && cv_owner() != 1 && (cr_vplay || cr_vkill || !cr_vname.empty())) cr_vtick(c);
}
static void s3d_tick(const Ctx& c) {
    tp_tick(c);
    s3d_frame++;
    tn_fill(c);
    sv_tick(c);
    if (cv_owner() == 3 || cv_owner() == 4) return;   // the game room, the map select 3D (map3d.inc); the game room's models (room3d.inc) until it closes
    if (!gen_run && !s3d_enabled() && s3d_alive() && gen_env("TTT_GEN_RENDERS", 0)) { int cn = s3d_cursor(c); if (cn >= 0) s3d_n = cn; }   // without the 3D the big render still follows the colour
    if (s3d_alive() && s3d_rn >= 0 && cv_owner() != 1 && !cv_busy()) gen_check(c);
    if (gen_run && !s3d_alive()) gen_end(c, "interrumpida (select cerrada)");
    bool live = (s3d_enabled() || gen_run) && s3d_alive() && s3d_rn >= 0 && s3d_frame - s3d_seen <= 3;
    if (!live) {
        if (cv_owner() == 2 && !cv_busy()) { s3d_report(", select cerrada"); cv_term(c); s3d_cache_free(c); s3d_last_obj = -1; s3d_last_n = -1;  s3d_n = -1; s3d_hide = false; printf("select3d: cerrado\n"); fflush(stdout); }
        return;
    }
    if (cv_owner() == 1) return;
    if (!gen_run) {
        int cn = s3d_cursor(c);
        static int logs = 0;
        if (cn >= 0 && cn != s3d_n && logs++ < 6) { printf("select3d: cursor %d (render mostrado %d)\n", cn, s3d_rn); fflush(stdout); }
        s3d_n = cn >= 0 ? cn : s3d_rn;
    }
    if (!s3d_files) { s3d_files = true; s3d_fall = s3d_file("fall_land.anm"); s3d_idle = s3d_file("stand_loop.anm"); s3d_cfg(); }
    s3d_ctx = &c;
    cv_keep = s3d_keep; cv_cached = s3d_cached; cv_cache_put = s3d_put; cv_dropmark = s3d_dropmark; cv_predraw = s3d_predraw; cv_objmark = s3d_objmark;
    if (!cv.on) {
        // map chosen in the stage select, else the free battle setup's, else map 1 (originals 0..9 at PACKFILE 0x495 + 2i, new maps from the port's base)
        uint32_t cfg = psp_l32(0x08B63D88u + 0x2D4);
        int st = st_shown >= 0 ? st_shown : cfg < 126 ? (int)cfg : 0;
        int base = psp_stage_base();
        auto map_of = [&](int i) { return i < 10 ? 0x495 + 2 * i : base >= 0 ? base + 2 * (i - 10) : -1; };
        int m = map_of(st);
        uint32_t sz = m > 0 ? mpm_call(c, 0x08831CACu, (uint32_t)m) : 0;
        if (!sz || sz > 0x400000u) { st = 0; m = map_of(0); }
        if (gen_run) m = -1;
        cv_init(c, m, 2);
        s3d_map = m; s3d_cam_ok = false;
        printf("select3d: abierto, mapa %d (entrada %d), de la seleccion de escenario %d, de la configuracion %d\n", st, m, st_shown, (int)cfg); fflush(stdout);
        s3d_hold_all(c);
    }
    if (!gen_run && s3d_n != s3d_last_n) { s3d_last_n = s3d_n; s3d_hold_near(c); }
    int s = gen_run ? (gen_i < gen_jobs.size() ? gen_jobs[gen_i].sys : -1) : s3d_sys_of_menu(s3d_n);
    int want_dmg = gen_run ? 0 : s3d_dmg;
    if (gen_run && gen_i < gen_jobs.size()) { s3d_colour = gen_jobs[gen_i].col; want_dmg = gen_jobs[gen_i].dmg; }
    int w2 = want_dmg == 2 ? 2 : 0;
    static int col_for = -1;
    if (!gen_run && s >= 0 && s3d_colour > 0 && col_for != s) {   // colour selector open: every colour of this character in memory
        col_for = s;
        for (int k = 1; k < psp_col_count(s); k++) psp_disc_hold(mpm_call(c, 0x089C6CA0u, (uint32_t)s, (uint32_t)k) & 0xFFFFu, true);
    }
    if (s >= 0 && s == cv.sys && (s3d_colour != cv.colour || (w2 != (cv.dmg == 2 ? 2 : 0) && (cv.has2 || !w2))) && s3d_colour < cv.ncol && !cv_busy()) {   // same character, other colour or costume: no fall
        s3d_recolour = true;
        cv_show_sys(c, s, s3d_colour, w2, -1);
        static int logs = 0;
        if (logs++ < 8) { printf("select3d: personaje %d, color %d\n", s, s3d_colour + 1); fflush(stdout); }
    }
    if (s >= 0 && s != cv.sys && !cv_busy()) {
        s3d_recolour = false;
        s3d_report(s3d_sw.landed < 0 ? ", cambiado antes de aterrizar" : "");
        s3d_sw.sys = s; s3d_sw.n = s3d_n; s3d_sw.f0 = s3d_frame; s3d_sw.t0 = psp_time_us();
        s3d_hit = false; s3d_req = s3d_sw.t0;
        if (!gen_run && s3d_entry() == 1 && cv.visible && cv.cur >= 0 && s3d_phase == 2) tp_req(c, cv.obj[cv.cur], 0);
        cv_show_sys(c, s, s3d_colour, w2, -1); s3d_phase = 0; s3d_yaw = S3D_YAW;
    }
    if (s >= 0 && s == cv.sys && s3d_colour == cv.colour && cv.dmg != 2) cv.dmg = want_dmg == 1 && cv.has1 ? 1 : 0;   // level 1: same model, its textures
    uint32_t o = cv.cur >= 0 ? cv_objp(cv.obj[cv.cur]) : 0;
    if (o && cv.obj[cv.cur] != s3d_last_obj) {   // a new model: its idle pose is measured drawn hidden, then it falls and lands with the final camera, then its idle
        s3d_last_obj = cv.obj[cv.cur];
        if (s3d_sw.sys == cv.sys) { s3d_sw.created = s3d_frame; s3d_sw.ms = (psp_time_us() - s3d_sw.t0) / 1000; s3d_sw.hit = s3d_hit; }
        if (!psp_l32(o + 0x738 + 4 * 7) && s3d_idle) { psp_s32(o + 0x738 + 4 * 7, s3d_idle); mpm_call(c, 0x089B14C8u, o, 7, 2); }
        s3d_base_sp = cv_rf(o + 0xCA0);
        cv.framed = false; s3d_blend = 0; s3d_phase = 0; s3d_probed = false; s3d_ptries = 0; s3d_land_f = -1;
        s3d_newobj = true;
        tp_hid = -1;
    }
    if (o && s3d_phase == 0 && s3d_probed) {
        if (s3d_recolour) { s3d_recolour = false; s3d_phase = 2; s3d_land_f = s3d_frame; }
        else if (s3d_fall && s3d_fmode != 2 && !gen_run && s3d_entry() != 1) {
            psp_s32(o + 0x738 + 4 * 6, s3d_fall); mpm_call(c, 0x089B14C8u, o, 6, 1); s3d_phase = 1; s3d_fall_f0 = s3d_frame; s3d_off = 0.0f;
            cv_wf(o + 0xCA0, s3d_base_sp * s3d_fspeed);
        } else { s3d_phase = 2; s3d_land_f = s3d_frame; if (!gen_run && s3d_entry() == 1) tp_req(c, cv.obj[cv.cur], 1); if (s3d_sw.sys == cv.sys) { s3d_sw.landed = s3d_frame; s3d_report(", sin caida"); } }
    }
    if (o && s3d_phase == 1 && psp_l32(o + 0xC94) == 0) {   // landed: idle, blended from the last pose
        s3d_phase = 2; s3d_land_f = s3d_frame;
        if (s3d_fall_f0 >= 0 && s3d_frame - s3d_fall_f0 > 8) s3d_fall_len = s3d_frame - s3d_fall_f0;   // the same file and speed for everyone: the next falls know when the landing comes
        cv_wf(o + 0xCA0, s3d_base_sp);
        s3d_snap(o);
        s3d_off = 0.0f;
        if (psp_l32(o + 0x738 + 4 * 7)) mpm_call(c, 0x089B14C8u, o, 7, 2);
        s3d_blend = S3D_BLEND;
        if (s3d_sw.sys == cv.sys) { s3d_sw.landed = s3d_frame; s3d_report(""); }
    }
    if (gen_run) gen_tick(c);
}

// name (0) and form label (1) plates over the 3D: no entrance animation (last frame), at the place the game's plate rests, 25% smaller
// (rect and scale taken from the game's own plate once it rests; until then the game's plate as it is)
struct S3dPlate { bool ok = false; int x = 0, y = 0, w = 0, h = 0; float sx = 1, sy = 1, col[4] = {1, 1, 1, 1}; };
static S3dPlate s3d_pl[2];
static const float S3D_PLATE = 0.75f;
static int s3d_last_frame(uint32_t i) {
    uint32_t sc = psp_l32(i + 0x30), a = sc ? psp_l32(sc) : 0, b = a ? psp_l32(a + 0x198) : 0;
    return b ? (int)psp_l32(b + 0x3C) - 1 : -1;
}
static void s3d_plate(Ctx& c, int k) {
    uint32_t base = k ? 0x6FCu : 0x458u;
    S3dPlate& p = s3d_pl[k];
    uint32_t hr = s3d_rn >= 0 ? psp_l32(s3d_obj + base + 4 * (uint32_t)s3d_rn) : 0, ir = hr ? psp_l32(hr) : 0, sl = hr ? psp_l32(hr + 8) : 0;
    if (!p.ok && ir && sl && sl < 0x10000000u && psp_l8(hr + 0x15)) {
        int last = s3d_last_frame(ir);
        if (last >= 0 && cv_rf(ir + 0x2C) >= (float)last) {
            p.x = (int)psp_l32(sl + 0xE4); p.y = (int)psp_l32(sl + 0xE8); p.w = (int)psp_l32(sl + 0xEC); p.h = (int)psp_l32(sl + 0xF0);
            p.sx = cv_rf(sl + 0x40); p.sy = cv_rf(sl + 0x54); p.ok = true;
            for (int q = 0; q < 4; q++) p.col[q] = cv_rf(sl + 0x20 + 4 * (uint32_t)q);
            printf("select3d: cartel %s: (%d, %d) %dx%d, escala %.2f x %.2f, color %.2f %.2f %.2f %.2f\n", k ? "de forma" : "de nombre", p.x, p.y, p.w, p.h, p.sx, p.sy, p.col[0], p.col[1], p.col[2], p.col[3]); fflush(stdout);
        }
    }
    bool shown = k == 0;
    for (int m = 0; !shown && m < 0xA7; m++) { uint32_t hm = psp_l32(s3d_obj + base + 4 * (uint32_t)m); shown = hm && psp_l32(hm) && psp_l8(hm + 0x15); }   // form label: while the game shows one (form select)
    uint32_t h = s3d_n >= 0 ? psp_l32(s3d_obj + base + 4 * (uint32_t)s3d_n) : 0, i = h ? psp_l32(h) : 0;
    if (!shown || !i) return;
    if (!p.ok) { if (s3d_rn == s3d_n) mpm_draw_holder(c, h); return; }
    int last = s3d_last_frame(i);
    if (last >= 0) cv_wf(i + 0x2C, (float)last);
    float r = 1.0f - S3D_PLATE;
    int x = k ? p.x : p.x + (int)(p.w * r), y = k ? p.y : p.y + (int)(p.h * r);
    psp_s32(i + 0x34, 0);
    psp_s32(i + 8, (uint32_t)x); psp_s32(i + 0xC, (uint32_t)y);
    psp_s32(i, psp_l32(h + 0x2C)); psp_s32(i + 4, psp_l32(h + 0x30));
    psp_s8(i + 0x4C, 1);
    for (int q = 0; q < 3; q++) psp_s32(i + 0x40 + 4 * q, psp_l32(h + 0x34 + 4 * q));
    cv_wf(i + 0x20, p.sx * S3D_PLATE); cv_wf(i + 0x24, p.sy * S3D_PLATE);
    psp_s32(i + 0x28, psp_l32(h + 0x28));
    for (int q = 0; q < 4; q++) cv_wf(i + 0x10 + 4 * q, p.col[q]);
    psp_s32(i + 0x50, 0);
    mpm_call(c, 0x089D1B94u, i);
}

static void s3d_mark_list(Ctx& c, uint32_t a, uint32_t b) {   // one port list with GE NOP markers, queued in order with the 3D lists
    uint32_t list = mpm_call(c, 0x08838054u, 0);
    uint32_t ctx = psp_l32(0x08B478D0u), p = ctx && ctx < 0x10000000u ? psp_l32(ctx + 8) : 0;
    if (p && p < 0x10000000u) { psp_s32(p, a); p += 4; if (b) { psp_s32(p, b); p += 4; } psp_s32(ctx + 8, p); }
    if (!list) return;   // nested: the words went into the open list
    mpm_call(c, 0x08838120u, list);
    mpm_call(c, 0x08837F54u, 0x08B4E690u, list, 0);
}

// frame end: the stage and the model in the panel, then the name and the form label of the character over them
static void s3d_draw(Ctx& c) {
    if (cv_owner() != 2 || !cv.visible || cv.cur < 0 || !s3d_alive() || s3d_frame - s3d_seen > 3) return;
    uint32_t o = cv_objp(cv.obj[cv.cur]);
    if (!o || !psp_l32(0x08A7E260u)) return;
    if (!cv.framed) { s3d_frame_of(o); cv.framed = true; }
    uint64_t now = psp_time_us();
    float dt = s3d_cam_t ? std::min(0.1f, (now - s3d_cam_t) / 1e6f) : 0.0f, k = 1.0f - expf(-dt * 9.0f);
    s3d_cam_t = now;
    if (gen_run && s3d_phase != 0) memcpy(s3d_cam, s3d_want, sizeof s3d_want), s3d_cam_ok = true;
    else if (s3d_phase != 0) {   // eased only between characters; during the probe the camera waits
        if (!s3d_cam_ok) { memcpy(s3d_cam, s3d_want, sizeof s3d_want); s3d_cam_ok = true; }
        else for (int i = 0; i < 4; i++) s3d_cam[i] += (s3d_want[i] - s3d_cam[i]) * k;
    } else if (!s3d_cam_ok) memcpy(s3d_cam, s3d_want, sizeof s3d_want);
    {
        const char* e = getenv("TTT_SEL3D_ROTATE");
        if (gen_run || (e && *e == '0')) s3d_yaw = S3D_YAW;
        else s3d_yaw += psp_input_rotate() * 2.4f * dt;
    }
    float dist = s3d_cam[3];
    float ctr[3] = {s3d_cam[0], s3d_cam[1], s3d_cam[2]}, eye[3] = {ctr[0] + dist * sinf(s3d_yaw), std::min(-1.0f, ctr[1] - dist * 0.05f), ctr[2] + dist * cosf(s3d_yaw)};
    {   // pan along the view's x axis (the camera of the last frame): the model a bit to the left
        float m[16];
        for (int i = 0; i < 16; i++) m[i] = cv_rf(cv.cam + 0x40 + 4 * (uint32_t)i);
        bool row = fabsf(m[12]) + fabsf(m[13]) + fabsf(m[14]) >= fabsf(m[3]) + fabsf(m[7]) + fabsf(m[11]);
        float r[3] = {row ? m[0] : m[0], row ? m[4] : m[1], row ? m[8] : m[2]}, n = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), d = s3d_cam_side * cv.h;
        if (n > 1e-3f && std::isfinite(n)) for (int i = 0; i < 3; i++) { ctr[i] += r[i] / n * d; eye[i] += r[i] / n * d; }
    }
    if (s3d_newobj) { s3d_newobj = false; s3d_mark_list(c, 0x00ED0000u, 0); }   // the GPU forgets where the last model was not
    s3d_mark_list(c, 0x00E70000u | (uint32_t)(S3D_X0 / 2) | (uint32_t)(S3D_X1 / 2) << 8, 0x00E80000u | (uint32_t)(S3D_Y0 / 2) | (uint32_t)(S3D_Y1 / 2) << 8);
    cv.face = -1;
    bool cap = gen_run && gen_state == 1;
    if (cap) {   // the head and waist of this pose locate the face crops
        uint32_t tb = s3d_mtx(o);
        float hd[3] = {0, 0, 0}, ws[3] = {0, 0, 0}, mo[3] = {0, 0, 0};
        bool mok = false; int mid = 0;
        if (tb && tb < 0x10000000u) {
            s3d_bone_pos(o, tb, 0x2F, hd); s3d_bone_pos(o, tb, 0x03, ws);
            s3d_head_bones(o, cv.sys);
            mok = s3d_bone_pos(o, tb, 0x2E, mo); mid = 0x2E;   // neck: the head bone sits anywhere between the chin and the eyes by model, the neck is steady
        }
        float dm = mok ? mo[1] - hd[1] : 0.0f;   // neck below head (world, +y down)
        if (!(dm > 0.02f && dm < 6.0f)) mok = false;
        float wspan = ws[1] - hd[1], rw = wspan > 0.5f && wspan < 40.0f ? wspan / 5.5f : 1.0f;   // head to waist in world units over Goku's (render_frame.h)
        if (const char* e = getenv("TTT_GEN_RW")) rw = (float)atof(e);
        static int logs = 0;
        if (logs++ < 60) { printf("render: personaje %d, distancia cabeza-cintura %.2f (%.2f de Goku), cuello bajo la cabeza %.2f (hueso %02X)%s\n", cv.sys, wspan, rw, dm, mid, mok ? "" : " (sin cuello)"); fflush(stdout); }
        gen_tag++;
        float tw[6]; psp_render_tweak(cv.sys, tw);
        psp_cap_diag(gen_env("TTT_CAP_DIAG", -1) == cv.sys);
        psp_cap_request(gen_tag, rw, tw, hd, ws, mok ? mo : nullptr);
        s3d_mark_list(c, 0x00EB0000u, 0);
    }
    cel_force_now = cap && gen_env("TTT_GEN_CEL", 0) > 0;
    cv_scene(c, o, eye, ctr);
    cel_force_now = false;
    if (!cap) tp_draw(c);
    if (cap) { s3d_mark_list(c, 0x00EC0000u, 0); gen_state = 3; gen_tries = 0; }
    s3d_mark_list(c, 0x00E90000u, 0);
    if (gen_run) return;
    // name and form label of the shown character over the 3D (the select page's holders +0x458 + 4n and +0x6FC + 4n)
    uint32_t list = mpm_call(c, 0x08838054u, 0);
    mpm_call(c, 0x08A6495Cu, 0, 0, 480, 272);
    bool zt = (psp_l32(0x08B47888u + 0x44) & 2) != 0;
    mpm_call(c, 0x08A64240u, 1);
    for (int k = 0; k < 2; k++) s3d_plate(c, k);
    if (zt) mpm_call(c, 0x08A641E8u, 1);
    mpm_call(c, 0x08838120u, list);
    mpm_call(c, 0x08837F54u, 0x08B4E690u, list, 0);
    static int logs = 0;
    if (logs++ < 1) { printf("select3d: primer dibujo, personaje %d (menu %d), escenario %s\n", cv.sys, s3d_n, psp_l32(0x08A7E678u) ? "si" : "no"); fflush(stdout); }
}

// item 43: grid page draw (0x089F9F34) frames the cell whose id (0x08A0679C(pos)) == record +0; the next player's record
// starts with an id outside the grid (99) while the page shows position 0, so no frame: the record gets the id of position 0
static void s3d_grid_draw(Ctx& c, void (*orig)(Ctx&)) {
    static int fix = -1;
    if (fix < 0) { const char* e = getenv("TTT_SEL_CURSOR"); fix = !(e && *e == '0'); }
    uint32_t g = c.r[4];
    uint32_t own = fix && g && g < 0x10000000u ? psp_l32(g + 0xC) : 0, vt = own && own < 0x10000000u ? psp_l32(own + 0x40) : 0;
    if (vt && vt < 0x10000000u) {
        uint32_t off = (uint32_t)(int32_t)(int16_t)psp_l16(vt + 0xA0), fn = psp_l32(vt + 0xA4);
        uint32_t r = fn >= 0x08804000u && fn < 0x0A000000u ? mpm_call(c, fn, own + off, psp_l32(g + 0x68)) : 0;
        static uint32_t ok_r = 0, ok_id = 0xFFFFFFFFu;
        uint32_t id = r && r < 0x10000000u ? psp_l32(r) : 0xFFFFFFFFu;
        if (id != 0xFFFFFFFFu && psp_l32(r + 4) != (uint32_t)RO_RANDOM && (r != ok_r || id != ok_id)) {
            int n = ro_get().total;
            bool found = false;
            for (int k = 0; k < n && !found; k++) found = mpm_call(c, 0x08A0679Cu, (uint32_t)k) == id;
            if (!found) {
                uint32_t id0 = mpm_call(c, 0x08A0679Cu, 0);
                psp_s32(r, id0);
                static int logs = 0;
                if (logs++ < 20) { printf("seleccion: cursor del jugador %u fuera de la rejilla (%d), pasa a la primera casilla (%u), personaje %d\n", psp_l32(g + 0x68), (int)id, id0, (int)psp_l32(r + 4)); fflush(stdout); }
                id = id0;
            }
            ok_r = r; ok_id = id;
        }
    }
    orig(c);
}

// form selector draw (0x089FF8C8): thumbnails (+0x84 + 4 * menu) and cursor (+0x80) are AGS 15 holders placed at the AGS 35
// slots without binding, so the slot scale never applies: with the 3D layout they are drawn at TTT_SEL3D_FORM_SCALE (2)
static void s3d_form_draw(Ctx& c, void (*orig)(Ctx&)) {
    static float k = -1.0f;
    if (k < 0.0f) { const char* e = getenv("TTT_SEL3D_FORM_SCALE"); k = e && *e ? (float)atof(e) : 2.0f; }
    uint32_t sel = c.r[4];
    if (!psp_sel_layout3d() || !(k > 0.0f) || k == 1.0f || !sel || sel >= 0x10000000u) { orig(c); return; }
    uint32_t hs[168], sv[168][2]; int n = 0;
    for (int i = 0; i < 168; i++) {
        uint32_t h = psp_l32(sel + 0x80 + 4 * (uint32_t)i);
        if (!h || h >= 0x10000000u) continue;
        hs[n] = h; sv[n][0] = psp_l32(h + 0x20); sv[n][1] = psp_l32(h + 0x24);
        for (int j = 0; j < 2; j++) { float f; memcpy(&f, &sv[n][j], 4); f *= k; uint32_t u; memcpy(&u, &f, 4); psp_s32(h + 0x20 + 4 * (uint32_t)j, u); }
        n++;
    }
    static int logs = 0;
    if (logs < 2) { logs++; float f; memcpy(&f, &sv[0][0], 4); printf("select3d: selector de forma, %d miniaturas y cursor a escala %.2f (antes %.2f)\n", n, (double)k, n ? (double)f : 0.0); fflush(stdout); }
    orig(c);
    for (int i = 0; i < n; i++) { psp_s32(hs[i] + 0x20, sv[i][0]); psp_s32(hs[i] + 0x24, sv[i][1]); }
}
