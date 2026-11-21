#include <mutex>
#include <chrono>
#include <map>
// heap_spill: allocations the game heap refuses (0x089BFD60 -> null) come from port blocks; their free (0x089BFDB0) stays here.
// Seen: with 4 local players the battle arena block (0x088FE300) got null and every arena allocation started at address 0
static std::vector<std::pair<uint32_t, uint32_t>> hs_regions;
static std::map<uint32_t, uint32_t> hs_free, hs_used;
static uint32_t hs_total = 0;
uint32_t psp_map_buf_size();
uint32_t psp_reserve_left();
uint32_t psp_alloc(uint32_t n);
static bool hs_mine(uint32_t p) { for (auto& r : hs_regions) if (p - r.first < r.second) return true; return false; }
static uint32_t hs_alloc(uint32_t size, uint32_t align) {
    size = (size + 63) & ~63u;
    if (align < 64) align = 64;
    for (int pass = 0; pass < 2; pass++) {
        for (auto it = hs_free.begin(); it != hs_free.end(); ++it) {
            uint32_t b = it->first, n = it->second, a = (b + align - 1) & ~(align - 1);
            if (a + size > b + n) continue;
            hs_free.erase(it);
            if (a > b) hs_free[b] = a - b;
            if (a + size < b + n) hs_free[a + size] = b + n - (a + size);
            hs_used[a] = size; hs_total += size;
            memset(psp_p(a), 0, size);
            return a;
        }
        uint32_t chunk = std::max((size + align + 0xFFFF) & ~0xFFFFu, 0x400000u);   // from the port pool: memory the HUD gave back serves the game too
        uint32_t r = size > 0x8000000u ? 0 : psp_alloc(chunk);
        if (!r) { printf("memoria: sin reserva para %u bytes (alineacion %u), quedan %u KB\n", size, align, psp_reserve_left() / 1024); fflush(stdout); return 0; }
        hs_regions.push_back({r, chunk}); hs_free[r] = chunk;
        printf("memoria: bloque de reserva 0x%08X de %u KB para lo que el monton del juego no pudo dar\n", r, chunk / 1024);
    }
    return 0;
}
static void hs_release(uint32_t p) {
    auto it = hs_used.find(p);
    if (it == hs_used.end()) return;
    uint32_t b = p, n = it->second;
    hs_total -= n; hs_used.erase(it);
    auto nx = hs_free.find(b + n);
    if (nx != hs_free.end()) { n += nx->second; hs_free.erase(nx); }
    auto pv = hs_free.lower_bound(b);
    if (pv != hs_free.begin()) { --pv; if (pv->first + pv->second == b) { b = pv->first; n += pv->second; hs_free.erase(pv); } }
    hs_free[b] = n;
}
// arena_room: arena record at [0x08A7D4D0] + 16 * id = {base, next, size, used}; 0x088FE410 never checks used against size.
// Seen: Pilaf Machine (BT3) ultimate, a later node pool landed on the class 6 pool and broke its owner link (no model, endless destroy)
static std::vector<uint32_t> ar_spill[16];
static void ar_drop(int id) { for (uint32_t p : ar_spill[id]) hs_release(p); ar_spill[id].clear(); }
static bool ar_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    uint32_t S = psp_l32(0x08A7D4D0u), id = c.r[4];
    if (dir == 0x088FE3BCu) { for (int k = 0; k < 16; k++) ar_drop(k); return false; }
    if (!S || id > 12 || !(psp_l32(S + 0xD8) >> (id + 1) & 1)) return false;
    uint32_t a = S + 16 * id;
    if (dir == 0x088FE4E8u) { ar_drop((int)id); return false; }
    uint32_t size = (c.r[5] + 0x1Fu) & ~0x1Fu, used = psp_l32(a + 0xC), cap = psp_l32(a + 8);
    if (!patch_arena_room.val || used + size <= cap) return false;
    uint32_t r = hs_alloc(size, 64);
    if (!r) return false;
    ar_spill[id].push_back(r);
    c.r[2] = r;
    static int logs = 0;
    if (logs++ < 12) { printf("memoria: arena %u llena (%u de %u bytes), %u bytes van a la reserva 0x%08X (desde 0x%08X)\n", id, used, cap, size, r, c.r[31] - 8); fflush(stdout); }
    return true;
}
bool psp_heap_arena(uint32_t* a, uint32_t* e);
// TTT_FREE_GUARD: a free of a newlib chunk that is already free (or has a broken header) is refused; a double free loops newlib for ever
static bool fg_bad(uint32_t p, const Ctx& c) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_FREE_GUARD"); on = e && *e ? atoi(e) : 1; }
    uint32_t a = 0, e = 0;
    if (!on || !p || !psp_heap_arena(&a, &e) || p < a + 16 || p >= e || (p & 7)) return false;
    uint32_t sz = psp_l32(p - 4) & ~7u, nx = p - 8 + sz;
    const char* why = nullptr;
    if (sz < 16 || nx + 8 > e) why = "cabecera rota";
    else if (!(psp_l32(nx + 4) & 1)) why = "ya estaba libre";
    if (!why) return false;
    static int logs = 0;
    if (logs++ < 30) { printf("memoria: liberacion rechazada de 0x%08X (%s, tamano %u), desde %s0x%08X, hilo %d\n", p, why, sz, c.r[31] < 0x08800000u ? "el port, ra " : "", c.r[31] - 8, psp_thread_current()); fflush(stdout); }
    return true;
}
static bool hs_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    if (dir == 0x089BFDB0u) {
        if (!hs_mine(c.r[4])) return fg_bad(c.r[4], c);
        hs_release(c.r[4]);
        return true;
    }
    uint32_t size = c.r[4], align = c.r[5], ra = c.r[31], mb = 0;
    if (size > 0x4000000u) {   // an impossible request: the guest return addresses on the stack name who asked
        static int logs = 0;
        if (logs++ < 4) {
            printf("memoria: pedido imposible de %u bytes, ra 0x%08X, pila:", size, ra);
            for (uint32_t k = 0, n = 0; k < 256 && n < 14; k++) { uint32_t v = psp_l32(c.r[29] + 4 * k); if (v >= 0x08804000u && v < 0x08A70000u && !(v & 3)) { printf(" 0x%08X", v); n++; } }
            printf("\n");
            if (ra == 0x089BFE54u && psp_l32(c.r[29]) == 0x089D1FC0u)   // holder setup 0x089D1EE8: its saved registers and its caller
                printf("memoria: soporte 0x%08X recurso 0x%X estados %d..%d, llamado desde 0x%08X\n", c.r[23], c.r[18], (int)c.r[19], (int)c.r[16], psp_l32(c.r[29] + 0x274) - 8);
            fflush(stdout);
        }
    }
    if (ra == 0x08856050u && size == 0x400000u && (rm_on || cv.on)) printf("mapas: AVISO, vista 3D %s aun abierta al reservar el buffer del mapa\n", rm_on ? "de la sala" : "del visor");
    if (ra == 0x08856050u && size == 0x400000u && (mb = psp_map_buf_size()) > size) c.r[4] = size = mb;   // battle map buffer (4 MB) sized for the biggest map
    orig(c);
    if (mb > 0x400000u) {
        if (c.r[2]) psp_s32(c.r[17] + 0x34, mb);
        printf("mapas: buffer de mapa de batalla de %u KB (original 4096 KB) en 0x%08X, registro 0x%08X\n", mb / 1024, c.r[2], c.r[17]);
    }
    if (c.r[2] || !size || !patch_heap_spill.val) return true;
    c.r[2] = hs_alloc(size, align);
    if (mb > 0x400000u && c.r[2]) { psp_s32(c.r[17] + 0x34, mb); printf("mapas: buffer de mapa de batalla servido desde la reserva en 0x%08X\n", c.r[2]); }
    static int logs = 0;
    if (!c.r[2] || logs++ < 40) printf("memoria: el monton del juego no dio %u bytes (desde 0x%08X), van a la reserva 0x%08X, en reserva %u KB\n", size, ra - 8, c.r[2], hs_total / 1024);
    return true;
}

// WLAN intro skip: the WLAN phase table (0x08A7DAC4) has no Start handler for the camera intro (phase 0) and the fighter
// intros (phase 1); they get the offline one (0x08905008). The intros run outside the lockstep (network phase 16/15,
// the barrier is the next phase), so the host's Start presses travel as a counter in a port packet (P_INTRO)
void psp_net_extra(int type, const uint8_t* d, int n);
uint32_t psp_net_intro(uint32_t* id);
static const int WS_P_INTRO = 12, WS_P_STATE = 13;
static uint32_t ws_phase = 0, ws_seen = 0, ws_id = 0, ws_count = 0, ws_used = 0, ws_cid = 0, ws_sent = 0;
static bool ws_prev = false;
static uint32_t s3d_a1a0_vb = 0;
static bool ws_wlan() { uint32_t m = psp_l32(0x08B48A28u); return m >= 11 && m <= 13; }
static bool ws_host() { return ws_phase >= 10 && !(ws_phase & 1); }
static void ws_put(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void rs_diag();
static void ws_quit_tick(Ctx& c);
static bool ws_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    uint32_t vb = psp_l32(0x08A78E14u);
    if (dir == 0x0881A140u) {
        orig(c);
        rs_diag();
        if (!psp_l32(0x08A7DAD8u) && !psp_l32(0x08A7DAF0u)) { psp_s32(0x08A7DAD8u, 0x08905008u); psp_s32(0x08A7DAF0u, 0x08905008u); }
        if (!ws_wlan() || !ws_host() || vb - ws_seen > 30) return true;
        bool on = (psp_l32(0x08AF31A0u + 4) & 8) != 0;
        if (on && !ws_prev) { ws_count++; printf("net: intro, el anfitrion pide saltar (%u)\n", ws_count); }
        ws_prev = on;
        if (ws_count && vb - ws_sent >= 3) { uint8_t d[8]; ws_put(d, ws_id); ws_put(d + 4, ws_count); psp_net_extra(WS_P_INTRO, d, 8); ws_sent = vb; }
        return true;
    }
    if (!ws_wlan()) return false;
    uint32_t rid = 0, rcnt = psp_net_intro(&rid);
    if (vb - ws_seen > 30) {
        if (ws_host()) { ws_id = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); ws_count = 0; ws_used = 0; ws_prev = true; }
        else if (rid == ws_cid) ws_used = rcnt;
        printf("net: intro con salto del anfitrion (%s), fase de red %u\n", ws_host() ? "anfitrion" : "cliente", ws_phase);
    }
    ws_seen = vb;
    uint32_t avail = ws_count;
    if (!ws_host()) { if (rid != ws_cid) { ws_cid = rid; ws_used = 0; } avail = rcnt; }
    c.r[2] = ws_used < avail;
    if (c.r[2]) { ws_used++; printf("net: intro saltada por el anfitrion (%s), salto %u\n", c.r[31] - 8 == 0x08908918u ? "camara" : "presentacion", ws_used); }
    return true;
}

// lockstep state check: at each complete frame every machine sends its fighters' life, ki, action and position and the
// rand count; fighters go by combatant (team/member), not by entity slot, whose order may differ between machines
struct WsState { uint32_t frame, rng, v[4][6]; uint8_t ent[4]; bool set; };
static WsState ws_mine[512], ws_theirs[512];
static std::mutex ws_mtx;
static int ws_bad = 0, ws_perm = 0;
static const int WS_P_QUIT = 19;
static std::atomic<int> ws_quit_team{-1};
void psp_net_quit_rx(const uint8_t* d, int n) { if (n >= 1) ws_quit_team = d[0]; }
static uint32_t ws_scenes = 0, rs_done = 0xFFFFFFFFu;
static void rs_life(const uint32_t*, const uint8_t*, int32_t me, uint32_t f);
static void rs_flush(const char* why);
static int32_t ld_me_now();
static uint32_t ws_good = 0;
// rand calls per lockstep frame on this machine: printed at the first difference to name who drew more
struct WsRand { uint32_t frame, ra, n; };
static WsRand ws_rr[256];
static uint32_t ws_rr_pos = 0, ws_cur = 0, ws_cur_vb = 0;
static bool ws_rr_shown = false;
static void ws_rand_rec(uint32_t ra) {
    if (!ws_cur || psp_l32(0x08A78E14u) - ws_cur_vb > 10) return;
    std::lock_guard<std::mutex> l(ws_mtx);
    WsRand& r = ws_rr[(ws_rr_pos - 1) & 255];
    if (ws_rr_pos && r.frame == ws_cur && r.ra == ra) { r.n++; return; }
    ws_rr[ws_rr_pos++ & 255] = {ws_cur, ra, 1};
}
static void ws_rand_show(uint32_t from, uint32_t to) {
    if (ws_rr_shown) return;
    ws_rr_shown = true;
    int shown = 0;
    for (uint32_t i = ws_rr_pos > 256 ? ws_rr_pos - 256 : 0; i < ws_rr_pos && shown < 24; i++) {
        WsRand& r = ws_rr[i & 255];
        if (r.frame + 2 < from || r.frame > to) continue;
        printf("net:   rand aqui en el cuadro %u desde 0x%08X x%u\n", r.frame, r.ra, r.n); shown++;
    }
    if (!shown) printf("net:   rand aqui: ninguna llamada entre los cuadros %u y %u\n", from, to);
}
static void ws_compare(uint32_t f) {
    WsState& a = ws_mine[f & 511]; WsState& b = ws_theirs[f & 511];
    if (!a.set || !b.set || a.frame != f || b.frame != f) return;
    bool same = a.rng == b.rng && !memcmp(a.v, b.v, sizeof(a.v));
    a.set = b.set = false;
    if (same) {
        if (f > ws_good) ws_good = f;
        if (memcmp(a.ent, b.ent, 4) && ws_perm < 2) { ws_perm++; printf("net: luchadores iguales en otro orden de entidades: aqui %u %u %u %u, el otro %u %u %u %u\n", a.ent[0], a.ent[1], a.ent[2], a.ent[3], b.ent[0], b.ent[1], b.ent[2], b.ent[3]); }
        return;
    }
    if (ws_bad >= 8) return;
    ws_bad++;
    printf("net: estado distinto en el cuadro %u (ultimo igual %u), rand %u/%u\n", f, ws_good, a.rng, b.rng);
    if (a.rng != b.rng) ws_rand_show(ws_good, f + 1);
    static const char* nm[6] = {"vida", "ki", "accion", "x", "y", "z"};
    for (int k = 0; k < 4; k++) {
        if (!memcmp(a.v[k], b.v[k], sizeof(a.v[k]))) continue;
        char line[256]; int n = snprintf(line, sizeof(line), "net:   J%d (entidad %u/%u)", k + 1, a.ent[k], b.ent[k]);
        for (int i = 0; i < 6; i++) {
            if (a.v[k][i] == b.v[k][i]) continue;
            if (i < 3) n += snprintf(line + n, sizeof(line) - n, " %s %u/%u", nm[i], a.v[k][i], b.v[k][i]);
            else { float x, y; memcpy(&x, &a.v[k][i], 4); memcpy(&y, &b.v[k][i], 4); n += snprintf(line + n, sizeof(line) - n, " %s %.3f/%.3f", nm[i], x, y); }
        }
        printf("%s\n", line);
    }
}
void psp_net_state_rx(const uint8_t* d, int n) {
    if (n < 108) return;
    auto g = [&](int o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
    std::lock_guard<std::mutex> l(ws_mtx);
    WsState& b = ws_theirs[g(0) & 511];
    b.frame = g(0); b.rng = g(4);
    for (int k = 0; k < 4; k++) for (int i = 0; i < 6; i++) b.v[k][i] = g(8 + 4 * (6 * k + i));
    memcpy(b.ent, d + 104, 4);
    b.set = true;
    ws_compare(b.frame);
}
static bool ws_gp(uint32_t a) { return a >= 0x08800000u && a < 0x10000000u; }
static uint32_t ws_scene(uint32_t o) { uint32_t m = psp_l32(0x08A7EA50u); return ws_gp(m) ? psp_l32(m + o) : 0xFFFFFFFFu; }
// entity of combatant k (0x08A7DBC8 -> {count, base}, +0x2C entity slot); TTT_NET_STATE_RAW=1 compares by table slot
static uint32_t ws_fighter(int k, uint8_t* slot) {
    static int raw = -1; if (raw < 0) { const char* e = getenv("TTT_NET_STATE_RAW"); raw = e ? atoi(e) : 0; }
    *slot = 0xFF;
    if (raw) { *slot = (uint8_t)k; return psp_l32(0x08B61178u + 4 * k); }
    uint32_t t = psp_l32(0x08A7DBC8u);
    if (!ws_gp(t)) return 0;
    uint32_t n = psp_l32(t), b = psp_l32(t + 4);
    if (n < 1 || n > 4 || (uint32_t)k >= n || !ws_gp(b)) return 0;
    uint32_t s = psp_l32(b + 0x1A90u * k + 0x2C);
    if (s > 3) return 0;
    *slot = (uint8_t)s;
    return psp_l32(0x08B61178u + 4 * s);
}
static void ws_state(uint32_t f) {
    static uint32_t last = 0;
    static bool shown = false;
    if (f < last) { std::lock_guard<std::mutex> l(ws_mtx); for (auto& x : ws_mine) x.set = false; for (auto& x : ws_theirs) x.set = false; ws_good = 0; ws_bad = 0; ws_perm = 0; ws_rr_pos = 0; ws_rr_shown = false; }
    if (!last || f < last) { rs_flush("nueva escena"); ws_scenes++; printf("net: combate en lockstep desde el cuadro %u, logica a %s fps, escena %u pagina %u, entidades 0x%08X 0x%08X\n", f, battle60() ? "60" : "30", ws_scene(0x10), ws_scene(0x14), psp_l32(0x08B61178u), psp_l32(0x08B61180u)); shown = false; }
    last = f; ws_cur = f; ws_cur_vb = psp_l32(0x08A78E14u);
    WsState a = {};
    a.frame = f; a.rng = psp_l32(0x08A7E8B4u); a.set = true;
    for (int k = 0; k < 4; k++) {
        uint32_t e = ws_fighter(k, &a.ent[k]);
        bool ok = ws_gp(e);
        static const uint32_t off[6] = {0x1A34, 0x1A40, 0x984, 0x950, 0x954, 0x958};
        uint32_t t = psp_l32(0x08A7DBC8u), fb = ws_gp(t) && (uint32_t)k < psp_l32(t) && psp_l32(t) <= 8 ? psp_l32(t + 4) : 0, fi = ws_gp(fb) ? fb + 0x1A90u * k : 0;
        for (int i = 0; i < 6; i++) a.v[k][i] = i < 2 ? (fi ? psp_l32(fi + off[i]) : 0) : ok ? psp_l32(e + off[i]) : 0;
    }
    if (!shown && a.ent[0] != 0xFF) { shown = true; printf("net: luchadores del cuadro %u por combatiente: entidades %u %u %u %u\n", f, a.ent[0], a.ent[1], a.ent[2], a.ent[3]); }
    uint8_t d[108];
    ws_put(d, f); ws_put(d + 4, a.rng);
    for (int k = 0; k < 4; k++) for (int i = 0; i < 6; i++) ws_put(d + 8 + 4 * (6 * k + i), a.v[k][i]);
    memcpy(d + 104, a.ent, 4);
    psp_net_extra(WS_P_STATE, d, 108);
    if (ws_scene(0x14) == 2 && a.ent[0] != 0xFF) { uint32_t lf[4]; for (int k = 0; k < 4; k++) lf[k] = a.v[k][0]; rs_life(lf, a.ent, ld_me_now(), f); }
    std::lock_guard<std::mutex> l(ws_mtx);
    ws_mine[f & 511] = a;
    ws_compare(f);
}

// battle result setter 0x088566C4(word, flags): logged only, the game does not call it in WLAN battles (EBOOT_TTT 62.1)
bool net_online_ranked();
int net_online_rank_mode();
int net_online_rank_free(int res, int team, int np);
int net_online_rank_surv(int waves, int np);
static void rs_result(Ctx& c) {
    static int logs = 0;
    if (logs < 30) { logs++; printf("combate: resultado palabra %u banderas 0x%X desde 0x%08X%s\n", c.r[4], c.r[5], c.r[31] - 8, ws_wlan() ? " (red)" : ""); }
}
// result by life: a team falls when none of its fighters has life. Free battle: 60 stable frames or the scene's last frame;
// survival (team A humans): every fall of team B is a wave, the run ends when team A falls
// fighters by combatant table [0x08A7DBC8] = {count, base}, stride 0x1A90: +4 team, +0x1A34 life (the fighter, not the entity)
struct RsLife { int n = 0; int32_t life[8]; uint8_t team[8]; };
static RsLife rs_read() {
    RsLife r;
    uint32_t t = psp_l32(0x08A7DBC8u);
    if (!ws_gp(t)) return r;
    uint32_t n = psp_l32(t), b = psp_l32(t + 4);
    if (n < 1 || n > 8 || !ws_gp(b)) return r;
    for (uint32_t k = 0; k < n; k++) { r.life[k] = (int32_t)psp_l32(b + 0x1A90u * k + 0x1A34); r.team[k] = (uint8_t)(psp_l32(b + 0x1A90u * k + 4) & 1); }
    r.n = (int)n;
    return r;
}
// TTT_LIFE_LOG=1: every fighter's life fields, team block and battle record bits whenever they change, in any battle
static void rs_diag() {
    static int on = -1, lines = 0;
    if (on < 0) { const char* e = getenv("TTT_LIFE_LOG"); on = e ? atoi(e) : 0; }
    if (!on || lines >= 2500) return;
    static uint32_t prev[8][10], pres = 0xFFFFFFFFu, pscene = 0xFFFFFFFFu;
    uint32_t vb = psp_l32(0x08A78E14u), res = psp_l32(0x08B49028u), sc = ws_scene(0x10) << 8 | (ws_scene(0x14) & 0xFF);
    if (res != pres || sc != pscene) { lines++; printf("vida: vblank %u, escena %u pagina %u, resultado %u/0x%X, modo de batalla %u\n", vb, sc >> 8, sc & 0xFF, res, psp_l32(0x08B4902Cu), psp_l32(0x08B48A28u)); pres = res; pscene = sc; }
    uint32_t t = psp_l32(0x08A7DBC8u);
    if (!ws_gp(t)) return;
    uint32_t n = psp_l32(t), b = psp_l32(t + 4);
    if (n < 1 || n > 8 || !ws_gp(b)) return;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t f = b + 0x1A90u * k, blk = psp_l32(f + 0x9D0), slot = psp_l32(f + 0x2C) & 3, rec = 0x08B48A20u + 0x698 + 0x30 * slot + 8;
        uint32_t v[10] = {psp_l32(f + 0x1A34), psp_l32(f + 0x1A38), psp_l32(f + 0x1A3C), psp_l32(f + 0xC), ws_gp(blk) ? psp_l32(blk + 4) : 0, ws_gp(blk) ? psp_l32(blk + 8) : 0,
                          psp_l32(rec), psp_l32(rec + 4), psp_l32(rec + 8), psp_l32(rec + 12)};
        if (!memcmp(v, prev[k], sizeof(v))) continue;
        memcpy(prev[k], v, sizeof(v));
        if (lines++ >= 2500) return;
        printf("vida: vblank %u luchador %u (equipo %u miembro %u ranura %u cpu %u): vida %d max %d +3C %d activo %u, bloque %d %d, registro %08X %08X %08X %08X\n", vb, k, psp_l32(f + 4), psp_l32(f + 8),
               slot, psp_l8(f + 0x1690), (int32_t)v[0], (int32_t)v[1], (int32_t)v[2], v[3], (int32_t)v[4], (int32_t)v[5], v[7], v[6], v[9], v[8]);
    }
}
static struct { uint32_t scene = 0xFFFFFFFFu, stable = 0, frame = 0; RsLife v; int32_t me = -1; int last = -1, bdown = 0; bool set = false, down = false, both = false; } rs;
static int rs_waves = 0, rs_bfalls = 0;
static bool rs_run = false;
static int32_t rs_slot = -1;
// the local player's team and the humans of the combat from the fighters: 0x0885862C() is the local fighter slot (+0x2C), absent partners have max life 0
static int rs_myteam(int* hum) {
    int team = -1;
    hum[0] = hum[1] = 0;
    uint32_t t = psp_l32(0x08A7DBC8u);
    if (!ws_gp(t)) return -1;
    uint32_t n = psp_l32(t), b = psp_l32(t + 4);
    if (n < 1 || n > 8 || !ws_gp(b)) return -1;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t f = b + 0x1A90u * k, tm = psp_l32(f + 4) & 1;
        if ((int32_t)psp_l32(f + 0x1A38) <= 0) continue;
        if (!psp_l8(f + 0x1690)) hum[tm]++;
        if ((int32_t)psp_l32(f + 0x2C) == rs_slot && team < 0) team = (int)tm;
    }
    return team;
}
// battle config 0x08B48A20 +0x464: room player index of team t member m (int[2][2], fighter slot 2t+m, 0x08858574)
static int rs_team(int32_t me, int* hum) {
    int team = -1;
    hum[0] = hum[1] = 0;
    for (int t = 0; t < 2; t++) for (int m = 0; m < 2; m++) {
        int32_t p = (int32_t)psp_l32(0x08B48E84u + 8 * t + 4 * m);
        if (p < 0 || p > 3) continue;
        hum[t]++;
        if (p == me) team = t;
    }
    return team;
}
// -1 nobody fell, 0 team A fell, 1 team B fell
static int rs_lost(const RsLife& v) {
    bool alive[2] = {};
    int cnt[2] = {};
    for (int k = 0; k < v.n; k++) { cnt[v.team[k]]++; if (v.life[k] > 0) alive[v.team[k]] = true; }
    return cnt[0] && cnt[1] && alive[0] != alive[1] ? (alive[0] ? 1 : 0) : -1;
}
static const char* rs_lives(const RsLife& v) {
    static char b[128];
    int o = 0;
    b[0] = 0;
    for (int k = 0; k < v.n && o < 110; k++) o += snprintf(b + o, sizeof(b) - o, "%s%c %d", k ? ", " : "", v.team[k] ? 'B' : 'A', v.life[k]);
    return b;
}
static void rs_send(int lost, const char* how) {
    rs_done = rs.scene;
    rs.both = false;
    int hum[2], team = rs_myteam(hum), mode = net_online_rank_mode();
    printf("net: resultado por vida (%s): cuadro %u, vidas %s, cae el equipo %c, yo jugador %d equipo %c, humanos %d+%d, oleadas %d, clasificatoria %d\n", how, rs.frame, rs_lives(rs.v),
           lost ? 'B' : 'A', rs.me + 1, team < 0 ? '-' : team ? 'B' : 'A', hum[0], hum[1], rs_waves, mode);
    int n = 0;
    if (mode == 1 && lost == 0) { n = net_online_rank_surv(rs_waves, hum[0] + hum[1]); printf("net: clasificatoria: supervivencia %d enviada, %d oleadas\n", n, rs_waves); rs_run = false; }
    else if (mode == 0 && team >= 0) { int res = team == lost ? 2 : 1; n = net_online_rank_free(res, team, hum[0] + hum[1]); printf("net: clasificatoria: combate %d %s enviado\n", n, res == 1 ? "ganado" : "perdido"); }
    if (lost == 0) rs_waves = 0;
}
// a fall counts only after both teams were seen alive in this scene (scenes between waves start with the old fighters at 0)
static void rs_wave(int lost, uint32_t f) {
    if (lost == 1 && !rs.bdown && rs.both) {
        rs.bdown = 1; rs_waves++;
        if (rs_bfalls++ < 200) printf("net: equipo B sin vida en el cuadro %u, escena %u (caida %d)\n", f, ws_scenes, rs_waves);
    } else if (lost < 0) rs.bdown = 0;
}
static void rs_life(const uint32_t*, const uint8_t*, int32_t me, uint32_t f) {
    if (rs.scene != ws_scenes) {
        rs.scene = ws_scenes; rs.stable = 0; rs.last = -1; rs.down = false;
        int hum[2], team = rs_team(me, hum), h2[2], t2 = rs_myteam(h2);
        printf("net: equipos del combate: jugadores A %d %d, B %d %d; yo jugador %d equipo %c (tabla) %c (luchador local %d), humanos %d+%d; modo de batalla %u; clasificatoria %d\n", (int32_t)psp_l32(0x08B48E84u), (int32_t)psp_l32(0x08B48E88u),
               (int32_t)psp_l32(0x08B48E8Cu), (int32_t)psp_l32(0x08B48E90u), me + 1, team < 0 ? '-' : team ? 'B' : 'A', t2 < 0 ? '-' : t2 ? 'B' : 'A', rs_slot, h2[0], h2[1], psp_l32(0x08B48A28u), net_online_rank_mode());
    }
    rs.v = rs_read();
    if (!rs.v.n) return;
    rs.set = true; rs.frame = f; rs.me = me;
    if (f % 300 == 0) printf("net: vidas en el cuadro %u: %s\n", f, rs_lives(rs.v));
    if (rs_done == ws_scenes || !ws_wlan()) return;
    if (net_online_rank_mode() == 1) mpm_surv_end = true;
    if (net_online_rank_mode() == 1 && !rs_run) { rs_run = true; printf("net: supervivencia clasificatoria en curso\n"); }
    static int rtest = -1;
    if (rtest < 0) { const char* e = getenv("TTT_RANK_TEST"); rtest = e ? atoi(e) : 0; }
    if (rtest && f == 30 && net_online_rank_mode() == 1) {
        uint32_t t = psp_l32(0x08A7DBC8u), b = psp_l32(t + 4);
        int done = 0;
        for (int k = 0; k < rs.v.n; k++) {
            uint32_t fl = b + 0x1A90u * k;
            if (rs.v.team[k] == 1 && (int32_t)psp_l32(fl + 0x1A34) > 1) { psp_s32(fl + 0x1A34, 1); done++; }
        }
        if (done) printf("net: prueba: %d luchadores CPU a 1 de vida en el cuadro 30\n", done);
    }
    int lost = rs_lost(rs.v);
    if (lost < 0) {
        bool alive[2] = {};
        for (int k = 0; k < rs.v.n; k++) if (rs.v.life[k] > 0) alive[rs.v.team[k]] = true;
        if (alive[0] && alive[1]) rs.both = true;
    }
    if (lost == 0 && !rs.down && rs.both) { rs.down = true; printf("net: equipo A sin vida en el cuadro %u\n", f); }
    rs_wave(lost, f);
    if (lost != rs.last) { rs.last = lost; rs.stable = 0; }
    if (lost < 0 || !rs.both || ++rs.stable < 60) return;
    if (net_online_rank_mode() == 1) return;
    rs_send(lost, "60 cuadros");
}
// survival also ends by time or by quitting: the run is reported when its battle pages are left (oleadas superadas so far)
void rs_run_end(const char* why) {
    if (!rs_run) return;
    rs_run = false;
    int hum[2];
    rs_myteam(hum);
    int np = hum[0] + hum[1] < 2 ? 2 : hum[0] + hum[1];
    int n = net_online_rank_surv(rs_waves, np);
    printf("net: clasificatoria: supervivencia %d enviada al terminar (%s), %d oleadas\n", n, why, rs_waves);
    rs_waves = 0; rs.both = false; rs.bdown = 0;
}
static void rs_flush(const char* why) {
    if (!rs.set) return;
    rs.set = false;
    int lost = rs_lost(rs.v);
    printf("net: fin del combate (%s): cuadro %u, vidas %s, oleadas %d%s\n", why, rs.frame, rs_lives(rs.v), rs_waves,
           rs_done == rs.scene ? ", ya enviado" : !rs.both ? ", sin combate" : lost < 0 ? ", sin equipo caido" : "");
    if (rs_done == rs.scene || lost < 0 || !rs.both || !ws_wlan()) return;
    if (net_online_rank_mode() == 1) { rs_wave(lost, rs.frame); return; }
    rs_send(lost, "final de escena");
}

int input_start_held();
// leaving a network battle: hold Start 5 s; the leaver tells the others (P_QUIT with its team) and every machine ends
// the session like the game does (0x08811568, phase 17). A ranked free battle left this way is lost by the leaver's team
static void rs_quit(int team, bool mine) {
    int hum[2], my = rs_myteam(hum);
    printf("net: abandono %s, equipo %c\n", mine ? "propio" : "del otro jugador", team ? 'B' : 'A');
    if (net_online_rank_mode() != 0 || !rs.both || rs_done == rs.scene || my < 0) return;
    rs_done = rs.scene; rs.both = false;
    int res = my == team ? 2 : 1;
    int n = net_online_rank_free(res, my, hum[0] + hum[1]);
    printf("net: clasificatoria: combate %d %s enviado (abandono)\n", n, res == 1 ? "ganado" : "perdido");
}
// leave like a lost peer: session failure 0x08817390(net object [0x08A78A28]) -> phase 18, the path the game already
// takes in battle (0x08811568 from inside the battle left the other machine stuck)
static void ws_leave(Ctx& c, const char* why) {
    uint32_t net = psp_l32(0x08A78A28u);
    printf("net: se sale del combate en red (%s), red 0x%08X\n", why, net);
    mpm_after_surv = 1;
    if (ws_gp(net)) hud_guest(c, 0x08817390u, net);
}
static void ws_quit_tick(Ctx& c) {
    static int team = 0, shown = 0;
    static bool done = false, held = false;
    static std::chrono::steady_clock::time_point t0;
    if (!ws_wlan() || ws_scene(0x14) != 2) { held = false; done = false; ws_quit_team = -1; return; }
    int qt = ws_quit_team.exchange(-1);
    if (qt >= 0 && !done) { done = true; rs_quit(qt, false); ws_leave(c, "el otro jugador abandono"); return; }
    bool st = input_start_held() != 0;
    if (st && !held) { t0 = std::chrono::steady_clock::now(); if (shown++ < 20) printf("net: Start mantenido, abandono en 5 s\n"); }
    held = st;
    if (held && !done && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(5)) {
        int hum[2];
        done = true; team = rs_myteam(hum); if (team < 0) team = 0;
        rs_quit(team, true);
        uint8_t d[4] = {(uint8_t)team, 0, 0, 0};
        for (int i = 0; i < 5; i++) psp_net_extra(WS_P_QUIT, d, 4);
        ws_leave(c, "propio");
    }
}

// lockstep input delay and input stream. Frame f applies the inputs recorded at f-D (the game uses D = 1 and waits every
// frame for the remote ones, one frame per half round trip); the host picks D per scene at its handshake (phase 12) and
// sends it (P_DELAY). Remote inputs travel as a port stream (P_STREAM, GGPO style): every vblank each machine sends,
// unreliably, all its inputs of the scene not yet acknowledged by the peers plus its own acks, so a lost packet is covered
// by the next one and nothing waits for a resend. Entries are tagged with the scene (epoch): a late packet of another
// scene is never used. The game's own input packets go unreliable and their entries are ignored
uint32_t psp_net_delay(uint32_t* epoch);
void psp_net_dump();
void psp_net_delay_reset();
int psp_net_rtt_ms();
void psp_net_rtt_parts(int* rtt, int* var);
uint32_t psp_reserve(uint32_t n);
void psp_net_set_stream(bool on);
static const int WS_P_DELAY = 14, WS_P_STREAM = 18, LD_RING = 64, LD_WIN = 32;
static const uint32_t LD_LOCAL = 1;
static uint32_t ld_buf = 0, ld_d = 1, ld_epoch = 0, ld_used = 0, ld_sent = 0, ld_cur = 0, ld_frames = 0, ld_waits = 0, ld_rtt = 0;
static uint32_t ld_var = 0, ld_sstep = 0, ld_pw[4], ld_pf[4];
static int ld_extra[2] = {0, 0};
static std::chrono::steady_clock::time_point ld_wall;
static uint32_t ld_next_d = 0, ld_next_epoch = 0, ld_waits0 = 0, ld_svb = 0xFFFFFFFFu, ld_tag[4][LD_RING];
static int32_t ld_rx[4], ld_ack[4], ld_own = -1, ld_me = -1;
static bool ld_on = false, ld_host = false, ld_seen[4];
static bool pl_net() { return ld_on; }
// 52b: a blast node lives its blast_params time after its hits and the attacker stays in the launch loop until the node's
// destructor calls 0x08894ACC. Hits are counted on the victim side (0x0892424C, hit info +0x70 node, +0x74 attack); when
// every victim got the hit count (blast_params +0x0A, plus 1) the attacker is released at once and the destructor's later
// release is skipped. TTT_BLAST_END=0 off. Attack classes: descriptor table 0x08A7C044.
struct BeHit { uint32_t node, desc, fighter, need, cnt[4], vb; int state; };
static BeHit be_t[8];
static int be_lines = 0;
static bool be_on() {
    static int n = 0, on = 1;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_BLAST_END"); on = e && *e ? atoi(e) : 1; }
    return on != 0;
}
static int be_kind(uint32_t d) {
    static const uint32_t D[11] = {0x08A7C0E0, 0x08A7C5A8, 0x08A7C110, 0x08A7C5D8, 0x08A7C488, 0x08A7C6D0, 0x08A7C4E8, 0x08A7C578, 0x08A7C4B8, 0x08A7C670, 0x08A7C6A0};
    for (int i = 0; i < 11; i++) if (D[i] == d) return i - 1;
    return -2;
}
static bool be_alive(const BeHit& h) { return nt_ram(h.node) && psp_l32(h.node + 0x28) == h.desc && !(psp_l8(h.node) & 1); }
static void be_hit(uint32_t victim, uint32_t info) {
    if (!be_on() || !nt_ram(info)) return;
    uint32_t node = psp_l32(info + 0x70), inst = psp_l32(info + 0x74), desc = nt_ram(node) ? psp_l32(node + 0x28) : 0;
    uint32_t spec = nt_ram(inst) ? psp_l32(inst + 0x28) : 0, t = psp_l32(0x08A7DBC8u), b = nt_ram(t) ? psp_l32(t + 4) : 0;
    if (be_kind(desc) < -1 || !nt_ram(spec) || !psp_l8(spec + 4) || !b || victim < b) return;
    uint32_t v = (victim - b) / 0x1A90u, vb = psp_l32(0x08A78E14u);
    if (v >= 4) return;
    BeHit* h = nullptr;
    for (auto& x : be_t) if (x.state >= 0 && x.node == node && x.desc == desc && be_alive(x)) h = &x;
    if (!h) for (auto& x : be_t) if (x.state < 0 || !be_alive(x) || vb - x.vb > 1200u) { h = &x; break; }
    if (!h) return;
    if (h->node != node || h->desc != desc || h->state < 0) { memset(h, 0, sizeof(*h)); h->node = node; h->desc = desc; h->fighter = psp_l32(inst); h->need = psp_l8(spec + 0xA) + 1u; }
    h->vb = vb;
    if (h->state != 0) return;
    h->cnt[v]++;
    bool full = false, mid = false;
    for (int k = 0; k < 4; k++) { if (h->cnt[k] >= h->need) full = true; else if (h->cnt[k]) mid = true; }
    if (full && !mid) h->state = 1;
}
static void be_tick(const Ctx& c) {
    for (auto& h : be_t) {
        if (h.state != 1) continue;
        if (!be_alive(h)) { h.state = -1; continue; }
        h.state = 2;
        if (be_lines++ < 60) printf("blast: control devuelto al luchador %u tras %u golpes (clase %d, nodo 0x%08X)\n", h.fighter, h.need, be_kind(h.desc), h.node);
        hud_guest(c, 0x08894ACCu, h.fighter);
    }
}
static bool be_skip(uint32_t fighter) {
    for (auto& h : be_t) if (h.state == 2 && h.fighter == fighter && nt_ram(h.node) && psp_l32(h.node + 0x28) == h.desc && (psp_l8(h.node) & 1)) { h.state = -1; return true; }
    return false;
}
// debug (temporary): Page Up leaves one opponent (team 1) at 1 life, Page Down leaves J1 at 1 life, offline only
std::atomic<int> psp_dbg_ko{0};
static void dbg_ko_tick() {
    int m = psp_dbg_ko.exchange(0);
    if (!m) return;
    if (ld_on) { printf("debug: vida a un golpe no disponible en red\n"); return; }
    uint32_t t = psp_l32(0x08A7DBC8u);
    if (!guest_ptr(t)) return;
    uint32_t n = psp_l32(t), b = psp_l32(t + 4);
    if (n < 1 || n > 8 || !guest_ptr(b)) return;
    int done = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t f = b + 0x1A90u * k;
        bool me = psp_l32(f + 4) == 0 && psp_l32(f + 8) == 0;
        if (!psp_l32(f + 0xC) || (m == 1 ? psp_l32(f + 4) != 1 : !me) || (int32_t)psp_l32(f + 0x1A34) <= 1) continue;
        psp_s32(f + 0x1A34, 1);
        done++;
        break;
    }
    printf(m == 1 ? "debug: %d oponentes con vida 1\n" : "debug: J1 con vida 1 (%d)\n", done);
}
// debug (temporary): End freezes every opponent (team 1): it reads an empty pad instead of the AI, offline only
std::atomic<int> psp_dbg_freeze{0};
static void dbg_freeze(uint32_t f) {
    static uint32_t keep[8][3]; static bool held[8];
    if (!guest_ptr(f)) return;
    uint32_t t = psp_l32(0x08A7DBC8u), b = t ? psp_l32(t + 4) : 0, i = b && f >= b ? (f - b) / 0x1A90u : 99;
    if (i >= 8 || psp_l32(f + 4) != 1) return;
    bool on = psp_dbg_freeze.load() && !ld_on;
    if (on && !held[i]) { held[i] = true; keep[i][0] = f; keep[i][1] = psp_l32(f + 0x18); keep[i][2] = psp_l8(f + 0x1690); printf("debug: oponente %u quieto (End)\n", i); }
    if (!on && held[i]) { held[i] = false; if (keep[i][0] == f) { psp_s32(f + 0x18, keep[i][1]); psp_s8(f + 0x1690, (uint8_t)keep[i][2]); } printf("debug: oponente %u con su control de nuevo\n", i); }
    if (on) { psp_s32(f + 0x18, 3); psp_s8(f + 0x1690, 0); }
}
static double ld_wait_ms = 0, ld_wait0_ms = 0, ld_t = 0;
static long ld_got = 0, ld_late = 0;
static std::mutex ld_qm;
static std::vector<std::vector<uint8_t>> ld_q;
void psp_net_stream_rx(const uint8_t* d, int n) { std::lock_guard<std::mutex> l(ld_qm); if (ld_q.size() < 512) ld_q.emplace_back(d, d + n); }
static uint32_t ld_slot(uint32_t k, uint32_t f) { return ld_buf + ((k & 3) * LD_RING + (f & (LD_RING - 1))) * 16; }
static uint32_t ld_cur_epoch() { return ld_on ? (ld_host ? ld_epoch : ld_used) : ld_next_d ? ld_next_epoch : 0; }
static bool ld_adapt() { static int a = -1; if (a < 0) { const char* e = getenv("TTT_NET_DELAY_ADAPT"); a = e ? atoi(e) : 1; } return a != 0; }
static int32_t ld_me_now() { return ld_me; }
static void ld_end(bool wipe = true) {
    rs_flush("fin del lockstep");
    if (ld_on && ld_frames) {
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - ld_wall).count();
        printf("net: escena en lockstep terminada: %u cuadros, retardo %u, esperas %u (%.0f ms), de ellas al inicio %u (%.0f ms), entradas recibidas %ld, tardias %ld, %.1f s reales (%.1f cuadros/s)\n", ld_frames, ld_d, ld_waits, ld_wait_ms, ld_waits0, ld_wait0_ms, ld_got, ld_late, sec, sec > 0 ? ld_frames / sec : 0.0);
        double worst = (ld_waits - ld_waits0) / (double)ld_frames;
        for (int s = 0; s < 4; s++) if (ld_seen[s] && ld_pf[s]) {
            printf("net: esperas del jugador %d sin contar el inicio: %u de %u cuadros\n", s + 1, ld_pw[s], ld_pf[s]);
            if (ld_pf[s] >= 60 && ld_pw[s] / (double)ld_pf[s] > worst) worst = ld_pw[s] / (double)ld_pf[s];
        }
        if (ld_host && ld_adapt() && ld_frames >= 60 && patch_net_delay.val < 0) {
            int& x = ld_extra[ld_sstep];
            int was = x;
            if (worst > 0.02 && x < 3) x++; else if (worst < 0.003 && x > 0) x--;
            printf("net: peor tasa de esperas %.1f %%, ajuste del retardo para la logica a %u fps: +%d (antes +%d)\n", worst * 100, ld_sstep ? 30 : 60, x, was);
        }
    }
    ld_on = false; ld_cur = 0; ld_frames = 0; ld_waits = 0; ld_wait_ms = 0; ld_waits0 = 0; ld_wait0_ms = 0; ld_got = 0; ld_late = 0;
    if (!wipe && ld_buf) return;
    if (!ld_buf) { ld_buf = psp_reserve((4 * LD_RING + 4) * 16); psp_net_set_stream(patch_net_delay.val != 0); }
    for (int i = 0; i < 4 * LD_RING; i++) psp_s32(ld_buf + 16 * i, 0xFFFFFFFFu);
    memset(ld_tag, 0, sizeof(ld_tag));
    ld_own = -1;
}
static uint32_t ld_step() { uint32_t step = psp_l32(0x08A78E0Cu); return step < 1 || step > 4 ? 1 : step; }
static void ld_pick() {
    int m = patch_net_delay.val;
    static int kr = 0, kv = 0;
    int r = 0, v = 0;
    psp_net_rtt_parts(&r, &v);
    if (r) { kr = r; kv = v; } else { r = kr; v = kv; }
    ld_rtt = (uint32_t)r; ld_var = (uint32_t)v;
    double fm = 1000.0 / 60.0 * ld_step();
    int d = m > 0 ? m : (int)ceil((r + 4 * v + 10) / (2 * fm)) + (r > 60) + (ld_adapt() ? ld_extra[ld_step() > 1] : 0);
    ld_next_d = d < 1 ? 1 : d > 12 ? 12 : d;
    uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() | 1;
    ld_next_epoch = ms - ld_epoch > 1 ? ms : ld_epoch + 2;
    ld_epoch = ld_next_epoch;
    ld_sent = 0;
}
// incoming stream packets (queued by the network thread): epoch, sender slot, count, first frame, acks[4], entries
static void ld_drain() {
    std::vector<std::vector<uint8_t>> q;
    { std::lock_guard<std::mutex> l(ld_qm); q.swap(ld_q); }
    if (!ld_buf) return;
    uint32_t E = ld_cur_epoch();
    auto g = [](const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; };
    for (auto& v : q) {
        if (v.size() < 26) continue;
        const uint8_t* p = v.data();
        uint32_t ep = g(p), s = p[4], cnt = p[5];
        int32_t first = (int32_t)g(p + 6);
        if (s > 3 || (int32_t)s == ld_me || v.size() < 26 + 10 * cnt || !ep) continue;
        if (E && ep != E) { ld_late++; continue; }
        for (uint32_t i = 0; i < cnt; i++) {
            int32_t f = first + (int32_t)i;
            if (f < 0) continue;
            uint32_t a = ld_slot(s, (uint32_t)f), &tg = ld_tag[s][f & (LD_RING - 1)];
            int32_t had = (int32_t)psp_l32(a);
            if (tg == ep && had >= f) continue;
            const uint8_t* e = p + 26 + 10 * i;
            psp_s32(a, (uint32_t)f); psp_s32(a + 4, g(e)); psp_s32(a + 8, g(e + 4)); psp_s32(a + 12, (uint32_t)e[8] | (uint32_t)e[9] << 8);
            tg = ep; ld_got++;
        }
        if (E && v.size() >= 30 + 10 * cnt) { const uint8_t* t = p + 26 + 10 * cnt; ld_pw[s] = t[0] | t[1] << 8; ld_pf[s] = t[2] | t[3] << 8; }
        if (E && ld_me >= 0) { ld_seen[s] = true; int32_t a = (int32_t)g(p + 10 + 4 * ld_me); if (a > ld_ack[s]) ld_ack[s] = a; }
    }
}
// outgoing: once per vblank, own entries from the oldest frame a peer lacks (at most 32: older ones are never needed,
// a peer is at most 2D frames behind) and the contiguous frame received from every slot
static void ld_stream() {
    uint32_t vb = psp_l32(0x08A78E14u);
    if (!ld_on || ld_me < 0 || ld_own < 0 || vb == ld_svb) return;
    ld_svb = vb;
    uint32_t E = ld_cur_epoch();
    int32_t from = ld_own - (LD_WIN - 1);
    if (from < 0) from = 0;
    int32_t need = ld_own + 1;
    for (int s = 0; s < 4; s++) if (ld_seen[s] && ld_ack[s] + 1 < need) need = ld_ack[s] + 1;
    if (need > from) from = need;
    uint8_t d[26 + 10 * LD_WIN + 4];
    ws_put(d, E); d[4] = (uint8_t)ld_me; ws_put(d + 6, (uint32_t)from);
    for (int s = 0; s < 4; s++) {
        while (s != ld_me && psp_l32(ld_slot(s, ld_rx[s] + 1)) == (uint32_t)(ld_rx[s] + 1) && ld_tag[s][(ld_rx[s] + 1) & (LD_RING - 1)] == E) ld_rx[s]++;
        ws_put(d + 10 + 4 * s, (uint32_t)(s == ld_me ? -1 : ld_rx[s]));
    }
    int n = 0;
    for (int32_t f = from; f <= ld_own && n < LD_WIN; f++, n++) {
        uint32_t a = ld_slot(ld_me, f);
        if (psp_l32(a) != (uint32_t)f || ld_tag[ld_me][f & (LD_RING - 1)] != LD_LOCAL) break;
        uint8_t* e = d + 26 + 10 * n;
        ws_put(e, psp_l32(a + 4)); ws_put(e + 4, psp_l32(a + 8)); e[8] = psp_l8(a + 12); e[9] = psp_l8(a + 13);
    }
    d[5] = (uint8_t)n;
    uint32_t w = ld_waits - ld_waits0, fr = ld_frames;
    uint8_t* t = d + 26 + 10 * n;
    t[0] = (uint8_t)(w > 0xFFFF ? 0xFF : w); t[1] = (uint8_t)((w > 0xFFFF ? 0xFFFF : w) >> 8); t[2] = (uint8_t)(fr > 0xFFFF ? 0xFF : fr); t[3] = (uint8_t)((fr > 0xFFFF ? 0xFFFF : fr) >> 8);
    psp_net_extra(WS_P_STREAM, d, 30 + 10 * n);
}
static void ld_tick() {
    if (!patch_net_delay.val) return;
    ld_drain();
    ld_stream();
    if (!ld_next_d && !(ld_on && ld_host)) return;
    uint32_t vb = psp_l32(0x08A78E14u);
    if (vb - ld_sent < 3) return;
    uint8_t d[8]; ws_put(d, ld_next_d ? ld_next_epoch : ld_epoch); ws_put(d + 4, ld_next_d ? ld_next_d : ld_d);
    psp_net_extra(WS_P_DELAY, d, 8);
    ld_sent = vb;
}
static bool ld_begin(bool host) {
    if (host) {
        if (!ld_next_d) ld_pick();
        ld_d = ld_next_d; ld_epoch = ld_next_epoch; ld_next_d = 0;
    } else {
        uint32_t ep = 0, d = psp_net_delay(&ep);
        if (!ep || ep == ld_used || !d || d > 12) return false;
        int r = 0, v = 0; psp_net_rtt_parts(&r, &v);
        ld_d = d; ld_used = ep; ld_rtt = (uint32_t)r; ld_var = (uint32_t)v;
    }
    ld_on = true; ld_host = host; ld_sstep = ld_step() > 1; ld_wall = std::chrono::steady_clock::now();
    for (int s = 0; s < 4; s++) { ld_rx[s] = -1; ld_ack[s] = -1; ld_seen[s] = false; ld_pw[s] = 0; ld_pf[s] = 0; }
    printf("net: retardo de entrada %u cuadros (%s, rtt %u ms, variacion %u ms, logica a %u fps)\n", ld_d, host ? "anfitrion" : "cliente", ld_rtt, ld_var, 60 / ld_step());
    return true;
}
static bool ld_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    if (!patch_net_delay.val) return false;
    if (dir == 0x08816E24u) { orig(c); ld_end(); return true; }
    if (!ld_buf) ld_end();
    if (ws_quit_team.load() >= 0) ws_quit_tick(c);
    uint32_t ra = c.r[31];
    if (dir == 0x088174FCu) {
        // own entry (client 0x08814A2C, host 0x088158AC); entries parsed from the game's packets are ignored
        if (ra != 0x08814A34u && ra != 0x088158B4u) return true;
        uint32_t k = c.r[5] & 3, e = c.r[6], f = psp_l32(e), s = ld_slot(k, f);
        if ((int32_t)f < 0) return true;
        memcpy(psp_p(s), psp_p(e), 16);
        ld_tag[k][f & (LD_RING - 1)] = LD_LOCAL;
        ld_me = (int32_t)k;
        if ((int32_t)f > ld_own) ld_own = (int32_t)f;
        ld_svb = 0xFFFFFFFFu;
        ld_stream();
        return true;
    }
    uint32_t k = c.r[5] & 3, f = c.r[6];
    int site = 0;
    for (uint32_t a = 0x08814824u; a <= 0x08814884u; a += 0x20) if (ra == a) site = 1;
    for (uint32_t a = 0x0881571Cu; a <= 0x0881577Cu; a += 0x20) if (ra == a) site = 2;
    if (site) {
        uint32_t cur = f + 1;
        if (cur < ld_cur) ld_end(false);
        if (!k) ld_drain();
        if (!ld_on && !ld_begin(site == 2)) { c.r[2] = 0; return true; }
        if (!k) {
            double t = psp_time_us() / 1000.0;
            if (cur == ld_cur) { ld_waits++; ld_wait_ms += t - ld_t; if (ld_frames <= ld_d + 1) { ld_waits0++; ld_wait0_ms += t - ld_t; } } else ld_frames++;
            ld_cur = cur; ld_t = t;
            ld_tick();
        }
        int32_t tg = (int32_t)cur - (int32_t)ld_d;
        if (tg < 0) {
            uint32_t s = ld_buf + (4 * LD_RING + k) * 16;
            psp_s32(s, (uint32_t)tg); psp_s32(s + 4, 0); psp_s32(s + 8, 0); psp_s32(s + 12, 0x8080u);
            c.r[2] = s;
            return true;
        }
        f = (uint32_t)tg;
    }
    uint32_t s = ld_slot(k, f), want = (int32_t)k == ld_me ? LD_LOCAL : ld_cur_epoch();
    c.r[2] = (int32_t)f >= 0 && psp_l32(s) == f && ld_tag[k][f & (LD_RING - 1)] == want ? s : 0;
    return true;
}

// step functions written for a 30 Hz tick: only half the position and turn change is kept
// fighter silhouette for its shadow (0x0880E480): beyond 300 units from the camera the game draws an 8x8 square (a
// diamond on the ground); with soft shadows every fighter keeps its real silhouette
static bool sh_wrap(Ctx& c, void (*orig)(Ctx&)) {
    const char* e = getenv("TTT_SOFT_SHADOWS");
    if (e && *e && !atoi(e)) return false;
    uint32_t blk = psp_l32(c.r[4] + 0xB10);
    if (!blk || blk >= 0x10000000u) return false;
    FReg d; d.u = psp_l32(blk + 0x40);
    if (!(d.f >= 300.0f)) return false;
    uint32_t keep = d.u;
    d.f = 299.0f; psp_s32(blk + 0x40, d.u);
    orig(c);
    psp_s32(blk + 0x40, keep);
    static int logs = 0;
    if (logs < 6) { logs++; FReg k; k.u = keep; printf("sombra: silueta completa a distancia %.0f\n", (double)k.f); }
    return true;
}

// cel shading: aura colour per fighter (0x088B540C, +0x400) and GE NOP markers around each fighter draw (0x0880CAF4)
extern uint32_t psp_fig_mdl[8][9];
static uint32_t cel_node[8];
static uint32_t cel_seen[8], cel_draws[8];
static int cel_on() {
    static int v = 1, n = 0;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_CEL"); v = e && *e ? atoi(e) : 1; }
    return v;
}
static void cel_aura(Ctx& c) {
    uint32_t k = c.r[5];
    if (k < 8 && c.r[4] && c.r[4] < 0x10000000u) { cel_node[k] = c.r[4]; cel_seen[k] = cel_draws[k]; }
}
static int cel_phase(int ty) {   // aura types with a permanent tint (TTT_CEL_PHASE, default 3 Kaioken and 8 Super Saiyan)
    static char list[64] = ""; static int n = 0;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_CEL_PHASE"); snprintf(list, sizeof(list), ",%s,", e && *e ? e : "3,8"); }
    char k[8]; snprintf(k, sizeof(k), ",%d,", ty);
    return ty && strstr(list, k) ? 1 : 0;
}
static int cel_b(uint32_t a) { FReg f; f.u = psp_l32(a); float v = f.f * 255.0f + 0.5f; return !(v > 0.0f) ? 0 : v > 255.0f ? 255 : (int)v; }
static bool cel_wrap(Ctx& c, void (*orig)(Ctx&)) {
    int cv = cel_on();   // 0 off, 1/2 every fighter, 3/4 Mixed: still marked, the GPU applies it only to BT3 models (they carry normals)
    bool nrm_only = (cv <= 0 || cv >= 3) && !cel_force_now, none = cv <= 0 && !cel_force_now;   // none: marked for the outline, no cel at all
    static int bt3_cel = -1;
    if (bt3_cel < 0) { const char* e = getenv("TTT_CEL_BT3"); bt3_cel = e && *e ? atoi(e) : 1; }
    if (nrm_only && !bt3_cel) return false;
    uint32_t ent = c.r[4], p = c.r[5];
    int k = -1;
    for (int i = 0; i < 8 && k < 0; i++) if (psp_l32(0x08B61178u + 4 * i) == ent) k = i;
    bool rlv1 = ent && rm_lv1(ent), vlv1 = cv_lv1(ent) || rlv1, veyes = cv_blink(ent);   // Character Reference viewer / game room: level 1 textures and closed eyes on demand
    if (rlv1) { static int logs = 0; if (logs++ < 4) printf("room3d: nivel 1, modelo %s\n", k >= 0 ? "con marcador propio" : "marcado como el visor"); }
    if (k < 0 && (cv_is(ent) || rlv1)) k = 0;
    if (k < 0 || !p || p >= 0x10000000u) return false;
    for (int i = 0; i < 9; i++) psp_fig_mdl[k][i] = psp_l32(i ? ent + 0x50 + 4 * i : ent + 0x14);
    cel_draws[k]++;
    uint32_t n = cel_node[k];
    int r = 0, g = 0, b = 0, in = 0, ty = 0;
    if (n && cel_draws[k] - cel_seen[k] < 30) { r = cel_b(n + 0x400); g = cel_b(n + 0x404); b = cel_b(n + 0x408); in = cel_b(n + 0x4C); ty = (int)(psp_l32(n + 0x14) & 0xFF); }
    psp_s32(p, 0x00E10000u | (uint32_t)(r << 8 | g));
    psp_s32(p + 4, 0x00E20000u | (uint32_t)(b << 8 | in));
    psp_s32(p + 8, 0x00E30000u | (uint32_t)(cel_phase(ty) << 8) | (uint32_t)((costume_lv1(k) || vlv1) << 9) | (uint32_t)(veyes << 10) | (uint32_t)(nrm_only << 11) | (uint32_t)(cel_force_now << 12) | (uint32_t)(none << 13) | (uint32_t)(k + 1));
    c.r[5] = p + 12;
    orig(c);
    uint32_t e = c.r[2];
    if (e && e < 0x10000000u) { psp_s32(e, 0x00E40000u); c.r[2] = e + 4; }
    static int logs = 0; static uint32_t last[8];
    uint32_t key = (uint32_t)(r << 16 | g << 8 | b) ^ (uint32_t)((in > 128) << 24) ^ (uint32_t)(ty << 25);
    if (logs < 24 && key != last[k]) {
        last[k] = key; logs++;
        printf("cel: luchador %d personaje %u aura tipo %d (%d %d %d) intensidad %d kaioken %d nodo %s\n", k, psp_l32(ent + 8), ty, r, g, b, in,
               (psp_l32(ent + 0xA60) >> 18) & 1, n ? "si" : "no");
    }
    return true;
}

static void cel_mark(uint32_t w) {   // a GE NOP in the current libgu list
    uint32_t ctx = psp_l32(0x08B378D0u), p = ctx && ctx < 0x10000000u ? psp_l32(ctx + 8) : 0;
    if (p && p < 0x10000000u) { psp_s32(p, w); psp_s32(ctx + 8, p + 4); }
}
static void ch_aura_draw(Ctx& c, void (*orig)(Ctx&));
static void cel_aura_draw(Ctx& c, void (*orig)(Ctx&)) {
    if (cel_on() <= 0 || cel_on() >= 3) { ch_aura_draw(c, orig); return; }
    static int al = 0;
    if (al < 2) { al++; uint32_t ctx = psp_l32(0x08B378D0u); printf("cel: dibujo de aura, lista 0x%08X\n", ctx && ctx < 0x10000000u ? psp_l32(ctx + 8) : 0); }
    cel_mark(0x00E50000u);
    ch_aura_draw(c, orig);
    cel_mark(0x00E60000u);
}

// water: 0x0880BB28 is the game's underwater pass gate (its draw 0x0880BB20 is an empty stub in TTT); every frame it inverts the
// camera's view matrix ([0x08A7E25C] + 0x40, row vectors) and compares the camera Y with the level: the GPU gets the same inverse
extern float psp_wat_cam[8], psp_cam_m[20];
static void wat_wrap(Ctx& c, void (*orig)(Ctx&)) {
    uint32_t cam = psp_l32(0x08A7E260u) ? psp_l32(0x08A7E25Cu) : 0;
    if (cam && cam < 0x10000000u) {
        float m[16];
        for (int i = 0; i < 16; i++) { FReg f; f.u = psp_l32(cam + 0x40 + 4 * i); m[i] = f.f; psp_cam_m[i] = f.f; }
        psp_cam_m[16] += 1.0f;
        float a00 = m[0], a01 = m[1], a02 = m[2], a10 = m[4], a11 = m[5], a12 = m[6], a20 = m[8], a21 = m[9], a22 = m[10];
        float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
        if (std::fabs(det) > 1e-12f) {
            float b0 = -(a01 * a22 - a02 * a21) / det, b1 = (a00 * a22 - a02 * a20) / det, b2 = -(a00 * a21 - a01 * a20) / det;
            psp_wat_cam[0] = b0; psp_wat_cam[1] = b1; psp_wat_cam[2] = b2;
            psp_wat_cam[3] = -(m[12] * b0 + m[13] * b1 + m[14] * b2);
            psp_wat_cam[4] = 1.0f; psp_wat_cam[5] += 1.0f;
        }
    }
    orig(c);
}

// item 46: in survival the clash event state 0x1D waits for the second replacement, but its request (issued in 0x1C) is
// consumed from the change queue without a load (cause not found; the queue code is translated correctly); when the queue is
// idle there, the request is issued again from the fighter's next-form fields (+0x9D8..+0x9EC), as 0x1C does. TTT_SURVIVAL_RETRY=0 off
static void sv_retry(const Ctx& c) {
    static int on = -1, logs = 0;
    if (on < 0) { const char* e = getenv("TTT_SURVIVAL_RETRY"); on = e && *e ? atoi(e) != 0 : 1; }
    uint32_t t = psp_l32(0x08A7DBC8u), q = guest_ptr(t) ? t + 0x198u : 0;
    if (!on || !q || psp_l32(q + 0x24C) != 0 || psp_l32(q + 0x240) || psp_l32(q + 0x244) != psp_l32(q + 0x248)) return;
    uint32_t f = psp_l32(t + 0xCC + 0x10);
    if (!guest_ptr(f)) return;
    if (logs++ < 20) printf("clash: cola vacia en el estado 0x1D, se repite la peticion del jugador %u (personaje %u color %u)\n", psp_l32(f), psp_l32(f + 0x9D8), psp_l32(f + 0x9DC));
    Ctx c2 = c;
    c2.r[4] = psp_l32(f); c2.r[5] = psp_l32(f + 0x9D8); c2.r[6] = psp_l32(f + 0x9DC); c2.r[7] = psp_l8(f + 0x9E0);
    c2.r[8] = psp_l32(f + 0x9E4); c2.r[9] = psp_l32(f + 0x9E8); c2.r[10] = psp_l32(f + 0x9EC); c2.r[31] = 0;
    psp_call(c2, 0x08933D80u);
}
static void sv_wrap(Ctx& c, void (*orig)(Ctx&)) {
    uint32_t a0 = c.r[4];
    orig(c);
    if (a0 == 0x1Du && c.r[2] == 0x1Du) sv_retry(c);
}

// item 50 trace: fighter intros (phase 1) and victory poses (phase 6): manager sub-state, fighter, its motion and its voice channel
static void ip_trace(const Ctx& c) {
    static int logs = 0;
    static uint32_t prev[6] = {0xFFFFFFFFu};
    static std::chrono::steady_clock::time_point t0;
    uint32_t m = psp_l32(0x08A7D880u);
    if (logs >= 300 || !guest_ptr(m)) return;
    uint32_t ph = psp_l32(m), sub = psp_l32(m + 8), fi = psp_l32(m + 0xC), mo = 0xFFFFFFFFu, vo = 9, chs = 0;
    if (ph != 1 && ph != 6) { prev[0] = ph; return; }
    uint32_t t = psp_l32(0x08A7DBC8u), mgr = psp_l32(0x08A78EE8u);
    if (guest_ptr(t) && fi < 4) mo = psp_l32(psp_l32(t + 4) + 0x1A90u * fi + 0x9B0);
    if (guest_ptr(mgr) && fi < 4) vo = mpm_call(c, 0x0883A7F4u, mgr, fi + 1) != 0;
    uint32_t so = guest_ptr(mgr) ? psp_l32(mgr) : 0, arr = guest_ptr(so) ? psp_l32(so + 8) : 0, nch = guest_ptr(so) ? psp_l32(so + 4) : 0;
    for (uint32_t k = 0; guest_ptr(arr) && k < nch && k < 6; k++) { uint32_t ch = psp_l32(arr + 4 * k); chs |= (guest_ptr(ch) ? (psp_l32(ch + 0x38) & 0xF) : 0xF) << (4 * k); }
    uint32_t now[6] = {ph, sub, fi, mo, vo, chs};
    auto tn = std::chrono::steady_clock::now();
    if (prev[0] != ph) t0 = tn;
    if (!memcmp(now, prev, sizeof(now))) return;
    memcpy(prev, now, sizeof(now)); logs++;
    printf("intro: fase %u sub %u luchador %d movimiento 0x%X voz %u canales %06X, %lld ms\n", ph, sub, (int)fi, mo, vo, chs, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tn - t0).count());
}

// item 51: fighter special camera smoothing 0x089167E0 (called by 0x08919674 each tick): returns the lerp factor of the camera angles
// (0.2..0.5 per 30 Hz tick) and counts down +0x450+0x9C in ticks; at 60 the factor becomes 1-sqrt(1-k) and the countdown runs on even
// ticks only. TTT_MCAM60=0 off
static void mc_wrap(Ctx& c, void (*orig)(Ctx&)) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_MCAM60"); on = !(e && *e == '0'); }
    if (!on || !battle60()) { orig(c); return; }
    uint32_t cd = c.r[4] + 0x450u + 0x9Cu;
    int32_t n0 = (int32_t)psp_l32(cd);
    orig(c);
    if (n0 > 0 && (int32_t)psp_l32(cd) == n0 - 1 && tick_odd) psp_s32(cd, (uint32_t)n0);
    float k = c.f[0].f;
    if (k > 0.0f && k < 1.0f) c.f[0].f = 1.0f - std::sqrt(1.0f - k);
}

// change loader job 0x088553F0 (state +8, chara +0x18, costume +0x28, variant +0x2C, object +0x2D): offline, its state 0 waits
// until the new character's pak is held in RAM plus TTT_TRANSFORM_WAIT seconds (-1 off); the transformation loop keeps playing
bool psp_disc_held(uint32_t idx);
void psp_disc_prepare(uint32_t idx);
static bool tw_wait(Ctx& c) {
    const char* e = getenv("TTT_TRANSFORM_WAIT");
    int extra = e && *e ? atoi(e) : 2;
    uint32_t j = c.r[4];
    if (extra < 0 || ld_on || !j || j >= 0x10000000u || psp_l32(j + 8) != 0 || psp_l8(j + 0x2D) || psp_l8(j + 0x2C)) return false;
    static uint32_t cur_j = 0, cur_idx = 0;
    static bool ready = false;
    static std::chrono::steady_clock::time_point t0, tr;
    uint32_t idx = mpm_call(c, 0x089C6CA0u, psp_l32(j + 0x18), psp_l32(j + 0x28)) & 0xFFFFu;
    auto now = std::chrono::steady_clock::now();
    if (cur_j != j || cur_idx != idx) { cur_j = j; cur_idx = idx; ready = false; t0 = now; psp_disc_prepare(idx); }
    if (!ready) {
        if (!psp_disc_held(idx) && now - t0 < std::chrono::seconds(6)) return true;
        ready = true; tr = now;
    }
    if (now - tr < std::chrono::milliseconds(1000 * extra)) return true;
    static int logs = 0;
    if (logs++ < 20) printf("transform: indice %u listo a los %lld ms, liberado a los %lld ms (+%d s)\n", idx,
                            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tr - t0).count(),
                            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count(), extra);
    cur_j = 0;
    return false;
}

// credits player update (7_7): TTT_SKIP_CREDITS ends them once their pak is loaded and the title paks are queued (state +0 = 2),
// the same exit as the last page (+0 = +4 = 0, returns 1); the login runs before the credits
static void boot_credits(Ctx& c, void (*orig)(Ctx&)) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_SKIP_CREDITS"); on = e && *e ? atoi(e) : 0; }
    uint32_t o = c.r[4];
    orig(c);
    if (on > 0 && o && o < 0x10000000u && psp_l32(o) == 2) { psp_s32(o, 0); psp_s32(o + 4, 0); c.r[2] = 1; printf("arranque: creditos iniciales saltados\n"); fflush(stdout); }
}

// BT3 stage water splashes (item 65 D2): fighter feet (entity +0x950) and body centre (body node) per logic frame, for the GPU's EftWater
void psp_gpu_water_objs(const float* d, int n);
// bubbles (EftBubble): BT3 objects 0/1 = first two distinct fighters; per object snapshot index + 1, nodes ok, then the world position of each BB_IDS node
void psp_gpu_bubble_objs(const float* d);
static const int BB_IDS[20] = {0x03, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x20, 0x21, 0x22, 0x23, 0x30};
// D2b: TTT hit list [0x08A7BBE0] (64 x 0x1A0, count +0x6800) = BT3 BtlBlastList + 0x10: type +0x10, pos +0x30, prev +0x40,
// flags +0x60, task node +0x70 (flags +4, hit pos +0x10), src +0x74 (def +0x28), shape +0x80 (type, a +0x8C)
void psp_gpu_water_blasts(const float* e, int n);
// BT3 EftWater_AddSplashAt from BtlStage_UpdateObjs (a piece of a type 0x80 object crossing y 0): drawn by the GPU when it draws BT3 water
void psp_gpu_water_piece(const float* p);
float psp_gpu_wlevel();
float psp_gpu_blevel();
void psp_snd_water(uint32_t mask, float level);
void psp_snd_paks(const uint32_t* p);
static bool wt_piece(Ctx& c) {
    float L = psp_gpu_wlevel();
    if (L != L) return false;
    uint32_t a = c.r[4];
    if (a < 0x08800000u || a >= 0x10000000u) return false;
    float p[3] = {lf(a), lf(a + 4), lf(a + 8)};
    psp_gpu_water_piece(p);
    static int nl = 0;
    if (nl < 8) { nl++; printf("agua: salpicadura BT3 de trozo en (%.0f %.0f %.0f), nivel %.1f\n", p[0], p[1], p[2], L); fflush(stdout); }
    c.r[2] = 1;
    return true;
}
static void wt_blasts(const float* fd, int fn) {
    struct Seen { uint32_t nd; float o[3], p[3], q[3]; bool done; };
    static Seen sn[64];
    static int nsn = 0, logs = 0;
    float ev[64 * 16];
    int ne = 0;
    uint32_t L = psp_l32(0x08A7BBE0u);
    int cnt = L && L < 0x10000000u ? (int)psp_l32(L + 0x6800u) : 0;
    if (cnt < 0 || cnt > 64) cnt = 0;
    Seen nx[64];
    int nn = 0;
    for (int i = 0; i < cnt; i++) {
        uint32_t r = L + 0x1A0u * (uint32_t)i, nd = psp_l32(r + 0x70u);
        if (!nd || nd >= 0x10000000u) continue;
        float o[3], p[3], q[3];
        for (int k = 0; k < 3; k++) { o[k] = lf(r + 0x20u + 4u * k); p[k] = lf(r + 0x30u + 4u * k); q[k] = lf(r + 0x40u + 4u * k); }
        Seen* old = nullptr;
        for (int j = 0; j < nsn; j++) if (sn[j].nd == nd && !memcmp(sn[j].o, o, 12)) { old = &sn[j]; break; }
        Seen& cur = nx[nn++];
        cur.nd = nd; memcpy(cur.o, o, 12); memcpy(cur.p, p, 12); memcpy(cur.q, q, 12); cur.done = old && old->done;
        uint32_t tf = psp_l32(nd + 4);
        if (!(tf & 1)) cur.done = false;
        if (psp_l32(r + 0x60u) & 8) continue;
        if ((tf & 4) || cur.done) continue;
        bool hit = (tf & 1) != 0;
        if (!hit && old && !memcmp(old->p, p, 12) && !memcmp(old->q, q, 12)) continue;
        if (hit) cur.done = true;
        int own = 0; float bd = 1e30f;
        for (int f = 0; f < fn; f++) {
            const float* b = fd + 7 * f + 4;
            float dx = b[0] - o[0], dy = b[1] - o[1], dz = b[2] - o[2], dd = dx * dx + dy * dy + dz * dz;
            if (dd < bd) { bd = dd; own = f; }
        }
        float h = fn ? 2.0f * (fd[7 * own + 2] - fd[7 * own + 5]) : 20.0f;
        if (!(h > 4.0f && h < 200.0f)) h = 20.0f;
        uint32_t sh = r + 0x80u, sa = psp_l32(sh + 0xC);
        int st = (int)psp_l32(sh);
        float rad = sa && sa < 0x10000000u ? (st == 0 ? lf(sa + 0x10) : st == 1 ? lf(sa + 0x20) : 0.0f) : 0.0f;
        uint32_t src = psp_l32(r + 0x74u), def = src && src < 0x10000000u ? psp_l32(src + 0x28u) : 0;
        float kind = -999.0f, cls = 0.0f;
        if (src && def && def < 0x10000000u) {
            int kd = (int8_t)psp_l8(def + 5), id = (int16_t)psp_l16(def), c = 0;
            if (kd == 3) c = 3; else if (kd == 4) c = 2;
            if (id == 0x1C8 || id == 0x159 || id == 0x270) c = 3;
            if (id == 0x16F) c = 4;
            kind = (float)kd; cls = (float)c;
        }
        float* e = ev + 16 * ne++;
        e[0] = (float)own; e[1] = h * 0.05f;
        memcpy(e + 2, p, 12); memcpy(e + 5, q, 12);
        e[8] = rad; e[9] = (float)(int)psp_l32(r + 0x10u); e[10] = hit ? 1.0f : 0.0f;
        for (int k = 0; k < 3; k++) e[11 + k] = lf(nd + 0x10u + 4u * k);
        e[14] = kind; e[15] = cls;
        static uint32_t pairs[16];
        uint32_t pr = (psp_l32(r) & 0xFFFF) << 8 | (uint32_t)own << 1 | (e[9] != 0 ? 1u : 0u);
        bool nw = true;
        for (int j = 0; j < logs; j++) if (pairs[j] == pr) nw = false;
        if (nw && logs < 16) { pairs[logs++] = pr; printf("agua: golpe BT3 en lista, id %d, dueno %d, tipo %d, radio %.2f, clase %d\n", (int)psp_l32(r), own, (int)e[9], rad, (int)cls); fflush(stdout); }
    }
    memcpy(sn, nx, sizeof(Seen) * nn); nsn = nn;
    if (ne) psp_gpu_water_blasts(ev, ne);
}
static uint32_t snd_req_frame = 0;
static int wt_state_of(int o);
float psp_gpu_wlevel();
void psp_gpu_water_sound(int id, const float* pos);
bool psp_bt3_snd_pcm(int id, const int16_t** pcm, int* n, int* rate);
static float wt_feet[8][3], wt_h[8];
static bool wt_have[8];
// BT3 BtlAct_DashMoveHandler: dash start 0x23 at the water surface (flag 0x12), 0x39 submerged (flag 0x11); TTT asks 0x1E
static bool wt_dash_sub(const Ctx& c) {
    int id = (int)c.r[7], o = (int)c.r[5];
    if ((c.r[6] & 0xFF) != 0 || (id != 0x1E && id != 0x23 && id != 0x39) || o < 0 || o >= 8 || !wt_have[o]) return false;
    float L = psp_gpu_wlevel();
    if (L != L) return false;
    float y = wt_feet[o][1], h = wt_h[o];
    bool surf = y - h < L && L < y + 1.0f, sub = L < y - h * 0.5f;
    int want = surf ? 0x23 : sub ? 0x39 : 0;
    const int16_t* p; int n, r;
    bool have = want && psp_bt3_snd_pcm(want, &p, &n, &r);
    static int nl = 0;
    if (nl < 40) { nl++; printf("sonido: dash c%u id 0x%X dueno %d pies %.1f altura %.1f nivel %.1f -> %s\n", snd_req_frame, id, o, y, h, L, !want ? "fuera del agua" : have ? (want == 0x23 ? "BT3 0x23 superficie" : "BT3 0x39 sumergido") : "sin wav, sonido de TTT"); fflush(stdout); }
    if (!have) return false;
    float pos[3];
    for (int k = 0; k < 3; k++) pos[k] = lf(c.r[4] + 4u * k);
    psp_gpu_water_sound(want, pos);
    return true;
}
static int wt_state_of(int o) {
    float L = psp_gpu_wlevel();
    if (o < 0 || o >= 8 || !wt_have[o] || L != L) return 0;
    float y = wt_feet[o][1], h = wt_h[o];
    return y - h < L && L < y + 1.0f ? 2 : L < y - h * 0.5f ? 3 : 1;
}

// SZ two-phase charge (item 69, TTT_CHARGE2): phase 1 charging below full ki, phase 2 ki full and the orange gauge rising,
// 3 sparking; phase 2 and sparking darken the scene (GPU) and give the aura a bigger, taller shape
extern int psp_charge_dark;
static uint8_t ch_ph[8], ch_pe[8], ch_in2[8], ch_srv[8]; static int ch_in2t[8];   // phase 2 charge in played inside the loop motion (0x35): state per entry, served data per slot   // phase by character slot (fighter +0x2C, highest of its entries) and by fighter entry
static uint32_t ch_alt[2] = {0, 0}; static int ch_alt_st[2] = {0, 0};   // phase 1 animations in guest memory: 0 charge in (phase1_in.tanm), 1 loop (phase1.tanm)
extern int psp_spark_n; extern float psp_spark[8][10];
static int ch_on() { static int v = 1, n = 0; if ((n++ & 63) == 0) { const char* e = getenv("TTT_CHARGE2"); v = e && *e == '1'; } return v; }
static uint32_t ch_load(const std::string& nm, const char* tag) {
    FILE* fp = fopen(nm.c_str(), "rb");
    if (!fp) return 0;
    std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(fp);
    if (b.size() < 16) { printf("carga: %s vacio o roto (%u bytes)\n", tag, (unsigned)b.size()); return 0; }
    uint32_t g = psp_reserve((uint32_t)(b.size() + 63) & ~63u);
    memcpy(psp_p(g), b.data(), b.size());
    printf("carga: %s cargada (%u bytes, tamano descomprimido %u) en 0x%08X\n", tag, (unsigned)b.size(), (unsigned)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]), g);
    return g;
}
static uint32_t ch_alt_get(int w) {
    if (ch_alt_st[w]) return ch_alt[w];
    ch_alt_st[w] = 1;
    const char* nm = w ? "../../games/ttt/charge/phase1.tanm" : "../../games/ttt/charge/phase1_in.tanm";
    ch_alt[w] = ch_load(nm, nm + 6);
    if (!ch_alt[w]) printf("carga: sin %s, se usa la del personaje\n", nm + 6);
    return ch_alt[w];
}
std::string psp_pak_char_dir(uint32_t pak);
std::string psp_cr_file(const std::string& rel);
// phase 1 animations of the character's own mod (Packfile/<character>/charge/phase1_in.tanm + phase1.tanm, both or none), per character folder
static uint32_t ch_own_get(uint32_t ent, int w) {
    struct Own { uint32_t a[2] = {0, 0}; };
    static std::map<std::string, Own> own;
    uint32_t hdr = psp_l32(ent + 0x14);
    if (!hdr || hdr >= 0x10000000u || psp_l32(hdr) != 0x6C644D70u) return 0;
    uint32_t pak = lip_pak(hdr);
    if (!pak) return 0;
    std::string d = psp_pak_char_dir(pak);
    if (d.empty()) return 0;
    auto it = own.find(d);
    if (it == own.end()) {
        Own o;
        std::string b = "Packfile/" + d + "/charge/", fi = psp_cr_file(b + "phase1_in.tanm"), fl = psp_cr_file(b + "phase1.tanm");
        if (!fi.empty() && !fl.empty()) { o.a[0] = ch_load(fi, (d + "/charge/phase1_in.tanm").c_str()); o.a[1] = ch_load(fl, (d + "/charge/phase1.tanm").c_str()); }
        if (!o.a[0] || !o.a[1]) o.a[0] = o.a[1] = 0;
        printf("carga: %s %s\n", d.c_str(), o.a[0] ? "con animaciones de fase 1 propias del mod" : "sin animaciones de fase 1 propias, se usan las genericas");
        it = own.emplace(d, o).first;
    }
    return it->second.a[w];
}
static float ch_k(const char* n, float d);
static uint32_t ch_vfx_node[8], ch_vfx_base[8];   // charge aura VFX (01_charge_aura) node per slot and its owner scale (data +0x544)
static void ch_vfx_scale(int slot, int ph) {
    uint32_t T = psp_l32(0x08A7D278u), node = T && T < 0x10000000u ? psp_l32(T + 8u * slot) : 0, data = node && node < 0x10000000u ? psp_l32(node + 0x38) : 0;
    if (!data || data >= 0x10000000u) { ch_vfx_node[slot] = 0; return; }
    if (ch_vfx_node[slot] != node) { ch_vfx_node[slot] = node; ch_vfx_base[slot] = psp_l32(data + 0x544); }
    FReg v; v.u = ch_vfx_base[slot];
    if (ph >= 2) v.f *= ch_k("TTT_CHARGE2_VFX", 1.4f);
    psp_s32(data + 0x544, v.u);
    // taller: the particle emitters (type 0x05) read their shape live; height keys (+0xE8 base, +0x10C random, +0x154/+0x160 oscillation) scaled while phase 2 lasts
    uint32_t R = psp_l32(data + 0x14), blk = R && R < 0x10000000u ? psp_l32(R + 0xC + 16 * 5) : 0;
    if (!blk || blk >= 0x10000000u) return;
    int r0 = psp_l8(R + 0xC + 16 * 5 + 8), nr = std::min((int)psp_l8(blk + 1), 8);
    static uint32_t shp[8][8]; static uint32_t keep[8][8][24];
    float ky = ph >= 2 ? ch_k("TTT_CHARGE2_TALL", 2.2f) : 1.0f;
    static const int OFF[4] = {0xE8, 0x10C, 0x154, 0x160}, CNT[4] = {9, 9, 3, 3};
    for (int r = 0; r < nr; r++) {
        uint32_t sh = psp_l32(R + 0x1BC + 8 * (r0 + r));
        if (!sh || sh >= 0x10000000u || psp_l8(sh + 5) == 1) continue;
        if (shp[slot][r] != sh) { shp[slot][r] = sh; int q = 0; for (int g = 0; g < 4; g++) for (int j = 0; j < CNT[g]; j++) keep[slot][r][q++] = psp_l32(sh + OFF[g] + 4 * j); }
        int q = 0;
        for (int g = 0; g < 4; g++) for (int j = 0; j < CNT[g]; j++) { FReg f; f.u = keep[slot][r][q++]; f.f *= ky; psp_s32(sh + OFF[g] + 4 * j, f.u); }
    }
    static int lg = 0;
    if (ph >= 2 && lg < 3) { lg++; FReg b; b.u = ch_vfx_base[slot]; printf("carga: casilla %d, VFX del aura de carga con escala %.2f (base %.2f)\n", slot, v.f, b.f); }
}
// phase 2 aura of the character's mod (Packfile/<character>/charge/phase2_aura.pak = a 01_charge_aura): a second effect set S2 parsed like the
// loader's (0x088C9474), served to the aura node created while phase >= 2 (0x088C95BC reads [[L+0x38]]); ticked and freed with the loader
struct ChAura { uint32_t L = 0, S = 0, S2 = 0, pak = 0, kill = 0; std::string dir; int st = 0; };
static ChAura ch_a[8];
static std::string ch_dir_of(int slot) {
    uint32_t ent = psp_l32(0x08B61178u + 4u * slot), hdr = ent && ent < 0x10000000u ? psp_l32(ent + 0x14) : 0;
    if (!hdr || hdr >= 0x10000000u || psp_l32(hdr) != 0x6C644D70u) return "";
    uint32_t pak = lip_pak(hdr);
    return pak ? psp_pak_char_dir(pak) : "";
}
static uint32_t ch_a_get(Ctx& c, int slot) {
    static std::map<std::string, uint32_t> paks;
    static uint32_t s2[8];
    uint32_t R0 = psp_l32(0x08A7CA50u), R1 = R0 && R0 < 0x10000000u ? psp_l32(R0 + 4) : 0;   // 0x088C5C7C(slot, 0)
    uint32_t P = R1 && R1 < 0x10000000u ? psp_l32(R1 + 0x30u * (uint32_t)slot + 8) : 0;   // the aura's node pool; its owner (+0) is the loader
    uint32_t L = P && P < 0x10000000u ? psp_l32(P) : 0, lp = L && L < 0x10000000u ? psp_l32(L + 0x38) : 0, S = lp && lp < 0x10000000u ? psp_l32(lp) : 0;
    ChAura& a = ch_a[slot];
    static int lgs = 0;
    if (!S || S >= 0x10000000u) { if (lgs < 6) { lgs++; printf("carga: casilla %d, aura de fase 2 sin cargador (tabla %08X, cargador %08X, datos %08X)\n", slot, R1, L, S); } a.L = 0; a.st = 0; return 0; }
    std::string d = ch_dir_of(slot);
    if (d.empty() && lgs < 6) { lgs++; uint32_t ent = psp_l32(0x08B61178u + 4u * slot), hdr = ent && ent < 0x10000000u ? psp_l32(ent + 0x14) : 0; printf("carga: casilla %d, aura de fase 2 sin carpeta del personaje (entidad %08X, modelo %08X)\n", slot, ent, hdr); }
    if (a.L != L || a.S != S || a.dir != d) {
        if (a.st == 2 && a.L == L) mpm_call(c, 0x088973B0u, a.S2, a.pak);
        a = ChAura(); a.L = L; a.S = S; a.dir = d;
    }
    if (a.st == 0) {
        a.st = 1;
        if (d.empty()) return 0;
        auto it = paks.find(d);
        if (it == paks.end()) {
            std::string f = psp_cr_file("Packfile/" + d + "/charge/phase2_aura.pak");
            it = paks.emplace(d, f.empty() ? 0u : ch_load(f, (d + "/charge/phase2_aura.pak").c_str())).first;
            if (!it->second) printf("carga: %s sin aura de fase 2 propia\n", d.c_str());
        }
        if (!it->second) return 0;
        if (!s2[slot]) s2[slot] = psp_reserve(0x340);
        a.S2 = s2[slot]; a.pak = it->second;
        memset(psp_p(a.S2), 0, 0x324);
        psp_s32(a.S2 + 0x320, a.pak);
        mpm_call(c, 0x0889BB64u, 0, a.S2, 0, a.pak, 2, 1);
        a.st = 2;
        printf("carga: casilla %d, aura de fase 2 de %s lista (cargador %08X)\n", slot, d.c_str(), L);
    }
    return a.st == 2 ? a.S2 : 0;
}
static void ch_a_create(Ctx& c, void (*orig)(Ctx&)) {   // 0x088C95BC(&slot)
    uint32_t slot = c.r[4] && c.r[4] < 0x10000000u ? psp_l32(c.r[4]) : 99;
    uint32_t S2 = slot < 8 && ch_on() && ch_ph[slot] >= 2 ? ch_a_get(c, (int)slot) : 0;
    if (!S2) { orig(c); return; }
    uint32_t lp = psp_l32(ch_a[slot].L + 0x38), keep = psp_l32(lp);
    psp_s32(lp, S2);
    orig(c);
    psp_s32(lp, keep);
    static int lg = 0; if (lg < 8) { lg++; printf("carga: casilla %u, aura de carga creada con el aura de fase 2 del mod\n", slot); }
}
static void ch_a_loader(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {   // 0x088C958C(L) update, 0x088C9534(L) destroy
    uint32_t L = c.r[4];
    int k = 0; while (k < 8 && !(ch_a[k].st == 2 && ch_a[k].L == L)) k++;
    if (k < 8 && dir == 0x088C9534u) { mpm_call(c, 0x088973B0u, ch_a[k].S2, ch_a[k].pak); ch_a[k] = ChAura(); }
    orig(c);
    if (k < 8 && dir == 0x088C958Cu) mpm_call(c, 0x08897458u, ch_a[k].S2, ch_a[k].pak);
}
static void ch_tick(Ctx& c) {
    uint8_t now[8] = {0};
    int dark = 0, ns = 0;
    float sp[8][10];
    uint32_t t = psp_l32(0x08A7DBC8u);
    int cnt = t && ch_on() ? (int)psp_l32(t) : 0;
    if (cnt > 0 && cnt <= 8) {
        uint32_t base = psp_l32(t + 4);
        for (int i = 0; i < cnt; i++) {
            uint32_t f = base + 0x1A90u * (uint32_t)i, slot = psp_l32(f + 0x2C);
            if (slot >= 8 || !entity_of(f)) continue;
            int act = (int)psp_l32(f + 0x984), ki = (int)psp_l32(f + 0x1A40), mx = (int)psp_l32(f + 0x1A44);
            int ph = psp_l8(f + 0xDDB) ? 3 : act == 60 ? (mx > 0 && ki >= mx ? 2 : 1) : 0;
            if (ph > now[slot]) now[slot] = (uint8_t)ph;
            if (ph >= 2) dark = 1;
            uint32_t e = entity_of(f), nd = e ? psp_l32(e + 0xDA0u + 0x18u) : 0;
            static uint8_t sk_atk[8]; static int sk_var = -1, sk_lg = 0; static int sk_lg2 = 0; static int sk_last[8];
            if (sk_var < 0) { const char* e = getenv("TTT_SPARK_CUT"); sk_var = e && *e ? atoi(e) : 1; }
            if (ph != 3) sk_atk[slot] = 0;
            else if (sk_var > 0 && !sk_atk[slot]) {   // one of the three techniques started in sparking (0x10C..0x111 are the skills, EBOOT_TTT 77.1): the flames go until the next sparking
                bool atk = act > 0x111 && act <= 0x14A;
                if (atk) { sk_atk[slot] = 1; if (sk_lg2 < 20) { sk_lg2++; printf("carga: casilla %u ataca en sparking (accion %d), llamas fuera\n", slot, act); } }
                else if (act != sk_last[slot] && act != 12 && act != 16 && act != 60 && sk_lg < 30) { sk_lg++; printf("carga: casilla %u en sparking, accion %d no cuenta como ataque\n", slot, act); }
            }
            sk_last[slot] = act;
            if (ph == 3 && !sk_atk[slot] && nd >= 0x08800000u && nd < 0x10000000u && ns < 8) {   // sparking: waist bone, head bone and aura colour for the GPU's flame emitter
                float* q = sp[ns++], ws[3], hd[3]; uint32_t tb = s3d_mtx(e);
                bool bones = tb && s3d_bone_pos(e, tb, 0x03, ws) && s3d_bone_pos(e, tb, 0x2F, hd);
                if (bones) { float d2 = 0; for (int k = 0; k < 3; k++) d2 += (hd[k] - ws[k]) * (hd[k] - ws[k]); bones = d2 > 1.0f && d2 < 900.0f && d2 == d2; }
                if (bones) for (int k = 0; k < 3; k++) { q[k] = ws[k]; q[3 + k] = hd[k]; }
                else for (int k = 0; k < 3; k++) { FReg v; v.u = psp_l32(nd + 0x40 + 4 * k); q[k] = v.f; v.u = psp_l32(e + 0x950 + 4 * k); q[3 + k] = 2.0f * q[k] - v.f; }
                static int lgb = 0; if (lgb < 2) { lgb++; printf("carga: casilla %d, ancla de llamas %s: cintura (%.1f %.1f %.1f) cabeza (%.1f %.1f %.1f)\n", slot, bones ? "por huesos" : "por nodo", q[0], q[1], q[2], q[3], q[4], q[5]); }
                uint32_t an = cel_node[slot];
                float mxc = 0; for (int k = 0; k < 3; k++) { FReg v; v.u = an ? psp_l32(an + 0x410 + 4 * k) : 0; q[6 + k] = an && v.f == v.f ? v.f : 0.0f; mxc = std::max(mxc, q[6 + k]); }   // +0x410 = visible aura hue (EBOOT_TTT 65.3), normalised by brightness
                for (int k = 0; k < 3; k++) q[6 + k] = mxc > 0.02f ? q[6 + k] / mxc : 0.8f;
                q[9] = (float)slot;
            } else if (ph == 3 && !sk_atk[slot]) { static int lgn = 0; if (lgn < 6) { lgn++; printf("carga: casilla %d en sparking sin fila de llamas (entidad %08X, nodo %08X, filas %d)\n", slot, e, nd, ns); } }
            static int lg = 0;
            if (ph != ch_pe[i] && lg < 60) { lg++; printf("carga: luchador %d (casilla %u) fase %d -> %d (ki %d de %d, naranja %d, animacion 0x%X)\n", i, slot, ch_pe[i], ph, ki, mx, (int)psp_l32(f + 0x1A54), psp_l32(f + 0x9B0)); }
            if (ch_pe[i] == 1 && ph == 2) {   // ki full: the character's own charge in, played as the loop motion so the charge (ki, aura, loop SFX) goes on; then its own loop
                ch_in2[i] = 1; ch_in2t[i] = 0; ch_srv[slot] = 1;
                mpm_callf(c, 0x089155B0u, f, 0x35, 0.15f);
                mpm_call(c, 0x0893D828u, f, 0x15); mpm_call(c, 0x08942B5Cu, f, 0xD);
            }
            if (ch_in2[i]) {
                uint32_t en = psp_l32(0x08B61178u + 4u * slot);
                FReg cu, sp2, ln; cu.u = en ? psp_l32(en + 0xB60 + 0x138) : 0; sp2.u = en ? psp_l32(en + 0xB60 + 0x140) : 0; ln.u = en ? psp_l32(en + 0xB60 + 4) : 0;
                ch_in2t[i]++;
                bool end = ph != 2 || ch_in2t[i] > 240;
                if (ch_in2[i] == 1 && cu.f > 1.5f) ch_in2[i] = 2;
                else if (ch_in2[i] == 2 && cu.f + 2.0f * sp2.f >= ln.f) end = true;   // the in animation reaches its last frame: switch before it wraps
                if (end) {
                    ch_in2[i] = 0; ch_srv[slot] = 0;
                    if (ph == 2) mpm_callf(c, 0x089155B0u, f, 0x35, 0.15f);
                    static int lg2 = 0; if (lg2 < 8) { lg2++; printf("carga: luchador %d, inicio de fase 2 terminado en %d ticks, sigue el bucle propio\n", i, ch_in2t[i]); }
                }
            }
            ch_pe[i] = (uint8_t)ph;
        }
    }
    memcpy(ch_ph, now, 8);
    psp_charge_dark = dark;
    if (cnt > 0 && cnt <= 8) for (int k = 0; k < 8; k++) {
        uint32_t S2 = now[k] >= 2 ? ch_a_get(c, k) : 0;
        if (now[k] < 2) ch_a[k].kill = 0;
        if (S2) {   // the phase 1 aura fades out (the game's own exit, 0x088C9690); once it is gone the game creates the phase 2 one
            uint32_t T = psp_l32(0x08A7D278u), node = T && T < 0x10000000u ? psp_l32(T + 8u * k) : 0, data = node && node < 0x10000000u ? psp_l32(node + 0x38) : 0;
            if (data && data < 0x10000000u && psp_l32(data + 0x14) != S2 && ch_a[k].kill != node) { ch_a[k].kill = node; mpm_call(c, 0x088C9690u, (uint32_t)k); printf("carga: casilla %d, aura de fase 1 desvaneciendose para el aura de fase 2\n", k); }
        }
        ch_vfx_scale(k, S2 ? 0 : now[k]);
    }
    memcpy(psp_spark, sp, sizeof(float) * 10 * ns); psp_spark_n = ns;
}
static void ch_anim(Ctx& c, void (*orig)(Ctx&)) {   // 0x089B0278(ent, dest, motion, out): charge in / loop of phase 1 decompress the port's animations
    uint32_t ent = c.r[4], id = c.r[6];
    if ((id != 0x34 && id != 0x35) || !ch_on() || !ent) { orig(c); return; }
    int k = 0; while (k < 8 && psp_l32(0x08B61178u + 4u * k) != ent) k++;
    bool p1 = false;
    uint32_t t = psp_l32(0x08A7DBC8u);
    int cnt = k < 8 && t ? (int)psp_l32(t) : 0;
    if (cnt > 0 && cnt <= 8) {
        uint32_t base = psp_l32(t + 4);
        for (int i = 0; i < cnt; i++) {
            uint32_t f = base + 0x1A90u * (uint32_t)i;
            if (psp_l32(f + 0x2C) == (uint32_t)k && entity_of(f) && !psp_l8(f + 0xDDB) && (int)psp_l32(f + 0x1A40) < (int)psp_l32(f + 0x1A44)) p1 = true;
        }
    }
    uint32_t alt = p1 ? ch_own_get(ent, id == 0x35 ? 1 : 0) : 0;
    if (p1 && !alt) alt = ch_alt_get(id == 0x35 ? 1 : 0);
    if (!p1 && id == 0x35 && k < 8 && ch_srv[k]) alt = psp_l32(ent + 0xC0u + 4u * 0x34u);   // phase 2 in: the character's own 052 inside the loop motion
    if (!alt) { orig(c); return; }
    uint32_t slot = ent + 0xC0u + 4u * id, keep = psp_l32(slot);
    psp_s32(slot, alt);
    orig(c);
    psp_s32(slot, keep);
    static int lg = 0;
    if (lg < 8) { lg++; printf("carga: casilla %d, %s\n", k, !p1 ? "inicio propio de fase 2 dentro del bucle" : id == 0x35 ? "bucle de carga con la animacion de fase 1" : "inicio de carga con la animacion de fase 1"); }
}
static float ch_k(const char* n, float d) { const char* e = getenv(n); return e && *e ? (float)atof(e) : d; }
static void ch_aura(Ctx& c, void (*orig)(Ctx&)) {   // after the colour/size tick: phase 2 and sparking enlarge the aura (data +0x60)
    uint32_t data = c.r[4], idx = c.r[5];
    orig(c);
    if (idx < 8 && ch_ph[idx] >= 2 && data && data < 0x10000000u) { FReg v; v.u = psp_l32(data + 0x60); v.f *= ch_k("TTT_CHARGE2_SIZE", 1.6f); psp_s32(data + 0x60, v.u); }
}

static void ch_aura_draw(Ctx& c, void (*orig)(Ctx&)) {   // 0x088B5CD0(node): phase 2 and sparking draw this owner's aura taller (particle +0x18 = height, x data +0x58 = width)
    uint32_t node = c.r[4], data = node && node < 0x10000000u ? psp_l32(node + 0x38) : 0, owner = data && data < 0x10000000u ? psp_l32(data + 0x64) & 0xFF : 99;
    uint32_t mgr = psp_l32(0x08A7C790u);
    if (owner >= 8 || ch_ph[owner] < 2 || !mgr || mgr >= 0x10000000u || !ch_on()) { orig(c); return; }
    float sy = ch_k("TTT_CHARGE2_TALL", 2.0f), sx = ch_k("TTT_CHARGE2_WIDE", 1.2f);
    std::vector<std::pair<uint32_t, uint32_t>> keep;
    for (uint32_t p = psp_l32(mgr + 0x14), n = 0; p && p < 0x10000000u && n < 4096; p = psp_l32(p + 0xC0), n++) {
        if (psp_l8(p + 7) != owner) continue;
        FReg v; v.u = psp_l32(p + 0x18); keep.push_back({p + 0x18, v.u}); v.f *= sy; psp_s32(p + 0x18, v.u);
    }
    FReg w; w.u = psp_l32(data + 0x58); keep.push_back({data + 0x58, w.u}); w.f *= sx / sy; psp_s32(data + 0x58, w.u);
    orig(c);
    for (auto& k : keep) psp_s32(k.first, k.second);
    static int lg = 0;
    if (lg < 3) { lg++; printf("carga: casilla %u, aura dibujada %.1f veces mas alta (%d particulas)\n", owner, sy, (int)keep.size() - 1); }
}
static void ch_aura_spawn(Ctx& c, void (*orig)(Ctx&)) {   // 0x088B446C(data, owner): taller rise of this owner's particles (block P +0x190)
    uint32_t owner = c.r[5], P = psp_l32(0x08A7C794u);
    bool on = owner < 8 && ch_ph[owner] >= 2 && P >= 0x08800000u && P < 0x10000000u;
    uint32_t keep = on ? psp_l32(P + 0x190) : 0;
    if (on) { FReg v; v.u = keep; v.f *= ch_k("TTT_CHARGE2_RISE", 2.6f); psp_s32(P + 0x190, v.u); }
    orig(c);
    if (on) psp_s32(P + 0x190, keep);
}
static void wt_tick() {
    static uint32_t le[8];
    static float lh[8];
    float d[4 * 7] = {0};
    int n = 0;
    float bb[2 * 62] = {0};
    uint32_t be[2] = {0, 0};
    float BL = psp_gpu_blevel();
    uint32_t t = psp_l32(0x08A7DBC8u), st = psp_l32(0x08A7E678u);
    int cnt = t && st ? (int)psp_l32(t) : 0;
    for (auto& x : wt_have) x = false;
    if (cnt > 0 && cnt <= 8) {
        uint32_t base = psp_l32(t + 4);
        for (int i = 0; i < cnt && n < 4; i++) {
            uint32_t e = entity_of(base + 0x1A90u * (uint32_t)i), nd = e ? psp_l32(e + 0xDA0u + 0x18u) : 0;
            if (!e || !nd || nd < 0x08800000u || nd >= 0x10000000u) continue;
            float* q = d + 7 * n++;
            q[0] = 1;
            if (BL == BL && e != be[0] && e != be[1]) {
                int k = be[0] ? 1 : 0;
                if (!be[k]) {
                    be[k] = e;
                    float* o = bb + 62 * k;
                    o[0] = (float)n;
                    uint32_t tb = s3d_mtx(e);
                    bool ok = tb && tb < 0x10000000u;
                    for (int j = 0; ok && j < 20; j++) ok = s3d_bone_pos(e, tb, BB_IDS[j], o + 2 + 3 * j);
                    o[1] = ok ? 1.0f : 0.0f;
                }
            }
            for (int k = 0; k < 3; k++) { q[1 + k] = lf(e + 0x950u + 4u * k); q[4 + k] = lf(nd + 0x40u + 4u * k); }
            float h = 2.0f * (q[2] - q[5]);
            if (le[i] != e) { le[i] = e; lh[i] = 0; }
            if (lh[i] == 0 && h > 4.0f && h < 200.0f) lh[i] = h;
            if (lh[i] > 0) q[5] = q[2] - lh[i] * 0.5f;   // BT3 water test uses the pose position and a fixed height, not the animated body
            wt_have[i] = lh[i] > 0; wt_h[i] = lh[i]; memcpy(wt_feet[i], q + 1, 12);
            // BT3 actions + 1 in TTT: move 14 (0xD) plays 0x38 submerged on enter; dash 16 (0xF) plays 0x39 submerged, not when resumed from 0x3F/0xB1/0xB2
            static uint32_t act0[8];
            uint32_t ac = psp_l32(base + 0x1A90u * (uint32_t)i + 0x984u), pa = act0[i];
            act0[i] = ac;
            float L = psp_gpu_wlevel();
            bool sub = wt_have[i] && L == L && L < wt_feet[i][1] - wt_h[i] * 0.5f;   // BT3 flag 0x11 (flag 0x12, surface, wins only for the dash)
            if (ac != pa && sub) {
                int sid = ac == 14 ? 0x38 : ac == 16 && wt_state_of(i) == 3 && pa != 0x3F && pa != 0xB1 && pa != 0xB2 ? 0x39 : 0;
                if (sid) {
                    psp_gpu_water_sound(sid, q + 4);
                    static int nl = 0;
                    if (nl < 8) { nl++; printf("sonido: BT3 0x%X por accion %u del luchador %d sumergido\n", sid, ac, i); fflush(stdout); }
                }
            }
        }
    }
    psp_gpu_water_objs(d, n);
    {   // underwater sound: submerged fighters (BT3 flag 0x11, same test as the water sounds) and the level, for the mixer
        float L = psp_gpu_wlevel();
        uint32_t um = 0;
        for (int i = 0; i < 8; i++) if (wt_have[i] && L == L && L < wt_feet[i][1] - wt_h[i] * 0.5f) um |= 1u << i;
        if (L == L) psp_snd_water(um, L);
        uint32_t sp[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // model pak of each slot: the voices' speaker (character folder)
        uint32_t tt = psp_l32(0x08A7DBC8u);
        int cn = tt ? (int)psp_l32(tt) : 0;
        for (int i = 0; L == L && i < cn && i < 8; i++) {
            uint32_t e = entity_of(psp_l32(tt + 4) + 0x1A90u * (uint32_t)i), hdr = e ? psp_l32(e + 0x14) : 0;
            if (hdr && hdr < 0x10000000u && psp_l32(hdr) == 0x6C644D70u) sp[i] = lip_pak(hdr);
        }
        if (L == L) psp_snd_paks(sp);
    }
    psp_gpu_bubble_objs(bb);
    wt_blasts(d, n);
    snd_req_frame++;
}
void psp_disc_reload_poll(bool blocked);
bool net_online_on();
// TTT_HEAP_CHECK=2: while the 3D viewer is on, the game heap is walked before and after every wrapped call; the first break names its window
static const uint32_t NL_AV = 0x08A809A0u;   // newlib av_: bin i header at NL_AV + 8 i, top = [NL_AV + 8]
static uint32_t hw_walk(const char** why) {
    uint32_t a, e;
    if (!psp_heap_arena(&a, &e)) return 0;
    uint32_t top = psp_l32(NL_AV + 8);
    if (top < a || top + 16 > e) { *why = "tope fuera del monton"; return top; }
    uint32_t p = a, n = 0;
    if ((psp_l32(p + 4) & ~7u) == 0 && (psp_l32(p + 12) & ~7u) >= 16) p = a + 8;
    while (p != top && n < 400000) {
        uint32_t sz = psp_l32(p + 4) & ~7u;
        if (sz < 16) { *why = sz ? "tamano menor que 16" : "tamano cero"; return p; }
        if (p + sz > top) { *why = "pasa del tope"; return p; }
        if (!(psp_l32(p + sz + 4) & 1) && psp_l32(p + sz) != sz) { *why = "pie del bloque libre distinto"; return p; }
        p += sz; n++;
    }
    if (p != top) { *why = "no llega al tope"; return p; }
    if (top + (psp_l32(top + 4) & ~7u) > e) { *why = "tope pasa del final"; return top; }
    for (uint32_t i = 1; i < 128; i++) {   // free lists: links both ways, chunks free and inside the heap
        uint32_t bin = NL_AV + 8 * i, prev = bin, q = psp_l32(bin + 8), k = 0;
        while (q != bin) {
            if (q < a || q >= top || (q & 7) || ++k > 200000) { *why = k > 200000 ? "lista de libres en bucle" : "enlace de lista fuera del monton"; return q ? q : bin; }
            if (psp_l32(q + 12) != prev) { *why = "enlace de lista roto"; return q; }
            uint32_t sz = psp_l32(q + 4) & ~7u;
            if (sz < 16 || q + sz > top || (psp_l32(q + sz + 4) & 1)) { *why = "bloque en lista de libres marcado en uso"; return q; }
            prev = q; q = psp_l32(q + 8);
        }
    }
    return 0;
}
static int hw_on() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_HEAP_CHECK"); v = e && *e ? atoi(e) : 0; } return v; }
static uint32_t hw_last = 0, hw_ra = 0, hw_a0 = 0, hw_lra = 0, hw_la0 = 0;
static bool hw_broken = false;
static void hw_mark(uint32_t dir, bool after) {
    if (hw_broken || hw_on() != 2 || !cv.on) return;
    const char* why = nullptr;
    uint32_t bad = hw_walk(&why);
    if (!bad) {
        static bool said = false;
        if (!said) { said = true; printf("heap: localizador activo, tope 0x%08X\n", psp_l32(NL_AV + 8)); fflush(stdout); }
        hw_last = after ? dir | 1u : dir; hw_lra = hw_ra; hw_la0 = hw_a0; return;
    }
    hw_broken = true;
    printf("heap: roto en 0x%08X (%s, cabecera %08X %08X) %s 0x%08X; ultimo punto sano: %s 0x%08X; cuadro %u, hilo %d\n", bad, why, psp_l32(bad), psp_l32(bad + 4),
           after ? "al salir de" : "al entrar en", dir, hw_last & 1u ? "salida de" : "entrada en", hw_last & ~1u, cv_frame, psp_thread_current());
    printf("heap:   llamada desde 0x%08X con a0 0x%08X; punto sano desde 0x%08X con a0 0x%08X; tope 0x%08X\n", hw_ra - 8, hw_a0, hw_lra - 8, hw_la0, psp_l32(NL_AV + 8));
    printf("heap:   visor: obj %d/%d, buf 0x%08X/0x%08X, cur %d, pend %d\n", cv.obj[0], cv.obj[1], cv.buf[0], cv.buf[1], cv.cur, cv.pend);
    psp_hle_recent(8);
    fflush(stdout);
}
// item 109: objects whose animation starts without a speed setter (supports, extra characters) keep the creation default 2.0
// frames per 30 Hz tick (0x089B02E4); at 60 the battle steps them every 60 Hz tick
static void sup_play(Ctx& c, void (*orig)(Ctx&)) {
    uint32_t o = c.r[4], ra = c.r[31], n = c.r[5];
    orig(c);
    if (!patch_support_anim60.val || !psp_fps60() || !battle_active() || o < 0x08800000u || o >= 0x10000000u) return;
    float sp = lf(o + 0xB60 + 0x140);
    bool fix = sp == 2.0f;
    if (fix) ef(o + 0xB60 + 0x140, 1.0f);
    static uint32_t seen[16]; static int ns = 0;
    uint32_t key = ra ^ (fix ? 0x80000000u : 0);
    for (int i = 0; i < ns; i++) if (seen[i] == key) return;
    if (ns < 16) { seen[ns++] = key; printf("anim: objeto 0x%08X animacion %u desde 0x%08X velocidad %.3f%s\n", o, n, ra, sp, fix ? " -> 1.000 (60 fps)" : " sin cambio"); }
}
// night moon: the transformation effect kind 5 (BT3 EftTransform, "Night Only") draws the stage sprite (pak entry 0x14);
// its loader 0x08886EEC runs at stage init behind the stage-ready gate 0x089B721C and skips it there; run it again in battle
static void moon_load(Ctx& c, const char* where) {
    uint32_t sf = psp_l32(0x08A7BDE8u), st = psp_l32(0x08A7F280u), pk = st ? psp_l32(st + 0xC) : 0;
    uint32_t fl = sf ? psp_l32(sf + 8) : 0, gate = mpm_call(c, 0x089B721Cu);
    int n = pk ? (int)mpm_call(c, 0x0887BB8Cu, pk, 0x14) : 0;
    bool run = patch_stage_moon.val && sf && pk && gate && !(fl & 4);
    if (run) mpm_call(c, 0x08886EECu);
    static int logs = 0;
    if (logs++ < 8) printf("luna: %s, efecto 0x%08X bits %X -> %X, escenario 0x%08X pak 0x%08X entrada 0x14 %d bytes, escenario listo %u, cargador %s, textura 0x%08X 0x%08X\n",
        where, sf, fl, sf ? psp_l32(sf + 8) : 0, st, pk, n, gate, run ? "si" : "no", sf ? psp_l32(sf + 0x10) : 0, sf ? psp_l32(sf + 0x14) : 0);
}
static void moon_start(Ctx& c, void (*orig)(Ctx&)) {
    uint32_t rq = c.r[4], k = rq ? psp_l32(rq + 4) : 0;
    if (k == 5) moon_load(c, "inicio de la transformacion");
    orig(c);
}
static void moon_draw(Ctx& c, void (*orig)(Ctx&)) {
    uint32_t sf = psp_l32(0x08A7BDE8u);
    if (sf && !(psp_l32(sf + 8) & 4)) moon_load(c, "dibujo");
    orig(c);
}
// artificial moon: TTT's kind->set table (0x088F5600) gives kind 6 no emitter set; a mod's Transform/artificial_moon.pak (BT3 resident
// set 0x1C1 as a TTT effect pak) is parsed like the transformation sets (0x0889BB64 from a pak, index 1), ticked and freed with the manager
struct TfMoon { uint32_t mgr = 0, S = 0, pak = 0; bool on = false; };
static TfMoon tf_moon;
static uint32_t tfm_get(Ctx& c) {
    uint32_t mgr = psp_l32(0x08A7D3E0u);
    if (!patch_transform_moon.val || !mgr) return 0;
    if (tf_moon.on && tf_moon.mgr == mgr) return tf_moon.S;
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::string f = psp_cr_file("Transform/artificial_moon.pak");
        tf_moon.pak = f.empty() ? 0 : ch_load(f, "Transform/artificial_moon.pak");
        if (!tf_moon.pak) printf("luna artificial: ningun mod activo tiene Transform/artificial_moon.pak\n");
    }
    if (!tf_moon.pak) return 0;
    if (!tf_moon.S) tf_moon.S = psp_reserve(0x340);
    memset(psp_p(tf_moon.S), 0, 0x324);
    psp_s32(tf_moon.S + 0x320, tf_moon.pak);
    mpm_call(c, 0x0889BB64u, 0, tf_moon.S, 0, tf_moon.pak, 2, 1);
    tf_moon.mgr = mgr; tf_moon.on = true;
    uint32_t h = psp_l32(tf_moon.S);
    printf("luna artificial: efecto del mod listo (cabecera 0x%08X, grupos %u, partes %u)\n", h, h ? psp_l8(h + 4) : 0, h ? psp_l8(h + 5) : 0);
    return tf_moon.S;
}
static bool tfm_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    if (dir == 0x088F5600u) {
        uint32_t k = c.r[4];
        orig(c);
        if (k == 6 && !c.r[2]) c.r[2] = tfm_get(c);
        return true;
    }
    if (dir == 0x088F70ECu) { orig(c); if (tf_moon.on && tf_moon.mgr == psp_l32(0x08A7D3E0u)) mpm_call(c, 0x08897458u, tf_moon.S, tf_moon.pak); return true; }
    if (dir == 0x088F7080u) { if (tf_moon.on) mpm_call(c, 0x088973B0u, tf_moon.S, tf_moon.pak); tf_moon.on = false; orig(c); return true; }
    return false;
}
// CMAN in pre-animations: the camera cut of a technique (0x0896F3D8: class 2..4 -> attack 1..3, 0..1 -> skill), of the ultimate's start
// (cut 0/3) and of a cinematic skill (cut 1/5) is replaced by the character's pre_cman file played on cman 0 like the event cameras
// (0x089AAC30), relative to where the fighter was at the start, stopping at its end or when the fighter leaves the action
std::string psp_pre_cman(uint32_t pak, const std::string& name, std::string* where);
static const char* const PC_NAME[6] = {"attack_1.cma", "attack_2.cma", "attack_3.cma", "attack_3_start.cma", "skill_1.cma", "skill_2.cma"};
struct PreCam { uint32_t cma = 0, act = 0; };
static PreCam pc_on[8];
static uint32_t pc_file(const std::string& f) {
    static std::map<std::string, uint32_t> files;
    auto it = files.find(f);
    if (it != files.end()) return it->second;
    uint32_t g = 0;
    if (FILE* fp = fopen(f.c_str(), "rb")) {
        std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0) b.insert(b.end(), buf, buf + n);
        fclose(fp);
        if (b.size() >= 0x20) { g = psp_reserve((uint32_t)(b.size() + 63) & ~63u); memcpy(psp_p(g), b.data(), b.size()); }
    }
    files[f] = g;
    return g;
}
static bool pc_cut(Ctx& c, void (*orig)(Ctx&)) {   // 0x0891A464(combatant, table, cut)
    uint32_t cb = c.r[4], tb = c.r[5], ix = c.r[6], ra = c.r[31];
    int k = -1;
    if (!patch_pre_cman.val || !cb) return false;
    if (ra == 0x0896F42Cu && tb == 0) { uint32_t cl = mpm_call(c, 0x08948CB8u, cb); k = cl >= 2 && cl <= 4 ? (int)cl - 2 : cl <= 1 ? 4 + (int)cl : -1; }
    else if (ra == 0x089721B0u && tb == 0 && ix == 3) k = 3;
    else if (ra == 0x089823D8u && tb == 1 && ix == 5) { uint32_t cl = mpm_call(c, 0x08948CB8u, cb); k = cl <= 1 ? 4 + (int)cl : -1; }
    if (k < 0) return false;
    uint32_t slot = psp_l32(cb), ent = slot < 8 ? psp_l32(0x08B61178u + 4u * slot) : 0, hdr = ent && ent < 0x10000000u ? psp_l32(ent + 0x14) : 0;
    uint32_t pak = hdr && hdr < 0x10000000u && psp_l32(hdr) == 0x6C644D70u ? lip_pak(hdr) : 0;
    std::string where, f = pak ? psp_pre_cman(pak, PC_NAME[k], &where) : "";
    uint32_t cma = f.empty() ? 0 : pc_file(f);
    static std::set<std::string> seen;
    if (seen.insert(where + "|" + PC_NAME[k]).second && seen.size() < 64)
        printf("pre_cman: luchador %u, %s (accion 0x%X, corte %u/%u): %s\n", slot, PC_NAME[k], psp_l32(cb + 0x984), tb, ix, cma ? f.c_str() : f.empty() ? (pak ? ("sin archivo (o vacio) en " + where).c_str() : "sin pak del personaje") : "archivo vacio o roto");
    if (!cma) return false;
    mpm_call(c, 0x089AA460u, cma, 0);
    mpm_call(c, 0x089AA9ECu, ent, 0);
    mpm_callf(c, 0x089AA57Cu, 0, 0, 0.0f);
    mpm_call(c, 0x089AA8E4u, 0);
    uint32_t dm = psp_l32(0x08A7E248u);
    if (dm) psp_s32(dm + 0x7E8, psp_l32(dm + 0x7E8) | 0x18);   // 8: ends at its end, 0x10: the fighter's matrix frozen at the start (0x089AB094)
    if (slot < 8) { pc_on[slot].cma = cma; pc_on[slot].act = psp_l32(cb + 0x984); }
    return true;
}
static void pc_tick(Ctx& c, uint32_t cb) {   // 0x08919EB4(combatant), per fighter and frame
    uint32_t slot = cb ? psp_l32(cb) : 99;
    if (slot >= 8 || !pc_on[slot].cma) return;
    uint32_t dm = psp_l32(0x08A7E248u);
    if (!dm || psp_l32(dm + 0x7A0) != pc_on[slot].cma || !(psp_l32(dm + 0x7E8) & 1)) { pc_on[slot] = PreCam(); return; }
    if (psp_l32(cb + 0x984) != pc_on[slot].act) { mpm_call(c, 0x089AA53Cu, 0); pc_on[slot] = PreCam(); }
}
static void psp_wrap_body(uint32_t dir, Ctx& c, void (*orig)(Ctx&));
// game message boxes (0x08A2EBFC class): progress boxes of the savedata ("... No apagues el sistema PSP") are never opened, so
// the game's waits on "closed" resolve at once (EBOOT_TTT 23.2); the box text is recorded by the set-messages hook 0x08A2F7CC
static std::map<uint32_t, uint32_t> box_id;
static std::set<uint32_t> box_skip;
static void box_note(uint32_t box, uint32_t slots) { if (box && slots) box_id[box] = psp_l32(slots + 4); }

static bool box_progress(const Ctx& c, uint32_t id) {
    uint32_t t = mpm_call(c, 0x0883EE74u, id, 0xFFFFFFFFu);
    if (!t || t < 0x08800000u || t >= 0x0A000000u) return false;
    std::string a;
    for (int k = 0; k < 400; k++) { uint16_t ch = psp_l16(t + 2 * k); if (!ch) break; a += ch < 0x80 ? (char)tolower(ch) : '?'; }
    return a.find("apagues") != std::string::npos || a.find("turn off") != std::string::npos || a.find("teignez") != std::string::npos;
}

static bool box_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    uint32_t box = c.r[4];
    auto it = box_id.find(box);
    if (it == box_id.end()) return false;
    if (dir == 0x089D161Cu) {
        if (!box_skip.count(box)) return false;
        box_skip.erase(box);
        return true;
    }
    if (!box_progress(c, it->second)) { box_skip.erase(box); return false; }
    box_skip.insert(box);
    static int logs = 0;
    if (logs++ < 30) printf("caja de mensaje: aviso de guardado %u omitido (desde 0x%08X)\n", it->second, c.r[31] - 8);
    return true;
}

// TTT_OBJ_LOG=1: object create / destroy with the pak handed in (model 002, textures 011) and the caller
static bool obj_log_on() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_OBJ_LOG"); v = e && *e == '1'; } return v == 1; }
static uint32_t obj_be32(uint32_t a) { uint32_t v = psp_l32(a); return v >> 24 | (v >> 8 & 0xFF00u) | (v << 8 & 0xFF0000u) | v << 24; }
static void obj_pak(char* m, size_t n, uint32_t pak) {   // a model pak as the object loader reads it (entries 3 and 0xC, 1-based)
    uint32_t cnt = obj_be32(pak), o3 = obj_be32(pak + 12), z3 = obj_be32(pak + 16) - o3, oc = obj_be32(pak + 48), zc = obj_be32(pak + 52) - oc;
    uint32_t e3 = pak + o3, ec = pak + oc, w = psp_l32(ec + 0x48);
    snprintf(m, n, "pak 0x%08X: %u entradas, 002 desp %u tam %u firma %02X%02X%02X%02X huesos %u, 011 desp %u tam %u texturas %u tex0 %ux%u formato %u",
             pak, cnt, o3, z3, psp_l8(e3), psp_l8(e3 + 1), psp_l8(e3 + 2), psp_l8(e3 + 3), psp_l32(e3 + 8), oc, zc, psp_l32(ec), 1u << (w >> 8 & 15), 1u << (w & 15), w >> 24);
}
static void obj_log(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    static int logs = 0;
    static std::map<uint64_t, int> rep;
    uint32_t a0 = c.r[4], rec = c.r[5], ra = c.r[31], up = psp_l32(c.r[29] + (ra == 0x088FF92Cu ? 8u : 0u));
    uint32_t base = psp_l32(0x08A7E278u);
    char m[320] = "";
    if (dir == 0x088A0584u) {   // class 6 owner init: a0 owner task (data +0x38), a1 params (+0x20 skill pak)
        uint32_t ow = psp_l32(a0 + 0x38), sk = psp_l32(rec + 0x20);
        printf("obj: clase 6 inicio: pak de tecnica 0x%08X, %u entradas, 00 desp %u tam %u, 03 desp %u tam %u\n", sk, obj_be32(sk), obj_be32(sk + 4), obj_be32(sk + 8) - obj_be32(sk + 4),
               obj_be32(sk + 16), obj_be32(sk + 20) - obj_be32(sk + 16));
        orig(c);
        uint32_t pk = psp_l32(ow + 0x324);
        obj_pak(m, sizeof m, pk);
        printf("obj: clase 6 tras cargar: dueno 0x%08X, +0x324 = 0x%08X (esperado 0x%08X); %s\n", ow, pk, sk + (obj_be32(sk + 4) & ~3u), m);
        fflush(stdout);
        if (psp_watch_nodes) psp_watch_node(0x088A0634u, a0);   // arm the write watch before anything else runs
        return;
    }
    if (dir == 0x089AE1FCu) {
        uint32_t obj = base + 0x16E0u * a0 + 0x10u;
        int live = (int)psp_l32(obj + 0xC), n = ++rep[(uint64_t)a0 << 32 | up];
        orig(c);
        if (n <= 3 || n == 1000) { printf("obj: destruir id %d (ra 0x%08X, desde 0x%08X, vez %d), objeto con id %d\n", (int)a0, ra, up, n, live); fflush(stdout); }
        return;
    }
    uint32_t pak = psp_l32(rec);
    if (a0 == 1 && pak) obj_pak(m, sizeof m, pak);
    if (up == 0x0889F8F8u) {   // class 6 model setup: node task in s3; owner = [[node + 0x20]] as 0x0889F728 reads it
        uint32_t node = c.r[19], P = psp_l32(node + 0x20), T = psp_l32(P), dat = psp_l32(T + 0x38), cls = psp_l32(T + 0x28);
        printf("obj: clase 6 modelo: nodo 0x%08X (clase 0x%08X), +0x20 = 0x%08X, tarea duena 0x%08X (clase 0x%08X, update 0x%08X), datos 0x%08X, +0x324 = 0x%08X, s1 en pila 0x%08X\n",
               node, psp_l32(psp_l32(node + 0x28)), P, T, cls, psp_l32(cls), dat, psp_l32(dat + 0x324), psp_l32(c.r[29] + 4));
        printf("obj:   datos del dueno:");
        for (uint32_t k = 0; k < 8; k++) printf(" %08X", psp_l32(dat + 4 * k));
        printf(" ... +0x310:");
        for (uint32_t k = 0; k < 8; k++) printf(" %08X", psp_l32(dat + 0x310 + 4 * k));
        printf("\n");
    }
    orig(c);
    if (logs++ >= 60) return;
    int id = (int)c.r[2];
    uint32_t obj = id >= 0 ? base + 0x16E0u * (uint32_t)id + 0x10u : 0, pm = obj ? psp_l32(obj + 0x14) : 0;
    printf("obj: crear tipo %u -> id %d (ra 0x%08X, desde 0x%08X), registro 0x%08X; %s; pMdl del objeto 0x%08X\n", a0, id, ra, up, rec, m, pm);
    fflush(stdout);
}

void psp_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    if (hw_on() != 2) { psp_wrap_body(dir, c, orig); return; }
    uint32_t ra = c.r[31], a0 = c.r[4];
    hw_ra = ra; hw_a0 = a0;
    hw_mark(dir, false);
    psp_wrap_body(dir, c, orig);
    hw_ra = ra; hw_a0 = a0;
    hw_mark(dir, true);
}
static void psp_wrap_body(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    if (dir == 0x089C34ACu && c.r[5] == 0 && (rm_on || cv.on)) {   // battle scene requested: the 3D views share the battle's resource record (map buffer +0xC), they close before the battle sets it up
        printf("mapas: escena de batalla pedida con la vista 3D %s abierta, se cierra ahora\n", rm_on ? "de la sala" : "del visor");
        if (rm_on) rm_close(c);
        if (cv.on) cv_term(c);
    }
    if (dir == 0x089A6F28u) {   // stage collision BVH descent (node = a0 + 32 * a2, children +0x18/+0x1C): a cycle in the map data recursed until the stack overflowed
        static int depth = 0, logs = 0;
        if (depth >= 96) {
            if (logs++ < 3) {
                uint32_t n = c.r[4] + 32u * c.r[6], pool = psp_l32(0x08A7E220u);
                printf("colision: arbol del mapa con ciclo, consulta cortada (nodos 0x%08X, nodo %d en 0x%08X = base del mapa 0x%08X + 0x%X, hijos %d %d)\n",
                       c.r[4], (int)c.r[6], n, pool, n - pool, (int)psp_l32(n + 0x18), (int)psp_l32(n + 0x1C));
                fflush(stdout);
            }
            return;
        }
        depth++; orig(c); depth--;
        return;
    }
    if (dir == 0x08A82B1Cu) { boot_credits(c, orig); return; }
    if ((dir == 0x089BFD60u || dir == 0x089BFDB0u) && hs_wrap(dir, c, orig)) return;
    if ((dir == 0x088FE410u || dir == 0x088FE4E8u || dir == 0x088FE3BCu) && ar_wrap(dir, c, orig)) return;
    if ((dir == 0x089AE0A4u || dir == 0x089AE1FCu || dir == 0x088A0584u) && obj_log_on()) { obj_log(dir, c, orig); return; }
    if (dir == 0x0880E0C8u) { orig(c); if (patch_bone_room.val && c.r[2] == 0x7700u) c.r[2] = 0xBC00u; return; }   // table 1 at +0x3BC0 fits bone ids up to 255
    AwScope aw_scope(dir);
    if (au_wrap(dir, c, orig)) return;
    if ((dir == 0x089BFDF4u || dir == 0x0883EE74u || dir == 0x08830274u || dir == 0x089C34ACu || (dir >= 0x08A81DC0u && dir <= 0x08A836F0u)) && cr_menu_wrap(dir, c, orig)) return;
    if ((dir == 0x08A06BB8u || dir == 0x089FC394u || dir == 0x089FCC08u) && cs_wrap(dir, c, orig)) return;
    if (dir == 0x08A0A0A0u) { sm_cursor(c, orig); return; }
    if (dir == 0x0881A1A0u) { s3d_a1a0_vb = psp_l32(0x08A78E14u); wt_tick(); ch_tick(c); mpm_frame(c); cr_frame(c); s3d_tick(c); m3d_tick(c); rm_tick(c); vw_tick(c); ip_trace(c); }
    if (dir == 0x0880BBF4u) { cv_draw3d(c); s3d_draw(c); m3d_draw(c); rm_draw(c); orig(c); return; }
    if (dir == 0x089D1B94u && c.r[31] == 0x089F228Cu && bh_tw_band(c, orig)) return;   // technique window band (BT3 HUD)
    if (dir == 0x089D1B94u && (c.r[31] == 0x089D8C14u || c.r[31] == 0x089D8CCCu) && sz_boost_inst(c, orig)) return;   // power-up title: SZ BBoost notice
    if (dir == 0x089D1B94u) { if (!s3d_inst(c) && !m3d_inst(c)) orig(c); return; }
    if (dir == 0x089FF8C8u) { s3d_form_draw(c, orig); return; }
    if (dir == 0x089F9F34u) { if (!sm_draw(c)) s3d_grid_draw(c, orig); return; }
    if (dir == 0x08832448u && c.r[31] == 0x088532CCu) { sub_depth(c, orig); return; }
    if (dir == 0x08834518u && c.r[31] == 0x088532F0u) { sub_text(c, orig); return; }
    if (dir == 0x088404D8u && c.r[31] == 0x089F8D08u && s3d_pair(c, orig)) return;
    if (dir == 0x08839268u && ac_boot_wrap(c, orig)) return;
    if (dir == 0x0881A140u) {
        ld_tick(); psp_disc_reload_poll(ld_on || net_online_on());
        if (psp_l32(0x08A78E14u) - s3d_a1a0_vb > 2) { cv_tick(c); s3d_tick(c); m3d_tick(c); rm_tick(c); vw_tick(c); }   // net session (scene 8, mode 8 in its select): the main loop skips 0x0881A1A0, the 3D views tick from the local pad read
    }
    if (dir == 0x088566C4u) { rs_result(c); orig(c); return; }
    if ((dir == 0x0881A140u || dir == 0x08905008u) && ws_wrap(dir, c, orig)) return;
    if ((dir == 0x088174ACu || dir == 0x088174FCu || dir == 0x08816E24u) && ld_wrap(dir, c, orig)) return;
    if (dir == 0x0883EE74u && mv_wrap(c, orig)) return;
    if (dir == 0x0883EE74u && mpn_wrap(dir, c, orig)) return;
    if (dir == 0x0882FFA4u || dir == 0x08830274u) mpm_pak(dir, c);
    if (dir == 0x088404D8u && ro_bind(c, orig)) return;
    if (costume_wrap(dir, c, orig)) return;
    if (dir == 0x088553F0u && tw_wait(c)) { c.r[2] = 0; return; }
    if (dir == 0x0893A10Cu) { sv_wrap(c, orig); return; }
    if (dir == 0x089167E0u) { mc_wrap(c, orig); return; }
    if ((dir == 0x08A03C0Cu || dir == 0x08A03C9Cu || dir == 0x08A07170u || dir == 0x08858764u) && st_wrap(dir, c, orig)) return;
    if (dir == 0x0880BD3Cu) {
        ly_scratch_ensure();
        hud_back_draw(c);
        uint32_t ra = c.r[31];
        orig(c);
        static int logs = 0;
        if (ra == 0x0885AD7Cu) {   // BT3 stage surfaces after the fighters (GPU marker F5)
            uint32_t mask = psp_l32(0x08B47888u + 0x44);   // a render packet of its own, queued after the fighters' packets (like the bars)
            hud_guest(c, 0x088403B8u, 0x08B48200u);
            uint32_t list = hud_guest(c, 0x08838054u, 0);
            uint32_t gc = psp_l32(0x08B478D0u), p = gc ? psp_l32(gc + 8) : 0;
            if (list && p) { psp_s32(p, 0x00F50000u); psp_s32(gc + 8, p + 4); }
            if (list) { hud_guest(c, 0x08838120u, list); hud_guest(c, 0x08837F54u, 0x08B4E690u, list, 0); }
            psp_s32(0x08B47888u + 0x44, mask);
            hud_guest(c, 0x088403D4u, 0x08B48200u);
            if (logs < 4) { logs++; printf("agua: marca F5 tras los luchadores, paquete 0x%08X, lista 0x%08X\n", list, p); }
        } else if (logs < 8) { logs++; printf("agua: dibujo de objetos llamado desde 0x%08X\n", ra); }
        return;
    }
    if (dir == 0x0898F130u) { bc_wrap(c, orig); return; }
    if (dir == 0x08A642A8u) { bc_proj(c, orig); return; }
    if (dir == 0x0898F22Cu) { bc_prio(c, orig); return; }
    if (dir == 0x08931BE4u) dbg_freeze(c.r[4]);
    if (lmp_wrap(dir, c, orig)) return;
    if ((dir == 0x089C34ACu || dir == 0x089C4214u) && mpm_wrap(dir, c, orig)) return;
    if ((dir == 0x08A0758Cu || dir == 0x08A07AE0u || dir == 0x08A07D18u || dir == 0x08A07284u) && mo_opt_wrap(dir, c, orig)) return;
    if (dir == 0x08A064B0u && mo_row_wrap(c, orig)) return;
    if ((dir == 0x089D15F0u || dir == 0x089D161Cu) && box_wrap(dir, c, orig)) return;
    if (dir >= 0x088404D8u && dir <= 0x089F6474u && hud_wrap(dir, c, orig)) return;
    if (dir == 0x08A642D4u && lmp_tflip) lmp_text_flip(c);
    if (dir == 0x08A642D4u && cl_gu(c, orig)) return;
    if (dir == 0x0880E480u && sh_wrap(c, orig)) return;
    if (dir == 0x088B540Cu) { cel_aura(c); ch_aura(c, orig); return; }
    if (dir == 0x088B446Cu) { ch_aura_spawn(c, orig); return; }
    if (dir == 0x089B0278u) { ch_anim(c, orig); return; }
    if (dir == 0x089B14C8u) { sup_play(c, orig); return; }
    if (dir == 0x088F7140u) { moon_start(c, orig); return; }
    if (dir == 0x088871A4u) { moon_draw(c, orig); return; }
    if (dir == 0x0891A464u && pc_cut(c, orig)) return;
    if (dir == 0x08919EB4u) { pc_tick(c, c.r[4]); ro_fighter(c.r[4]); }
    if ((dir == 0x088F5600u || dir == 0x088F70ECu || dir == 0x088F7080u) && tfm_wrap(dir, c, orig)) return;
    if (dir == 0x088C95BCu) { ch_a_create(c, orig); return; }
    if (dir == 0x088C958Cu || dir == 0x088C9534u) { ch_a_loader(dir, c, orig); return; }
    if (dir == 0x0880CAF4u) { s3d_body(c.r[4]); LipSave ls; lip_begin(c, ls); if (!cel_wrap(c, orig)) orig(c); lip_end(ls); return; }
    if (dir == 0x088B5CD0u) { cel_aura_draw(c, orig); return; }
    if (dir == 0x0880BB28u) { wat_wrap(c, orig); return; }
    if (dir == 0x08817100u || dir == 0x08817390u || dir == 0x088134ECu) {   // network: phases, errors and stopwatch of the WLAN session
        uint32_t a0 = c.r[4], a1 = c.r[5], ra = c.r[31] - 8;
        uint64_t begin_v = 0, ya = 0, tope = (uint64_t)c.r[7] << 32 | c.r[6];
        if (dir == 0x088134ECu) { begin_v = (uint64_t)psp_l32(a0 + 4) << 32 | psp_l32(a0); ya = (uint64_t)psp_l32(a0 + 0xC) << 32 | psp_l32(a0 + 8); }
        orig(c);
        if (dir == 0x08817100u) ws_phase = a1;
        if (dir == 0x08817100u && (a1 < 10 || a1 == 17 || a1 == 18)) { ld_end(); if (a1 < 10) { rs_run_end("fase de red"); rs_waves = 0; rs.both = false; rs.bdown = 0; } ld_used = 0; ld_next_d = 0; psp_net_delay_reset(); }
        if (dir == 0x08817100u && a1 == 12 && patch_net_delay.val) ld_pick();
        static uint32_t ws_out = 0;
        if (dir == 0x08817100u && (a1 == 13 || a1 == 14) && ws_out) printf("net: lockstep tras %u vblanks fuera de el\n", psp_l32(0x08A78E14u) - ws_out);
        if (dir == 0x08817100u) ws_out = a1 == 15 || a1 == 16 ? (ws_out ? ws_out : psp_l32(0x08A78E14u)) : a1 == 13 || a1 == 14 ? 0 : ws_out;
        if (dir == 0x08817100u && a1 == 18) rand_dump(a1);
        static int n = 0;
        if (n >= 200) return;
        double t = psp_time_us() / 1000000.0;
        if (dir == 0x08817100u) { n++; printf("net: fase %u desde 0x%08X, vblank %u, %.2f s\n", a1, ra, psp_l32(0x08A78E14u), t); }
        else if (dir == 0x08817390u) { n++; printf("net: fallo de sesion desde 0x%08X, vblank %u, %.2f s\n", ra, psp_l32(0x08A78E14u), t); psp_net_dump(); }
        else if (c.r[2]) { n++; printf("net: cronometro vencido desde 0x%08X: inicio %.2f s, ahora %.2f s, tope %.2f s, vblank %u\n", ra, begin_v / 1e6, ya / 1e6, tope / 1e6, psp_l32(0x08A78E14u)); }
        return;
    }
    if (dir == 0x089AECE0u) {                 // 53: entity +0xA60 bit 0x02000000 (mini camera) turns on the camera-inside-box test that draws the
        static int n = 0, on = 0;             // fighter as a flat silhouette (render flag 0x200000); TTT_CAM_SILHOUETTE=1 keeps it
        if ((n++ & 255) == 0) { const char* e = getenv("TTT_CAM_SILHOUETTE"); on = e && *e ? atoi(e) : 0; }
        uint32_t a = c.r[4] + 0xA60, v = c.r[4] && c.r[4] < 0x10000000u ? psp_l32(a) : 0;
        if (on || !(v & 0x02000000u)) { orig(c); return; }
        psp_s32(a, v & ~0x02000000u);
        orig(c);
        psp_s32(a, psp_l32(a) | 0x02000000u);
        return;
    }
    if (dir == 0x0880740Cu) {                 // 54/86: animated map parts (group 7) add speed x 0.1 to their UV phase (+0x1C) on every draw;
        static int on = -1;                   // the phase steps at most once per 2 vblanks (original 30 fps); TTT_MAP_UV30=0 keeps a step per draw
        static uint32_t last_vb = 0;
        if (on < 0) { const char* e = getenv("TTT_MAP_UV30"); on = !(e && *e == '0'); }
        uint32_t vb = psp_l32(0x08A78E14u), g = psp_l32(0x08A7E678u), lst = g && g < 0x10000000u ? psp_l32(g + 0x24) : 0;
        if (!on || !lst || lst >= 0x10000000u || vb - last_vb >= 2u) { last_vb = vb; orig(c); return; }
        static uint32_t pa[4096], pv[4096];
        int n = 0;
        for (uint32_t o = 0x30; o <= 0x38; o += 8) {
            uint32_t cnt = psp_l32(lst + o), arr = psp_l32(lst + o + 4);
            if (!arr || arr >= 0x10000000u || cnt > 4096u) continue;
            for (uint32_t i = 0; i < cnt && n < 4096; i++) {
                uint32_t pt = psp_l32(arr + 4 * i);
                if (pt && pt < 0x10000000u) { pa[n] = pt + 0x1C; pv[n++] = psp_l32(pt + 0x1C); }
            }
        }
        orig(c);
        for (int i = 0; i < n; i++) psp_s32(pa[i], pv[i]);
        return;
    }
    if (dir == 0x089B303Cu) {                 // 53: pulsing team glow (material ambient, flag 0xDDD via 0x0892C5E0); TTT_TEAM_GLOW=1 keeps it
        static int n = 0, on = 0;
        if ((n++ & 255) == 0) { const char* e = getenv("TTT_TEAM_GLOW"); on = e && *e ? atoi(e) : 0; }
        if (on) orig(c); else c.r[2] = 0;
        return;
    }
    if (dir == 0x0892424Cu) { uint32_t v = c.r[4], info = c.r[6]; orig(c); be_hit(v, info); return; }   // 52b: hit receive
    if (dir == 0x08894ACCu) { if (be_on() && be_skip(c.r[4])) return; orig(c); return; }               // 52b: attacker release
    if (dir == 0x0893CECCu) { if (wt_dash_sub(c)) return; orig(c); return; }   // BtlCharSnd_Request
    if (dir == 0x0888C56Cu) { if (wt_piece(c)) return; orig(c); return; }   // EftWater_AddSplashAt
    if (dir == 0x088C9EF8u) {                 // ki blast carrier at real 60: life countdown +0x5A4 steps on even ticks only
        uint32_t d = psp_l32(c.r[4] + 0x38), life = psp_l32(d + 0x5A4);
        orig(c);
        if (battle60() && class60(dir) && tick_odd && psp_l32(d + 0x5A4) == life - 1u) psp_s32(d + 0x5A4, life);
        return;
    }
    if (dir == 0x089C044Cu) {
        uint32_t site = c.r[31] - 8;
        if (site >= 0x089B32F8u && site <= 0x089B3BE0u) {   // face ctrl (blink, expressions): visual only, and the port's 3D views run it on one machine
            static uint32_t x = 0x9E3779B9u;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            c.r[2] = c.r[4] ? x % c.r[4] : 0;
            return;
        }
        if (!rand_private) { rand_note(site); ws_rand_rec(site); }
        orig(c);
        return;
    }
    if (dir == 0x089AD480u) { shake_private(c, orig); return; }
    if (dir == 0x088155C0u || dir == 0x08814674u) {   // WLAN sync check: compares +4 (rand count) of each player
        uint32_t tab = c.r[5], n = c.r[6];
        rs_slot = (int32_t)hud_guest(c, 0x0885862Cu, 0);
        ws_quit_tick(c);
        ws_state(psp_l32(c.r[4] + 0x10));
        orig(c);
        if (c.r[2]) return;
        static int strict = -1; if (strict < 0) { const char* e = getenv("TTT_SYNC"); strict = e ? atoi(e) : 1; }
        static int times = 0;
        if (times < 40) {
            times++;
            printf("net: sincronia distinta, tick %u, vblank %u:", tick_local, psp_l32(0x08A78E14u));
            for (uint32_t i = 0; i < n && i < 5; i++) { uint32_t e = psp_l32(tab + 4 * i); printf(" %u", e ? psp_l32(e + 4) : 0xFFFFFFFFu); }
            printf("%s\n", strict ? "" : " (se ignora)");
            rand_dump(0);
        }
        if (!strict) c.r[2] = 1;
        return;
    }
    if (dir == 0x0881548Cu) {                 // phase 14: player inputs for frame obj+0x10
        uint32_t o = c.r[4], cuadro = psp_l32(o + 0x10);
        orig(c);
        static int n = 0, missing = 0;
        bool ok = c.r[2] != 0;
        if (n < 60 && (!ok || missing)) { n++; printf("net: cuadro %u %s, vblank %u, %.2f s\n", cuadro, ok ? "completo" : "sin entradas de algun jugador", psp_l32(0x08A78E14u), psp_time_us() / 1e6); }
        missing = !ok;
        return;
    }
    if (dir == 0x08930378u) { aura_bit(c, orig); return; }
    if (dir == 0x089477B4u) {                 // TTT_FLOAT=0: no floating bob in the air (+0x64 = 2 sin(phase))
        uint32_t cb = c.r[4];
        orig(c);
        const char* e = getenv("TTT_FLOAT");
        if (e && *e == '0') psp_s32(cb + 0x64u, 0);
        return;
    }
    if (dir == 0x08916904u) {                 // TTT_FLOAT=0: the camera height offset 1.5 sin(phase) in the air returns 0
        orig(c);
        const char* e = getenv("TTT_FLOAT");
        if (e && *e == '0') c.f[0].u = 0;
        return;
    }
    if (dir == 0x08901E10u) { walker_vb = psp_l32(0x08A78E14u); prompt_pad_tick(); dbg_ko_tick(); if (nt_depth == 0) be_tick(c); }
    if (!battle60()) { if (dir == 0x08901E10u) { nt_sample(c.r[4]); st_walk30(); nt_depth++; orig(c); nt_depth--; return; } orig(c); return; }
    if (dir == 0x08901E10u || dir == 0x08901ECCu) { tasks(c, dir); return; }
    if (dir == 0x089B1A58u) {                 // animation events: with animation at half a frame per tick, the tick that does not cross
        uint32_t ent = c.r[4], anim = ent + 0xB60u;   // a whole frame would repeat the current frame's events; the block is left empty
        static int once_v = -1; if (once_v < 0) { const char* e = getenv("TTT_EVENTS_ONCE"); once_v = e ? atoi(e) : 1; }
        float cur = lf(anim + 0x138), prev = lf(anim + 0x13C);
        orig(c);
        if (!once_v || !battle_active() || cur == prev || (int)(cur + 1.0f / 61.0f) != (int)(prev + 1.0f / 61.0f)) return;
        uint8_t* b = psp_p(ent + EV_BLOQUE);   // only the tick list: +0x1C..+0x20 and +0x0C belong to the animation
        b[30] = 0;
        uint32_t a_lo = psp_l32(0x08A6FE80u), a_hi = psp_l32(0x08A6FE84u);
        memcpy(b + 136, &a_lo, 4); memcpy(b + 140, &a_hi, 4);
        return;
    }
    if (dir == 0x088B4EF0u) {                 // the aura reserves render table slots; without TTT_SLOTS only on even ticks
        if (!slots_on() && half_active && tick_odd && vfx_mode() != 60) return;
        orig(c);
        return;
    }
    if (dir == 0x08902B40u) {                 // 0x0887BAA4 clears the slot table every tick: with the gate, the even tick's clear
        if (slots_on() && vfx_mode() != 60 && !tick_odd && battle_active()) return;   // carries into the odd tick (its ids stay valid)
        orig(c);
        return;
    }
    if (dir == 0x089056BCu) {                 // battle clock (+0 frames, +8 s, +0xA ms): adds 1/30 s per call
        static std::unordered_map<uint32_t, bool> done;
        if (battle_active()) { bool& h = done[c.r[4]]; h = !h; if (!h) { c.r[2] = 1; return; } }
        orig(c);
        return;
    }
    if (dir == 0x089D1788u) {                 // frames in state >= a1: the battle UI counted at 30
        if (battle_active() && (int32_t)c.r[5] > 0 && !(prompt_20 && c.r[31] == 0x089DC3F0u)) c.r[5] *= 2;
        orig(c);
        return;
    }
    if (dir == 0x089DADDCu) {                 // battle text sounds per frame (table 0x08A71730, 30 Hz frames)
        if (battle_active()) {
            if (c.r[5] & 1) return;
            c.r[5] >>= 1;
        }
        orig(c);
        return;
    }
    if (dir == 0x0893BD50u) { clash_manager(c, orig); return; }
    if (dir == 0x089DC2A0u) {                 // clash HUD, prompt state: waits >= 10 (doubled to 20 by 0x089D1788) and then exactly == 10
        uint32_t o = c.r[4], t = psp_l32(o + 0x54);
        bool twenty = battle_active() && t == 20u;
        if (twenty) { psp_s32(o + 0x54, 10u); prompt_20 = true; }
        orig(c);
        if (twenty) { psp_s32(o + 0x54, 20u); prompt_20 = false; }
        return;
    }
    if (dir == 0x089CFA54u) {                 // arms a timer in seconds (x30 ticks)
        if (c.r[31] >= 0x08900000u && c.r[31] < 0x08910000u) c.f[12].f *= 2.0f;
        orig(c);
        return;
    }
    uint32_t cb = c.r[4];
    if (dir == 0x08946110u) {                 // homing dash to the rival: v0 = 1 when it reached the target
        float p[3] = { lf(cb + 0x40), lf(cb + 0x44), lf(cb + 0x48) };
        orig(c);
        if (c.r[2] != 1) for (int i = 0; i < 3; i++) ef(cb + 0x40 + i * 4, p[i] + (lf(cb + 0x40 + i * 4) - p[i]) * 0.5f);
        return;
    }
    if (dir == 0x08944A48u) {                 // facing turn +0x54 toward the movement direction: half the turn
        static int snap = -1; if (snap < 0) { const char* e = getenv("TTT_TURN_SNAP"); snap = e ? atoi(e) : 1; }
        if (snap && c.f[12].f >= 3.1f && c.f[13].f >= 0.999f) { orig(c); return; }   // maximum half-turn step and rate 1: instant turn (teleport)
        static int exact = -1; if (exact < 0) { const char* e = getenv("TTT_TURN_EXACT"); exact = e ? atoi(e) : 1; }
        float g = lf(cb + 0x54);
        float t = c.f[13].f, pmax = c.f[12].f, diff = wrap_angle(lf(cb + 0xD4) - g);
        if (exact && t > 0.0f && t < 1.0f) {   // two 60 Hz ticks = one 30 Hz tick: proportional span with rate 1-sqrt(1-t), fixed-step spans halved
            orig(c);
            float d30 = wrap_angle(lf(cb + 0x54) - g);
            if (std::fabs(diff) * t < 0.0314f || std::fabs(d30) >= pmax - 1e-6f) ef(cb + 0x54, wrap_angle(g + d30 * 0.5f));
            else ef(cb + 0x54, wrap_angle(g + diff * (1.0f - std::sqrt(1.0f - t))));
            return;
        }
        orig(c);
        ef(cb + 0x54, wrap_angle(g + wrap_angle(lf(cb + 0x54) - g) * 0.5f));
        return;
    }
    if (dir == 0x0894660Cu) {                 // gravity: vy += g, y += vy
        float vy0 = lf(cb + 0xDC), y0 = lf(cb + 0x44);
        orig(c);
        float vy = vy0 + (lf(cb + 0xDC) - vy0) * 0.5f;
        ef(cb + 0xDC, vy);
        ef(cb + 0x44, y0 + vy * 0.5f);
        return;
    }
    if (dir == 0x08946658u) {                 // vertical flight: vy approaches f12 with step f13, y += vy
        float y0 = lf(cb + 0x44);
        c.f[13].f *= 0.5f;
        orig(c);
        ef(cb + 0x44, y0 + lf(cb + 0xDC) * 0.5f);
        return;
    }
    orig(c);
}

