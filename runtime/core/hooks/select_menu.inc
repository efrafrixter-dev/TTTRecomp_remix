#include <functional>
#include <condition_variable>
#include <deque>
#include <filesystem>
// item 81: optional select menus (BT3 style, modern). Step 1: TTT_SEL_PROBE=1 prints the free battle select as the game runs it
// (cursor dispatcher 0x08A0A0A0 of class 0x08A06368, vtable 0x08A73E78; W = [cursor+0x12C], data base W+0xB8, EBOOT_TTT 53, 84, 125)
static bool sm_probe_on() {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_SEL_PROBE"); on = e && *e == '1'; }
    return on != 0;
}
static bool sm_gp(uint32_t a) { return a >= 0x08800000u && a < 0x10000000u; }

static void sm_probe(uint32_t o) {
    static uint32_t lw = 0;
    static int lines = 0;
    static std::string lc, lr, lp, lg;
    static uint32_t held[4];
    uint32_t w = psp_l32(o + 0x12C);
    if (!sm_gp(w) || lines > 900) return;
    uint32_t c0 = psp_l32(w + 0x70);
    if (o != c0) return;   // once per frame, from cursor 0
    uint32_t vb = psp_l32(0x08A78E14u), b = w + 0xB8, g = 0x08B63D88u;
    if (w != lw) {
        lw = w; lc.clear(); lr.clear(); lp.clear(); lg.clear(); memset(held, 0, sizeof held);
        uint32_t m = psp_l32(0x08A7EA50u);
        printf("sel81: W 0x%08X vt 0x%08X datos 0x%08X, modo W %d G %d, G+0x2E8 %d, modo global %d, escena %d, cursores 0x%08X 0x%08X 0x%08X 0x%08X\n",
               w, psp_l32(w + 0x40), psp_l32(o + 0x68), (int)psp_l32(b), (int)psp_l32(g + 0xC0), (int)psp_l32(g + 0x2E8), (int)psp_l32(0x08B48A28u),
               sm_gp(m) ? (int)psp_l32(m + 0x10) : -1, c0, psp_l32(w + 0x74), psp_l32(w + 0x78), psp_l32(w + 0x7C));
        lines++;
    }
    char t[1024]; int n = 0;
    for (int p = 0; p < 4; p++) {
        uint32_t q = psp_l32(w + 0x70 + 4 * p);
        if (!sm_gp(q)) { n += snprintf(t + n, sizeof t - n, " | c%d -", p); continue; }
        uint32_t st = psp_l32(q + 0x78);
        int v = st < 12 ? (int)psp_l32(q + 0x8C + 4 * st) : -9, fl = st < 12 ? (int)psp_l32(q + 0xEC + 4 * st) : -9;
        n += snprintf(t + n, sizeof t - n, " | c%d st %d>%d pad %d/%d luch %d val %d fl %d v5 %d", p, (int)st, (int)psp_l32(q + 0x7C), (int)psp_l32(q + 0x80),
                      (int)psp_l32(q + 0x84), (int)psp_l32(q + 0x88), v, fl, (int)psp_l32(q + 0x8C + 4 * 5));
    }
    if (lc != t) { lc = t; lines++; printf("sel81: v%u cursores%s\n", vb, t); }
    n = 0;
    for (int r = 0; r < 4; r++) {
        uint32_t row = b + 4 + 0x6C * r, rec = b + 0x1B4 + 0x18 * r;
        int m = 0;
        for (int i = 0; i < 4; i++) if (psp_l32(row + 0x5C + 4 * i)) m |= 1 << i;
        n += snprintf(t + n, sizeof t - n, " | r%d eq %d act %d cpu %d niv %d pad %d m%X reg %d %d %d %d %d %d dan %d", r, (int)psp_l32(row + 0x40), (int)psp_l32(row + 0x44),
                      (int)psp_l32(row + 0x4C), (int)psp_l32(row + 0x50), (int)psp_l32(row + 0x58), m, (int)psp_l32(rec), (int)psp_l32(rec + 4), (int)psp_l32(rec + 8),
                      (int)psp_l32(rec + 0xC), (int)psp_l32(rec + 0x10), (int)psp_l32(rec + 0x14), cs_sel[r]);
    }
    if (lr != t) { lr = t; lines++; printf("sel81: v%u filas%s\n", vb, t); }
    n = 0;
    for (int k = 0; k < 13; k++) {
        uint32_t pg = psp_l32(w + 0x80 + 4 * k);
        if (!sm_gp(pg)) continue;
        n += snprintf(t + n, sizeof t - n, " +%X %d/%d", 0x80 + 4 * k, (int)psp_l32(pg + 0x48), (int)psp_l32(pg + 0x50));
    }
    if (lp != t) { lp = t; lines++; printf("sel81: v%u paginas%s\n", vb, t); }
    snprintf(t, sizeof t, " peticion %d, mapa %d %d %d", (int)psp_l32(w + 0x6C), (int)psp_l32(b + 0x214), (int)psp_l32(b + 0x218), (int)psp_l32(b + 0x21C));
    if (lg != t) { lg = t; lines++; printf("sel81: v%u widget%s\n", vb, t); }
    for (int k = 0; k < 4; k++) {
        uint32_t pr = psp_l32(0x08B4E990u + 0x58u * k + 0x30);
        if (pr && pr != held[k]) {
            int who = -1;
            for (int p = 0; p < 4; p++) { uint32_t q = psp_l32(w + 0x70 + 4 * p); if (sm_gp(q) && psp_l32(q + 0x80) == (uint32_t)k) who = p; }
            lines++;
            printf("sel81: v%u entrada %d pulsa 0x%X, cursor %d\n", vb, k, pr, who);
        }
        held[k] = pr;
    }
}

// BT3 style logic (decomp CharSel, handoff 92.5) over the game's own records: the cursors of the select stay frozen in states 1..5 and
// the port writes W+0x26C+0x18*row (+0 cell id, +4 menu number, +8 form, +0xC colour); the stage select opens through the page
// callback (W vtable +0xC8, state 5, value 4). Supported: free battle of one player (48_30) and local (24_18), one fighter per team
enum { SM_GRID, SM_FORM, SM_COLOR, SM_CUSTOM, SM_DONE };
struct SmSide { int row = -1, pad = 0, st = SM_GRID, pos = 0, form = 0, col = 0, dmg = 0, foc = 0, cus = 0, team = 0; bool human = false; };
// up to 4 slots (rows); seq = more than 2: one slot picks at a time in ord (his design 93.5); vis = the slot each side of the screen shows
struct SmMenu { bool on = false, ret = false, seq = false; uint32_t w = 0, drv = 0; int last = -1, shown = 0, logs = 0, hide = 0, n = 0, act = -1, ord[4] = {0, 1, 2, 3}, vis[2] = {0, 1}; SmSide s[4]; };
static SmMenu sm;
// BT3 movie timings (PZS3US1 file 0x1C1: side reels 23/24, plates 31/32, main 14), in 60 Hz frames since each event
enum { SB_NONE, SB_COL_IN, SB_COL_OUT, SB_SWAP, SB_SWAP_BACK, SB_CUS_OK, SB_CUS_IN };
struct SmAnim { float in = -1, out = -1, row = -1, wipe = -1, on = -1, off = -1, ok = -1, box = -1; int rdir = 0, rold = 0, offi = -1, bev = 0; bool wforms = false, wait = false; };
static SmAnim sm_an[4];
static float sm_now = 0, sm_t0 = -1;
static bool sm_busy() { return sm.on; }
static int sm_style() { const char* e = getenv("TTT_SEL_STYLE"); return e && *e ? atoi(e) : 0; }
static uint32_t sm_rng = 0x9E3779B9u;
static int sm_rand(int n) { sm_rng ^= sm_rng << 13; sm_rng ^= sm_rng >> 17; sm_rng ^= sm_rng << 5; return n > 0 ? (int)(sm_rng % (uint32_t)n) : 0; }
static bool sm_vt_ok(uint32_t w) { uint32_t vt = psp_l32(w + 0x40); return vt == 0x08A905F8u || (vt == 0x08A83C50u && lmp_cfg_on); }
static uint32_t sm_rec(int row) { return sm.w + 0x26C + 0x18 * row; }
static int sm_cells() { return (int)ro_get().cells.size(); }
static const RosterCell& sm_cell(int pos) { return ro_get().cells[pos]; }
static bool sm_random(int pos) { return sm_cell(pos).ids[0] == RO_RANDOM; }
static bool sm_open(int pos) {
    int m = sm_cell(pos).ids[0];
    uint32_t s = psp_l32(0x08A7F708u);
    if (m == RO_RANDOM || !sm_gp(s)) return true;
    return (psp_l32(s + 0x184 + (m >> 5) * 4) >> (m & 31)) & 1;
}
static int sm_ncol(int menu) {
    static std::map<int, int> memo;
    auto it = memo.find(menu);
    if (it != memo.end()) return it->second;
    int s = s3d_sys_of_menu(menu), n = 1;
    uint32_t p = psp_l32(0x08A7F280u), b = sm_gp(p) ? psp_l32(p) : 0;
    if (s >= 0 && s < RO_SYS && sm_gp(b)) {
        uint32_t off = __builtin_bswap32(psp_l32(b + 0x94)) & ~3u;
        n = (int16_t)psp_l16(b + off + (uint32_t)s * 60 + 0xA);
    }
    int r = n >= 1 && n <= 10 ? n : 1;
    if (memo.size() < 4096) memo[menu] = r;
    if (sm.logs++ < 300) printf("sel81: menu %d (id %d) con %d colores%s\n", menu, s, r, r != n ? " (valor fuera de rango)" : "");
    return r;
}
static int sm_pos_of_cell(uint32_t cell) {
    Roster& r = ro_get();
    for (int i = 0; i < (int)r.cells.size(); i++) if (psp_l32(r.idtab + i * 4) == cell) return i;
    return 0;
}
static void sm_write(Ctx& c, int k) {
    SmSide& d = sm.s[k];
    const RosterCell& cl = sm_cell(d.pos);
    int f = d.form < cl.n ? d.form : 0, m = cl.ids[f];
    uint32_t rec = sm_rec(d.row);
    psp_s32(rec, psp_l32(ro_get().idtab + d.pos * 4));
    psp_s32(rec + 4, (uint32_t)m);
    psp_s32(rec + 8, (uint32_t)(d.st >= SM_FORM ? f : 0));
    if (d.st >= SM_COLOR) {
        psp_s32(rec + 0xC, (uint32_t)d.col);
        uint32_t ent = 0;
        cs_av_of(c, m, d.col, &ent);
        cs_menu[d.row] = m; cs_sel[d.row] = d.dmg; cs_ent[d.row] = ent;
    }
    psp_s32(rec + 0x10, (uint32_t)(d.st >= SM_CUSTOM ? d.cus : 0));
}
static void sm_log(int k, const char* what) {
    SmSide& d = sm.s[k];
    if (sm.logs++ < 300) printf("sel81: lado %d (fila %d, entrada %d%s) %s: estado %d, casilla %d, menu %d, forma %d, color %d, danado %d, personalizar %d\n", k, d.row, d.pad,
                               d.human ? "" : ", COM", what, d.st, d.pos, sm_cell(d.pos).ids[d.form < sm_cell(d.pos).n ? d.form : 0], d.form, d.col, d.dmg, d.cus);
}
static void sm_show(int k) { sm.shown = k; }
// pad 0 drives J1's side, then the COM sides in order; other humans drive their own side at the same time
static bool sm_turn(int k) {
    if (sm.s[k].pad != 0) return true;
    int first = -1, last = -1;
    for (int j = 0; j < sm.n; j++) if (sm.s[j].pad == 0) { if (first < 0 && sm.s[j].st != SM_DONE) first = j; last = j; }
    return k == (first >= 0 ? first : last);
}
static int sm_prev0(int k) {
    for (int j = k - 1; j >= 0; j--) if (sm.s[j].pad == 0) return j;
    return -1;
}
static int sm_cur() { for (int i = 0; i < sm.n; i++) if (sm.s[sm.ord[i]].st != SM_DONE) return sm.ord[i]; return -1; }
static bool sm_act(int k) { return sm.seq ? k == sm_cur() : sm_turn(k); }
static int sm_prev(int k) {
    if (!sm.seq) return sm_prev0(k);
    for (int i = 1; i < sm.n; i++) if (sm.ord[i] == k) return sm.ord[i - 1];
    return -1;
}
static void sm_finish(Ctx& c) {
    uint32_t o = sm.drv, pg = psp_l32(o + 0x12C), vt = sm_gp(pg) ? psp_l32(pg + 0x40) : 0;
    sm.on = false;
    if (!vt) return;
    for (int p = 0; p < 4; p++) {
        uint32_t q = psp_l32(sm.w + 0x70 + 4 * p);
        if (!sm_gp(q) || q == o) continue;
        int st = (int)psp_l32(q + 0x78);
        if (st >= 1 && st <= 5) { psp_s32(q + 0x78, 0); psp_s32(q + 0x7C, 0); psp_s32(q + 0x80, 0xFFFFFFFFu); }
    }
    for (int i = 0; i < 6; i++) {   // character pages leave as the game's state 4 -> 5 does (0x089D161C)
        uint32_t q = psp_l32(sm.w + 0x84 + 4 * i);
        if (sm_gp(q) && (int)psp_l32(q + 0x48) != -1) mpm_call(c, 0x089D161Cu, q);
    }
    psp_s32(o + 0x78, 5); psp_s32(o + 0x7C, 5); psp_s32(o + 0x8C + 4 * 5, 4);
    printf("sel81: %d puestos listos: abre la seleccion de mapa\n", sm.n);
    mpm_call(c, psp_l32(vt + 0xCC), pg + (uint32_t)(int16_t)psp_l16(vt + 0xC8), 0, o);
}
static void sm_back_out(Ctx& c) {
    uint32_t o = sm.drv, pg = psp_l32(o + 0x12C), vt = sm_gp(pg) ? psp_l32(pg + 0x40) : 0;
    sm.on = false;
    psp_s32(o + 0xEC + 4 * 5, 0);   // 0x08A8D6CC: back from state 1 opens the game room when [cursor+0xEC+4*5] == 1 (the port skips it)
    printf("sel81: atras desde J1: vuelve a la pantalla de equipos\n");
    if (vt) mpm_call(c, psp_l32(vt + 0xCC), pg + (uint32_t)(int16_t)psp_l16(vt + 0xC8), 1, o);
}
static bool sm_forms(const SmSide& d) { const RosterCell& cl = sm_cell(d.pos); return d.st >= SM_FORM && cl.n > 1 && !sm_random(d.pos); }
static void sm_ev(int k, const SmSide& o, bool up) {
    SmSide& d = sm.s[k];
    SmAnim& a = sm_an[k];
    float t = sm_now;
    auto box = [&](int e) { a.bev = e; a.box = t; };
    if (o.st == d.st) {
        if ((d.st == SM_GRID && o.pos != d.pos) || (d.st == SM_FORM && o.form != d.form)) {
            a.offi = d.st == SM_GRID ? o.pos % 7 : o.form; a.off = t; a.on = t; a.ok = -1;
            if (d.st == SM_GRID && o.pos / 7 != d.pos / 7) { a.row = t; a.rdir = up ? -1 : 1; a.rold = o.pos / 7 * 7; }
        }
        return;
    }
    if ((o.st == SM_GRID && d.st == SM_FORM) || (o.st == SM_FORM && d.st == SM_GRID)) { a.wipe = t; a.wforms = o.st == SM_FORM; a.on = t; a.ok = -1; a.row = a.off = -1; }
    else if (d.st == SM_COLOR && o.st < SM_COLOR) { a.ok = t; box(SB_COL_IN); }
    else if (o.st == SM_COLOR && d.st < SM_COLOR) { a.on = t; a.ok = -1; box(SB_COL_OUT); }
    else if (o.st == SM_COLOR && d.st == SM_CUSTOM) box(SB_SWAP);
    else if (o.st == SM_CUSTOM && d.st == SM_COLOR) box(SB_SWAP_BACK);
    else if (o.st == SM_CUSTOM && d.st == SM_DONE) { box(SB_CUS_OK); a.out = t; a.in = -1; }
    else if (o.st == SM_DONE && d.st == SM_CUSTOM) { box(SB_CUS_IN); a.in = t; a.out = -1; }
}
static void sm_input0(Ctx& c, int k) {
    SmSide& d = sm.s[k];
    uint32_t pad = 0x08B4E990u + 0x58u * (uint32_t)d.pad, rep = psp_l32(pad + 0x34), edge = psp_l32(pad + 0x30);
    bool ok = edge & psp_l32(0x08A793D4u), bk = edge & psp_l32(0x08A793D0u);
    int L = rep & 1, R = rep & 2, D = rep & 4, U = rep & 8, n = sm_cells();
    const RosterCell* cl = &sm_cell(d.pos);
    if (!(ok || bk || L || R || D || U)) return;
    sm_show(k);
    switch (d.st) {
    case SM_GRID:
        if (L || R || U || D) {
            int rows = (n + 6) / 7, col = d.pos % 7, row = d.pos / 7, p = d.pos;
            auto ok_at = [&](int r, int cc) { int q = r * 7 + cc; return q < n && sm_open(q); };
            if (L || R) {
                for (int t = 0; t < 7; t++) { col = (col + (L ? 6 : 1)) % 7; if (ok_at(row, col)) { p = row * 7 + col; break; } }
            } else {
                for (int t = 0; t < rows; t++) {
                    row = (row + (U ? rows - 1 : 1)) % rows;
                    int cc = col, u = 0;
                    while (u < 7 && !ok_at(row, cc)) { cc = (cc + 1) % 7; u++; }
                    if (u < 7) { p = row * 7 + cc; break; }
                }
            }
            if (p != d.pos) { d.pos = p; d.form = 0; mpm_sfx(c, 2, 5); mpm_sfx(c, 2, 6); sm_write(c, k); sm_log(k, "mueve"); }
        } else if (ok) {
            if (sm_random(d.pos)) {
                int p = d.pos;
                for (int t = 0; t < 64 && (sm_random(p) || !sm_open(p)); t++) p = sm_rand(n);
                if (!sm_random(p)) { d.pos = p; d.form = sm_rand(sm_cell(p).n); }
                d.st = SM_COLOR;
            } else d.st = cl->n > 1 ? SM_FORM : SM_COLOR;
            if (d.st == SM_COLOR) { int nc = sm_ncol(sm_cell(d.pos).ids[d.form]); if (d.col >= nc) d.col = 0; d.dmg = 0; d.foc = 0; }
            mpm_sfx(c, 1, 6); sm_write(c, k); sm_log(k, "confirma");
        } else if (bk) {
            int j = sm_prev(k);
            if (j >= 0 && (sm.seq || d.pad == 0)) { mpm_sfx(c, 1, 0); sm.s[j].st = SM_CUSTOM; sm_show(j); sm_write(c, j); sm_log(j, "vuelve"); }
            else if (j < 0 && (sm.seq || k == 0 || d.pad == 0)) { mpm_sfx(c, 1, 0); sm_back_out(c); }
        }
        break;
    case SM_FORM:
        if (L || R) { d.form = (d.form + (L ? cl->n - 1 : 1)) % cl->n; mpm_sfx(c, 1, 3); sm_write(c, k); sm_log(k, "forma"); }
        else if (ok) { d.st = SM_COLOR; int nc = sm_ncol(cl->ids[d.form]); if (d.col >= nc) d.col = 0; d.dmg = 0; d.foc = 0; mpm_sfx(c, 1, 6); sm_write(c, k); sm_log(k, "confirma"); }
        else if (bk) { d.st = SM_GRID; d.form = 0; mpm_sfx(c, 1, 0); sm_write(c, k); sm_log(k, "atras"); }
        break;
    case SM_COLOR: {
        int m = cl->ids[d.form < cl->n ? d.form : 0], nc = sm_ncol(m);
        uint32_t ent = 0;
        int av = net_online_ranked() ? 0 : cs_av_of(c, m, d.col, &ent) & 6;
        if ((U || D) && av) { int f = D ? 1 : 0; if (f != d.foc) { d.foc = f; mpm_sfx(c, 1, 3); sm_log(k, f ? "fila danado" : "fila color"); } }
        else if ((L || R) && d.foc && av) {
            int o[3], q = 0, i = 0;
            for (int l = 0; l < 3; l++) if (!l || (av & (1 << l))) { if (l == d.dmg) i = q; o[q++] = l; }
            d.dmg = o[(i + (L ? q - 1 : 1)) % q];
            mpm_sfx(c, 1, 3); sm_write(c, k); sm_log(k, "danado");
        } else if (L || R) {
            if (nc > 1) {
                d.col = (d.col + (L ? nc - 1 : 1)) % nc;
                int a2 = net_online_ranked() ? 0 : cs_av_of(c, m, d.col, &ent) & 6;
                if (d.dmg && !(a2 & (1 << d.dmg))) d.dmg = 0;
                if (!a2) d.foc = 0;
                mpm_sfx(c, 1, 3); sm_write(c, k); sm_log(k, "color");
            }
        } else if (ok) { d.st = SM_CUSTOM; d.cus = 0; mpm_sfx(c, 1, 6); sm_write(c, k); sm_log(k, "color listo"); }
        else if (bk) { d.st = cl->n > 1 && !sm_random(d.pos) ? SM_FORM : SM_GRID; d.foc = 0; mpm_sfx(c, 1, 0); sm_write(c, k); sm_log(k, "atras"); }
        break;
    }
    case SM_CUSTOM:
        if (U || D) { d.cus = (d.cus + (U ? 3 : 1)) % 4; mpm_sfx(c, 1, 3); sm_write(c, k); sm_log(k, "personalizar"); }
        else if (ok) { d.st = SM_DONE; mpm_sfx(c, 1, 6); sm_write(c, k); sm_log(k, "listo"); }
        else if (bk) { d.st = SM_COLOR; mpm_sfx(c, 1, 0); sm_log(k, "atras"); }
        break;
    case SM_DONE:
        if (bk) { d.st = SM_CUSTOM; mpm_sfx(c, 1, 0); sm_log(k, "atras"); }
        break;
    }
}
static void sm_input(Ctx& c, int k) {
    SmSide o[4] = {sm.s[0], sm.s[1], sm.s[2], sm.s[3]};
    bool up = psp_l32(0x08B4E990u + 0x58u * (uint32_t)sm.s[k].pad + 0x34) & 8;
    sm_input0(c, k);
    for (int j = 0; j < sm.n; j++) sm_ev(j, o[j], up);
}
static bool sm_rows(uint32_t w, int rows[2][2], int* cnt) {   // active rows per team; 1 or 2 each (1 vs 1 .. 2 vs 2)
    cnt[0] = cnt[1] = 0;
    for (int r = 0; r < 4; r++) {
        uint32_t row = w + 0xB8 + 4 + 0x6C * r;
        if (!psp_l32(row + 0x44)) continue;
        int t = psp_l32(row + 0x40) ? 1 : 0;
        if (cnt[t] < 2) rows[t][cnt[t]] = r;
        cnt[t]++;
    }
    return cnt[0] >= 1 && cnt[0] <= 2 && cnt[1] >= 1 && cnt[1] <= 2;
}
// the game's select pages stay alive but draw nothing while the port's menu owns the screen (also their in/out animations): each page
// gets a copy of its vtable whose draw entry (+0x68, grid page vtable 0x08A733D8 -> 0x089F9F34) is the empty 0x089F8FA0
static std::map<uint32_t, uint32_t> sm_vtc, sm_vto;
static bool sm_masked = false;
static void sm_mask(uint32_t w, bool on) {
    static const uint32_t PG[] = {0x88, 0x8C, 0x90, 0x94, 0x98, 0xAC};
    if (!on && !sm_masked) return;
    sm_masked = on;
    for (uint32_t off : PG) {
        uint32_t q = psp_l32(w + off);
        if (!sm_gp(q)) continue;
        uint32_t vt = psp_l32(q + 0x40);
        if (!sm_gp(vt)) continue;
        if (on && !sm_vto.count(vt)) {
            uint32_t& cp = sm_vtc[vt];
            if (!cp) {
                cp = psp_alloc(0x100);
                memcpy(psp_p(cp), psp_p(vt), 0x100);
                psp_s32(cp + 0x68, 0); psp_s32(cp + 0x6C, 0x089F8FA0u);
                sm_vto[cp] = vt;
                printf("sel81: pagina +0x%X (vtable 0x%08X, dibujo 0x%08X) sin dibujo mientras manda el menu del port\n", off, vt, psp_l32(vt + 0x6C));
            }
            psp_s32(q + 0x40, cp);
        } else if (!on && sm_vto.count(vt)) psp_s32(q + 0x40, sm_vto[vt]);
    }
}
static bool sm_begin(uint32_t w, uint32_t o) {
    int rows[2][2], cnt[2];
    if (!sm_rows(w, rows, cnt)) {
        static uint32_t lw = 0;
        if (lw != w) { lw = w; printf("sel81: estilo BT3 sin soporte para %d contra %d: seleccion original\n", cnt[0], cnt[1]); }
        return false;
    }
    sm.on = true; sm.w = w; sm.drv = o; sm.shown = 0;
    int n = 0, sl[2][2];
    for (int t = 0; t < 2; t++) for (int i = 0; i < cnt[t]; i++) {
        SmSide& d = sm.s[n];
        int r = rows[t][i], pl = lmp_cfg_on ? mpm_row_player(r) : (n == 0 ? 0 : -1);
        if (d.row != r || !sm.ret) d = SmSide();
        d.row = r; d.team = t; d.human = pl >= 0; d.pad = pl >= 0 ? pl : 0;
        uint32_t rec = sm_rec(r);
        if (!sm.ret) { uint32_t cell = psp_l32(rec); d.pos = cell == 99 ? 0 : sm_pos_of_cell(cell); d.st = SM_GRID; }
        else d.st = SM_DONE;
        sl[t][i] = n++;
    }
    sm.n = n; sm.seq = n > 2;
    // order: J1, then its partner when human (same reel), else J1, COM1, partner, COM2 (93.5)
    int q = 0;
    if (cnt[0] == 2 && sm.s[sl[0][1]].human) { sm.ord[q++] = sl[0][0]; sm.ord[q++] = sl[0][1]; for (int i = 0; i < cnt[1]; i++) sm.ord[q++] = sl[1][i]; }
    else for (int i = 0; i < 2; i++) for (int t = 0; t < 2; t++) if (i < cnt[t]) sm.ord[q++] = sl[t][i];
    if (sm.ret) {
        if (sm.seq) sm.s[sm.ord[n - 1]].st = SM_CUSTOM;
        else for (int k = 1; k >= 0; k--) if (sm.s[k].pad == 0 || sm.s[k].human) { sm.s[k].st = SM_CUSTOM; if (sm.s[k].pad == 0) break; }
        for (int t = 0; t < 2; t++) for (int i = 0; i < n; i++) if (sm.s[sm.ord[i]].team == t) sm.vis[t] = sm.ord[i];
    } else for (int t = 0; t < 2; t++) sm.vis[t] = sl[t][0];
    sm.act = sm.seq ? sm_cur() : -1;
    for (int k = 0; k < n; k++) {
        SmAnim& a = sm_an[k];
        a = SmAnim();
        if (sm.s[k].st != SM_DONE) { a.in = sm_now; a.on = sm_now; }
        if (sm.s[k].st == SM_CUSTOM) { a.bev = SB_CUS_IN; a.box = sm_now; }
    }
    sm_t0 = sm.ret ? -1 : sm_now;
    sm.ret = false;
    sm.hide = 8;
    if (sm_rng == 0x9E3779B9u) sm_rng ^= (uint32_t)psp_time_us();
    char b[160]; int m = 0;
    for (int i = 0; i < n; i++) { const SmSide& d = sm.s[sm.ord[i]]; m += snprintf(b + m, sizeof b - m, " %s fila %d equipo %d (%s)", i ? "," : "", d.row, d.team, d.human ? "humano" : "COM"); }
    printf("sel81: estilo BT3 activo en W 0x%08X, %d contra %d, orden:%s\n", w, cnt[0], cnt[1], b);
    return true;
}
// seq: the next slot takes over; on the same side the reel stays (it passes to him), on a side whose reel left it comes in again
static void sm_handover() {
    int b = sm_cur(), a = sm.act;
    if (!sm.seq || b == sm.act) return;
    sm.act = b;
    if (b < 0) return;
    int t = sm.s[b].team, pv = sm.vis[t];
    SmAnim& an = sm_an[b];
    if (a >= 0 && sm.s[a].team == t && sm.s[a].st == SM_DONE) { an.in = -1; an.out = -1; an.on = sm_now; }
    else if (pv != b && pv >= 0 && sm.s[pv].st == SM_DONE) { an.in = sm_now; an.out = -1; an.on = sm_now; }
    sm.vis[t] = b;
    if (sm.logs++ < 300) printf("sel81: turno de la fila %d (lado %s)\n", sm.s[b].row, t ? "abajo" : "arriba");
}

static void sm_cursor(Ctx& c, void (*orig)(Ctx&)) {
    uint32_t o = c.r[4], w = psp_l32(o + 0x12C);
    int st = (int)psp_l32(o + 0x78);
    if (sm.on && (w != sm.w || !sm_gp(w))) sm.on = false;
    if (sm.on && st >= 1 && st <= 5) {
        if (o == sm.drv) {
            if (sm.hide > 0) {   // the game's character pages leave (0x089D161C) but the grid, whose draw becomes the port's select
                sm.hide--;
                for (int i = 1; i < 6; i++) { uint32_t q = psp_l32(sm.w + 0x84 + 4 * i); if (sm_gp(q) && (int)psp_l32(q + 0x48) != -1 && (int)psp_l32(q + 0x48) != 2) mpm_call(c, 0x089D161Cu, q); }
            }
            int k0 = -1;
            bool turn[4];   // decided before any input: a confirm that ends J1's turn is not read again by the next slot
            for (int k = 0; k < sm.n; k++) turn[k] = sm_act(k);
            for (int k = 0; k < sm.n && sm.on; k++) if (turn[k]) { if (k0 < 0) k0 = k; sm_input(c, k); }
            bool all = sm.on;
            for (int k = 0; k < sm.n; k++) if (sm.s[k].st != SM_DONE) all = false;
            if (all) sm_finish(c);
            else if (sm.on) { sm_handover(); if (k0 >= 0 && !sm.s[k0].human && sm.shown != k0) sm_show(k0); }
            if (sm_probe_on()) sm_probe(o);
        }
        return;
    }
    orig(c);
    if (sm_probe_on()) sm_probe(o);
    if (sm_gp(w) && !sm.on && o == psp_l32(w + 0x70)) {
        int rows[2][2], cnt[2];
        sm_mask(w, sm_style() == 1 && sm_vt_ok(w) && sm_rows(w, rows, cnt));
    }
    if (!sm_gp(w) || sm.on || sm_style() != 1 || !sm_vt_ok(w)) return;
    if (sm.w != w) { sm.w = w; sm.drv = 0; sm.last = -1; sm.ret = false; }
    uint32_t mc = sm.drv ? sm.drv : psp_l32(w + 0x70);
    int now = (int)psp_l32(o + 0x78);
    if (o == mc) {
        if (sm.last >= 6 && now == 5) sm.ret = true;
        if (now == 0) sm.ret = false;
        sm.last = now;
        if (now == 5 && sm.ret) {   // back from the stage select: the character page of J1's row opens again
            uint32_t vt = psp_l32(w + 0x40);
            int r = -1;
            for (int q = 0; q < 4 && r < 0; q++) if (psp_l32(w + 0xB8 + 4 + 0x6C * q + 0x44)) r = q;
            if (lmp_cfg_on && lmp_cfg_n > 0) r = lmp_cfg_slot[0];
            psp_s32(o + 0x8C + 4 * 5, (uint32_t)(r < 0 ? 0 : r));
            printf("sel81: atras desde la seleccion de mapa: abre la fila %d\n", r);
            mpm_call(c, psp_l32(vt + 0xCC), w + (uint32_t)(int16_t)psp_l16(vt + 0xC8), 0, o);
            return;
        }
    }
    if (now == 1 && sm_begin(w, o)) { for (int k = 0; k < sm.n; k++) sm_write(c, k); sm_show(0); }
}

// ---- drawing (GE sprites in the menu's 2D list, in place of the grid page's draw 0x089F9F34): pieces games/ttt/select_bt3, renders, texts
bool psp_sel_small(int menu, std::vector<uint32_t>& px, int* w, int* h);
bool psp_sel_map_big(int map, std::vector<uint32_t>& px, int* w, int* h);
int psp_form_row(int menu);
std::string psp_assets_dir();
std::string psp_shared_rel(const std::string& legacy);
std::string psp_shared_file(const std::string& rel);
struct SmImg { int st = 0; std::vector<uint32_t> px; int w = 0, h = 0; BhTex t{0, 0, 0}; uint32_t use = 0; bool big = false; };
static std::map<std::string, SmImg> sm_img;
static std::mutex sm_mx;
static std::condition_variable sm_cv;
static std::deque<std::pair<std::string, std::function<bool(std::vector<uint32_t>&, int*, int*)>>> sm_q;
static uint32_t sm_frame = 0;
static void sm_worker() {
    for (;;) {
        std::pair<std::string, std::function<bool(std::vector<uint32_t>&, int*, int*)>> j;
        { std::unique_lock<std::mutex> g(sm_mx); sm_cv.wait(g, [] { return !sm_q.empty(); }); j = std::move(sm_q.front()); sm_q.pop_front(); }
        std::vector<uint32_t> px; int w = 0, h = 0;
        bool ok = j.second(px, &w, &h) && w > 1 && h > 1 && px.size() >= (size_t)w * h;
        while (ok && (w > 1024 || h > 1024)) {   // oversized art: halved until it fits a texture
            int W = w / 2, H = h / 2;
            std::vector<uint32_t> o((size_t)W * H);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                uint32_t s4[4] = {px[(size_t)(2 * y) * w + 2 * x], px[(size_t)(2 * y) * w + 2 * x + 1], px[(size_t)(2 * y + 1) * w + 2 * x], px[(size_t)(2 * y + 1) * w + 2 * x + 1]}, r = 0;
                for (int ch = 0; ch < 32; ch += 8) { uint32_t t = 0; for (uint32_t v : s4) t += v >> ch & 255; r |= (t + 2) / 4 << ch; }
                o[(size_t)y * W + x] = r;
            }
            printf("sel81: %s de %dx%d reducida a %dx%d\n", j.first.c_str(), w, h, W, H);
            px.swap(o); w = W; h = H;
        }
        std::lock_guard<std::mutex> g(sm_mx);
        SmImg& e = sm_img[j.first];
        if (ok) { e.px.swap(px); e.w = w; e.h = h; e.st = 2; } else e.st = 4;
    }
}
static void sm_upload(SmImg& e) {
    int W = 1, H = 1;
    while (W < e.w) W <<= 1;
    while (H < e.h) H <<= 1;
    e.t.a = psp_alloc((uint32_t)(W * H * 4)); e.t.w = W; e.t.h = H; e.t.ow = e.w; e.t.oh = e.h;
    memset(psp_p(e.t.a), 0, (size_t)W * H * 4);
    for (int y = 0; y < e.h; y++) memcpy(psp_p(e.t.a + (uint32_t)(y * W * 4)), e.px.data() + (size_t)y * e.w, (size_t)e.w * 4);
    std::vector<uint32_t>().swap(e.px);
    e.st = 3;
}
// an image by key, loaded in the worker the first time; nullptr until it is ready. Big renders: the 6 most recent stay
static const BhTex* sm_get(const std::string& key, std::function<bool(std::vector<uint32_t>&, int*, int*)> load, bool big = false) {
    static bool started = false;
    if (!started) { started = true; std::thread(sm_worker).detach(); }
    std::lock_guard<std::mutex> g(sm_mx);
    SmImg& e = sm_img[key];
    e.use = sm_frame; e.big = big;
    if (e.st == 0) { e.st = 1; sm_q.emplace_back(key, std::move(load)); sm_cv.notify_one(); return nullptr; }
    if (e.st == 2) {
        if (big) {
            int n = 0; std::map<std::string, SmImg>::iterator old = sm_img.end();
            for (auto it = sm_img.begin(); it != sm_img.end(); ++it) if (it->second.big && it->second.st == 3) { n++; if (old == sm_img.end() || it->second.use < old->second.use) old = it; }
            if (n >= 6 && old != sm_img.end() && old->second.use != sm_frame) { psp_free(old->second.t.a); sm_img.erase(old); }
        }
        SmImg& f = sm_img[key];
        sm_upload(f);
        return &f.t;
    }
    return e.st == 3 ? &e.t : nullptr;
}
static bool sm_file(const std::string& f, std::vector<uint32_t>& px, int* w, int* h) { return !f.empty() && sz_png(f, px, w, h); }
// games/ttt/select_bt3/origen.txt: pieces read from the user's own assets (never shipped). One line per layer, same name = more layers:
// name = lienzo W H [tam W H] [espejo] (whole piece: composed, then Lanczos as PIL, then mirrored)
// name = <path under assets/Shared> [x y w h] [en dx dy] [tam W H] [color r g b] (one layer; its tam is bilinear)
struct SmLayer { std::string path; int cx = 0, cy = 0, cw = 0, ch = 0, dx = 0, dy = 0, sw = 0, sh = 0; float mul[3] = {1, 1, 1}; bool col = false; };
struct SmOrig { int W = 0, H = 0, sw = 0, sh = 0; bool mir = false; std::vector<SmLayer> ly; };
// Lanczos-3 resample exactly as PIL's LANCZOS: RGBA premultiplied to 8 bits, horizontal pass then vertical, 22-bit fixed weights,
// 8-bit rounding between passes, support widened when shrinking
static void sm_lz_axis(const std::vector<uint8_t>& in, int w, int h, int on, bool horiz, std::vector<uint8_t>& out) {
    int n = horiz ? w : h, m = horiz ? h : w, ow = horiz ? on : w, oh = horiz ? h : on;
    double sc = (double)n / on, fs = sc > 1 ? sc : 1, sup = 3 * fs;
    out.assign((size_t)ow * oh * 4, 0);
    std::vector<int> k;
    std::vector<double> wd;
    for (int i = 0; i < on; i++) {
        double c = (i + 0.5) * sc;
        int a = std::max(0, (int)(c - sup + 0.5)), b = std::min(n, (int)(c + sup + 0.5));
        wd.assign(b - a, 0.0);
        double tot = 0;
        for (int j = a; j < b; j++) {
            double x = (j - c + 0.5) / fs, v;
            if (x == 0) v = 1; else if (x <= -3 || x >= 3) v = 0; else { double px = 3.14159265358979323846 * x; v = sin(px) / px * (sin(px / 3) / (px / 3)); }
            wd[j - a] = v; tot += v;
        }
        k.assign(b - a, 0);
        for (int j = 0; j < b - a; j++) { double v = tot != 0 ? wd[j] / tot : 0; k[j] = (int)(v * (1 << 22) + (v < 0 ? -0.5 : 0.5)); }
        for (int r = 0; r < m; r++) {
            int64_t acc[4] = {1 << 21, 1 << 21, 1 << 21, 1 << 21};
            for (int j = a; j < b; j++) {
                const uint8_t* p = &in[((size_t)(horiz ? r * w + j : j * w + r)) * 4];
                for (int ch = 0; ch < 4; ch++) acc[ch] += (int64_t)p[ch] * k[j - a];
            }
            uint8_t* q = &out[((size_t)(horiz ? r * ow + i : i * ow + r)) * 4];
            for (int ch = 0; ch < 4; ch++) { int64_t v = acc[ch] >> 22; q[ch] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
        }
    }
}
static void sm_lanczos(const std::vector<uint32_t>& px, int w, int h, int ow, int oh, std::vector<uint32_t>& out) {
    std::vector<uint8_t> f((size_t)w * h * 4), t;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t a = px[i] >> 24;
        for (int c = 0; c < 3; c++) { uint32_t v = (px[i] >> (8 * c) & 255) * a + 128; f[i * 4 + c] = (uint8_t)((v + (v >> 8)) >> 8); }
        f[i * 4 + 3] = (uint8_t)a;
    }
    if (ow != w) { sm_lz_axis(f, w, h, ow, true, t); f.swap(t); }
    if (oh != h) { sm_lz_axis(f, ow, h, oh, false, t); f.swap(t); }
    out.assign((size_t)ow * oh, 0);
    for (size_t i = 0; i < (size_t)ow * oh; i++) {
        uint32_t a = f[i * 4 + 3], r = a << 24;
        for (int c = 0; c < 3; c++) { uint32_t v = f[i * 4 + c]; if (a && a != 255) { v = 255 * v / a; if (v > 255) v = 255; } r |= v << (8 * c); }
        out[i] = r;
    }
}

static const std::map<std::string, SmOrig>& sm_origins() {
    static std::map<std::string, SmOrig> m;
    static bool done = false;
    if (done) return m;
    done = true;
    FILE* fp = fopen("../../games/ttt/select_bt3/origen.txt", "rb");
    if (!fp) return m;
    char ln[512];
    int bad = 0;
    while (fgets(ln, sizeof ln, fp)) {
        std::string l = ln;
        size_t hs = l.find('#');
        if (hs != std::string::npos) l = l.substr(0, hs);
        size_t e = l.find('=');
        if (e == std::string::npos) continue;
        std::vector<std::string> t;
        auto split = [&](const std::string& x, std::vector<std::string>& o) { size_t i = 0; while (i < x.size()) { while (i < x.size() && isspace((unsigned char)x[i])) i++; size_t j = i; while (j < x.size() && !isspace((unsigned char)x[j])) j++; if (j > i) o.push_back(x.substr(i, j - i)); i = j; } };
        std::vector<std::string> k;
        split(l.substr(0, e), k); split(l.substr(e + 1), t);
        if (k.size() != 1 || t.empty()) continue;
        SmOrig& o = m[k[0]];
        auto num = [&](size_t i) { return i < t.size() ? atof(t[i].c_str()) : 0.0; };
        if (t[0] == "lienzo") {
            o.W = (int)num(1); o.H = (int)num(2);
            for (size_t i = 3; i < t.size(); i++) {
                if (t[i] == "tam") { o.sw = (int)num(i + 1); o.sh = (int)num(i + 2); i += 2; }
                else if (t[i] == "espejo") o.mir = true;
                else bad++;
            }
            continue;
        }
        SmLayer y;
        y.path = t[0];
        size_t i = 1;
        if (t.size() >= 5 && isdigit((unsigned char)t[1][0])) { y.cx = (int)num(1); y.cy = (int)num(2); y.cw = (int)num(3); y.ch = (int)num(4); i = 5; }
        for (; i < t.size(); i++) {
            if (t[i] == "en") { y.dx = (int)num(i + 1); y.dy = (int)num(i + 2); i += 2; }
            else if (t[i] == "tam") { y.sw = (int)num(i + 1); y.sh = (int)num(i + 2); i += 2; }
            else if (t[i] == "color") { for (int c = 0; c < 3; c++) y.mul[c] = (float)num(i + 1 + c); y.col = true; i += 3; }
            else bad++;
        }
        o.ly.push_back(y);
    }
    fclose(fp);
    printf("sel81: origen.txt: %d piezas del juego original%s\n", (int)m.size(), bad ? " (con palabras desconocidas)" : "");
    return m;
}
static bool sm_layer(const SmLayer& y, std::vector<uint32_t>& px, int* w, int* h) {
    std::string f = psp_shared_file(y.path);
    std::vector<uint32_t> s; int W = 0, H = 0;
    if (!sm_file(f, s, &W, &H)) return false;
    int cw = y.cw > 0 ? y.cw : W, ch = y.ch > 0 ? y.ch : H;
    if (y.cx < 0 || y.cy < 0 || y.cx + cw > W || y.cy + ch > H) { printf("sel81: recorte fuera de %s (%dx%d)\n", f.c_str(), W, H); return false; }
    int ow = y.sw > 0 ? y.sw : cw, oh = y.sh > 0 ? y.sh : ch;
    px.assign((size_t)ow * oh, 0);
    for (int j = 0; j < oh; j++) for (int i = 0; i < ow; i++) {   // bilinear over the crop (identity when not scaled)
        float u = ow == cw ? (float)i : std::max(0.0f, std::min((float)cw - 1, (i + 0.5f) * cw / ow - 0.5f));
        float v = oh == ch ? (float)j : std::max(0.0f, std::min((float)ch - 1, (j + 0.5f) * ch / oh - 0.5f));
        int u0 = (int)u, v0 = (int)v, u1 = std::min(u0 + 1, cw - 1), v1 = std::min(v0 + 1, ch - 1);
        float fu = u - u0, fv = v - v0, acc[4] = {0, 0, 0, 0};
        uint32_t q[4] = {s[(size_t)(y.cy + v0) * W + y.cx + u0], s[(size_t)(y.cy + v0) * W + y.cx + u1], s[(size_t)(y.cy + v1) * W + y.cx + u0], s[(size_t)(y.cy + v1) * W + y.cx + u1]};
        float wq[4] = {(1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv};
        for (int n = 0; n < 4; n++) for (int c = 0; c < 4; c++) acc[c] += wq[n] * (q[n] >> (8 * c) & 255);
        uint32_t r = 0;
        for (int c = 0; c < 4; c++) { float x = acc[c] * (c < 3 && y.col ? y.mul[c] : 1.0f) + 0.5f; r |= (uint32_t)std::min(255.0f, x) << (8 * c); }
        px[(size_t)j * ow + i] = r;
    }
    *w = ow; *h = oh;
    return true;
}
static bool sm_orig_comp(const SmOrig& o, std::vector<uint32_t>& px, int* w, int* h);
static bool sm_orig_png(const SmOrig& o, std::vector<uint32_t>& px, int* w, int* h) {
    if (!sm_orig_comp(o, px, w, h)) return false;
    if (o.sw > 0 && o.sh > 0 && (o.sw != *w || o.sh != *h)) { std::vector<uint32_t> t; sm_lanczos(px, *w, *h, o.sw, o.sh, t); px.swap(t); *w = o.sw; *h = o.sh; }
    if (o.mir) for (int y = 0; y < *h; y++) std::reverse(px.begin() + (size_t)y * *w, px.begin() + (size_t)(y + 1) * *w);
    return true;
}
static bool sm_orig_comp(const SmOrig& o, std::vector<uint32_t>& px, int* w, int* h) {
    if (o.ly.empty()) return false;
    if (o.ly.size() == 1 && !o.W && !o.ly[0].dx && !o.ly[0].dy) return sm_layer(o.ly[0], px, w, h);
    std::vector<std::pair<std::vector<uint32_t>, std::pair<int, int>>> L;
    int W = o.W, H = o.H;
    for (auto& y : o.ly) {
        std::vector<uint32_t> p; int lw = 0, lh = 0;
        if (!sm_layer(y, p, &lw, &lh)) return false;
        if (!o.W) { W = std::max(W, y.dx + lw); H = std::max(H, y.dy + lh); }
        L.push_back({std::move(p), {lw, lh}});
    }
    if (W <= 0 || H <= 0) return false;
    px.assign((size_t)W * H, 0);
    for (size_t n = 0; n < L.size(); n++) {   // layers over each other in order (straight alpha)
        int lw = L[n].second.first, lh = L[n].second.second, dx = o.ly[n].dx, dy = o.ly[n].dy;
        for (int j = 0; j < lh; j++) for (int i = 0; i < lw; i++) {
            int X = dx + i, Y = dy + j;
            if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
            uint32_t sp = L[n].first[(size_t)j * lw + i], &dp = px[(size_t)Y * W + X];
            float sa = (sp >> 24) / 255.0f, da = (dp >> 24) / 255.0f, oa = sa + da * (1 - sa);
            if (oa <= 0) { dp = 0; continue; }
            uint32_t r = (uint32_t)(oa * 255 + 0.5f) << 24;
            for (int c = 0; c < 3; c++) r |= (uint32_t)(((sp >> (8 * c) & 255) * sa + (dp >> (8 * c) & 255) * da * (1 - sa)) / oa + 0.5f) << (8 * c);
            dp = r;
        }
    }
    *w = W; *h = H;
    return true;
}
static const BhTex* sm_piece(const char* name) {
    std::string k = std::string("p:") + name;
    return sm_get(k, [n = std::string(name)](std::vector<uint32_t>& px, int* w, int* h) {
        auto it = sm_origins().find(n);
        if (it != sm_origins().end()) {
            if (sm_orig_png(it->second, px, w, h)) {
                if (sm_probe_on()) {   // TTT_SEL_PROBE: the piece from the game against the PNG of the same name, if it is still there
                    std::vector<uint32_t> q; int qw = 0, qh = 0;
                    if (!sm_file("../../games/ttt/select_bt3/" + n + ".png", q, &qw, &qh)) printf("sel81: origen %s %dx%d (sin PNG para comparar)\n", n.c_str(), *w, *h);
                    else if (qw != *w || qh != *h) printf("sel81: origen %s %dx%d, el PNG es %dx%d\n", n.c_str(), *w, *h, qw, qh);
                    else {
                        int dif = 0, mx = 0;
                        for (size_t i = 0; i < q.size(); i++) {
                            uint32_t a = px[i], b = q[i];
                            if ((a >> 24) == 0 && (b >> 24) == 0) continue;
                            int d = 0;
                            for (int c = 0; c < 32; c += 8) d = std::max(d, abs((int)(a >> c & 255) - (int)(b >> c & 255)));
                            if (d) { dif++; mx = std::max(mx, d); }
                        }
                        printf("sel81: origen %s %dx%d: %s (%d pixeles distintos, max %d)\n", n.c_str(), *w, *h, dif ? "distinto" : "igual", dif, mx);
                    }
                }
                return true;
            }
            printf("sel81: falta la pieza original %s (%s)\n", n.c_str(), it->second.ly.empty() ? "" : it->second.ly[0].path.c_str());
        }
        std::string f = "../../games/ttt/select_bt3/" + n + ".png";
        bool ok = sm_file(f, px, w, h);
        if (!ok) {
            FILE* fp = fopen(f.c_str(), "rb");
            if (fp) fclose(fp);
            if (n.compare(0, 7, "custom_") || n.compare(0, 12, "custom_title") == 0 || n == "custom_row" || n == "custom_row_on")
                printf("sel81: %s games/ttt/select_bt3/%s.png\n", fp ? "no se pudo leer (formato PNG)" : "falta", n.c_str());
        }
        return ok;
    });
}
static const BhTex* sm_small(int menu) {
    return sm_get("s:" + std::to_string(menu), [menu](std::vector<uint32_t>& px, int* w, int* h) { return psp_sel_small(menu, px, w, h); });
}
static const BhTex* sm_big(int menu, int col) {
    int s = s3d_sys_of_menu(menu);
    if (s < 0) return nullptr;
    return sm_get("b:" + std::to_string(s) + ":" + std::to_string(col), [s, col](std::vector<uint32_t>& px, int* w, int* h) {
        return psp_render_big_gen(s, col, px, w, h) || (col && psp_render_big_gen(s, 0, px, w, h)) || sm_file(psp_big_png(s), px, w, h);
    }, true);
}
static const BhTex* sm_map(int st) {
    return sm_get("m:" + std::to_string(st), [st](std::vector<uint32_t>& px, int* w, int* h) {
        if (psp_sel_map_big(st, px, w, h)) return true;
        std::string dir = psp_assets_dir() + "/Shared/" + psp_shared_rel("Menus/map_select/map_renders_big.rhg"), best;
        int want = st >= 0 && st < 10 ? st + 1 : st >= 10 && st < 0x24 ? st - 10 + 12 : 11;
        for (int pass = 0; pass < 2 && best.empty(); pass++, want = 11) {
            std::error_code ec;
            for (auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec)) {
                std::string n = e.path().filename().u8string();
                if (n.size() < 5 || !isdigit((unsigned char)n[0]) || atoi(n.c_str()) != want || n.compare(n.size() - 4, 4, ".png")) continue;
                if (best.empty() || n.find('@') != std::string::npos) best = e.path().u8string();
            }
        }
        if (best.empty()) printf("sel81: sin render de mapa %d en %s\n", st, dir.c_str());
        return sm_file(best, px, w, h);
    });
}
static std::string sm_u16(uint32_t p) {
    std::string u;
    for (int i = 0; p && p < 0x10000000u && i < 64; i++) {
        uint32_t ch = psp_l16(p + 2u * (uint32_t)i);
        if (!ch) break;
        if (ch < 0x80) u += (char)ch;
        else if (ch < 0x800) { u += (char)(0xC0 | (ch >> 6)); u += (char)(0x80 | (ch & 63)); }
        else { u += (char)(0xE0 | (ch >> 12)); u += (char)(0x80 | ((ch >> 6) & 63)); u += (char)(0x80 | (ch & 63)); }
    }
    return u;
}
static int sm_lang() { return psp_window_language() == 3 ? 1 : 0; }
static std::string sm_name0(const Ctx& c, int menu, int l);
static std::string sm_name(const Ctx& c, int menu) {
    int l = sm_lang();
    static std::map<int, std::string> memo;
    auto it = memo.find(menu * 2 + l);
    if (it != memo.end()) return it->second;
    std::string r = sm_name0(c, menu, l);
    if (!r.empty()) memo[menu * 2 + l] = r;
    return r;
}
static std::string sm_name0(const Ctx& c, int menu, int l) {
    if (menu >= 0 && menu < roster::K_OLD) {
        const roster::Rename* rn = roster::renamed(roster::VANILLA[menu]);
        if (rn && !rn->name[l].empty()) return rn->name[l];
        int k = (int)psp_l8(0x08A69720u + roster::VANILLA[menu]);
        uint32_t id = k < roster::K_OLD && roster::name_orig()[k] ? roster::name_orig()[k] : psp_l32(0x08A70E90u + 4u * (uint32_t)k);
        return id ? sm_u16(mpm_call(c, 0x0883EE74u, id, 0xFFFFFFFFu)) : std::string();
    }
    for (auto& ch : roster::defs().chars) if (roster::menu_of(ch.sysid) == menu) return !ch.name[l].empty() ? ch.name[l] : ch.name[0];
    return "";
}
static std::string sm_form(int menu) {
    if (menu < 0) return "";
    int id = -1;
    if (menu >= 0 && menu < roster::K_OLD) {
        const roster::Rename* rn = roster::renamed(roster::VANILLA[menu]);
        id = rn && rn->form != -2 ? rn->form : psp_form_row(menu);
    } else for (auto& ch : roster::defs().chars) if (roster::menu_of(ch.sysid) == menu) {
        id = ch.form;
        if (id == -2 && ch.parent >= 0) return sm_form(roster::menu_of(ch.parent) >= 0 ? roster::menu_of(ch.parent) : roster::rank_vanilla(ch.parent));
    }
    return id >= 0 ? roster::form_text(id, sm_lang()) : std::string();
}
// a text texture (font of the select's names), cell cw x ch at 2x, aligned 0 left, 1 centre, 2 right
static const BhTex* sm_text(const std::string& txt, int kind, int cw, int ch, int al) {
    static std::map<std::string, BhTex> tx;
    static ft::Font f[2] = {ft::load("../../games/ttt/fonts/char_name"), ft::load("../../games/ttt/fonts/char_form/right")};
    if (txt.empty() || !f[kind].ok) return nullptr;
    std::string key = std::to_string(kind) + "|" + std::to_string(cw) + "|" + std::to_string(al) + "|" + txt;
    auto it = tx.find(key);
    if (it != tx.end()) return it->second.a ? &it->second : nullptr;
    if (tx.size() >= 48) { for (auto& e : tx) if (e.second.a) psp_free(e.second.a); tx.clear(); }
    ft::Img im = ft::compose(f[kind], txt, cw, ch, 2, al, 2);
    SmImg e; e.px = im.px; e.w = im.w; e.h = im.h;
    BhTex t{0, 0, 0};
    if (e.w > 0 && e.h > 0 && e.w <= 1024 && e.h <= 512) { sm_upload(e); t = e.t; }
    return &(tx[key] = t);
}
static void sm_at(const BhTex* t, float x, float y, float w = -1, float h = -1, uint32_t col = 0xFFFFFFFFu, bool flip = false) {
    if (!t) return;
    if (w < 0) w = (float)t->ow;
    if (h < 0) h = (float)t->oh;
    bh_quad(t, flip ? (float)t->ow : 0, flip ? 0 : (float)t->ow, 0, (float)t->oh, x, x + w, y, y + h, col);
}
static void sm_text_at(const BhTex* t, float x0, float y0, float cw, float ch, uint32_t col = 0xFFFFFFFFu) {   // the 2x text image in a cw x ch cell
    if (t) bh_quad(t, 0, (float)t->ow, 0, (float)t->oh, x0, x0 + cw, y0, y0 + ch, col);
}
static void sm_qc(const BhTex* t, float u0, float u1, float v0, float v1, float x0, float x1, float y0, float y1, uint32_t col, float cx0, float cx1, float cy0, float cy1) {
    if (!t || x1 <= x0 || y1 <= y0) return;
    float ku = (u1 - u0) / (x1 - x0), kv = (v1 - v0) / (y1 - y0);
    if (x0 < cx0) { u0 += (cx0 - x0) * ku; x0 = cx0; }
    if (x1 > cx1) { u1 -= (x1 - cx1) * ku; x1 = cx1; }
    if (y0 < cy0) { v0 += (cy0 - y0) * kv; y0 = cy0; }
    if (y1 > cy1) { v1 -= (y1 - cy1) * kv; y1 = cy1; }
    if (x1 > x0 && y1 > y0) bh_quad(t, u0, u1, v0, v1, x0, x1, y0, y1, col);
}
static float sm_cl(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static uint32_t sm_a(float a, uint32_t rgb = 0xFFFFFFu) { return (uint32_t)(sm_cl(a) * 255.0f + 0.5f) << 24 | (rgb & 0xFFFFFFu); }
static const BhTex* sm_white() {
    return sm_get("w", [](std::vector<uint32_t>& px, int* w, int* h) { px.assign(64, 0xFFFFFFFFu); *w = *h = 8; return true; });
}
// layout in 480x272 units, room left at the bottom for the button bar (y 251)
static const float SM_TOP = 0, SM_BOT = 211, SM_NAME = 164, SM_FORMY = 191, SM_RY0 = 40, SM_RY1 = 209, SM_BAR = 251;
static float sm_alpha[2];
static int sm_akey[2] = {-1, -1};
static float sm_ent(float t0, float len) { return sm_t0 < 0 ? 1.0f : sm_cl((sm_now - sm_t0 - t0) / len); }   // main movie entrance
static void sm_side_render(int k, uint32_t dvb) {   // big render of the side over the map, in its half; fades in on change (BT3 face +0.075 a frame)
    SmSide& d = sm.s[k];
    const RosterCell& cl = sm_cell(d.pos);
    int f = d.form < cl.n ? d.form : 0, m = cl.ids[f], sd = d.team;
    bool top = sd == 0;
    if (sm_random(d.pos) || (sm_t0 >= 0 && sm_now - sm_t0 < 14)) return;
    int col = d.st >= SM_COLOR ? d.col + (d.dmg == 2 ? 64 : 0) : 0, key = m * 256 + col;
    const BhTex* b = sm_big(m, col);
    if (key != sm_akey[sd]) { sm_akey[sd] = key; sm_alpha[sd] = 0; }
    if (!b) return;
    sm_alpha[sd] = std::min(1.0f, sm_alpha[sd] + 0.075f * (float)dvb);
    float sl = (1.0f - sm_ent(14, 15)) * 240.0f * (top ? -1.0f : 1.0f);
    float hh = SM_RY1 - SM_RY0, ww = hh * b->ow / (float)b->oh, cx = (top ? 120.0f : 360.0f) + sl;
    float x0 = cx - ww / 2, x1 = cx + ww / 2, lo = top ? 0.0f : 241.0f, hi = top ? 239.0f : 480.0f;
    float u0 = 0, u1 = (float)b->ow, k0 = (float)b->ow / ww;
    if (x0 < lo) { u0 += (lo - x0) * k0; x0 = lo; }
    if (x1 > hi) { u1 -= (x1 - hi) * k0; x1 = hi; }
    if (x1 <= x0) return;
    if (!top) { float t = u0; u0 = (float)b->ow - u1; u1 = (float)b->ow - t; std::swap(u0, u1); }
    bh_quad(b, u0, u1, 0, (float)b->oh, x0, x1, SM_RY0, SM_RY1, sm_a(sm_alpha[sd]));
}
// chip scale of the BT3 chip sprite: fl_on_start 1.0 -> 1.2 in 4 frames, fl_on_loop 1.2 -> 1.1 -> 1.2 every 50, fl_off_start 1.16 -> 1.0,
// fl_ok chip 1.2 -> 1.1 -> 1.2 and cursor 1.2 -> 1.0 -> 1.2 in 10 frames, then held
static float sm_on_s(float t) {
    if (t < 4) return 1.0f + 0.05f * t;
    float u = fmodf(t - 4, 50.0f);
    return u <= 24 ? 1.2f - 0.1f * u / 24 : 1.1f + 0.1f * (u - 24) / 26;
}
static void sm_chip_s(const SmAnim& a, float& sc, float& scur) {
    if (a.ok >= 0 && a.ok >= a.on) {
        float t = sm_now - a.ok;
        if (t >= 10) { sc = scur = 1.2f; return; }
        sc = t <= 5 ? 1.2f - 0.02f * t : 1.1f + 0.02f * (t - 5);
        scur = t <= 5 ? 1.2f - 0.04f * t : 1.0f + 0.04f * (t - 5);
        return;
    }
    sc = scur = a.on >= 0 ? sm_on_s(sm_now - a.on) : 1.0f;
}
// both boxes: 0 colour (BT3 custom window: fl_custom_in / fl_custom_cansel), 1 customize (BT3 colour window: fl_color_in / fl_color_cansel /
// fl_color_ok); shown amount 0..1 = offset toward the side's edge and alpha
static float sm_box_q(const SmSide& d, const SmAnim& a, int b) {
    float t = sm_now - a.box;
    if (a.box < 0 || t > 30) return (b == 0 && d.st == SM_COLOR) || (b == 1 && d.st == SM_CUSTOM) ? 1.0f : 0.0f;
    switch (a.bev) {
    case SB_COL_IN: return b ? 0 : sm_cl((t + 1) / 15);
    case SB_COL_OUT: return b ? 0 : sm_cl(1 - t / 14);
    case SB_SWAP: return b ? (t < 9 ? 0 : sm_cl((t - 8) / 15)) : (t < 9 ? 1 : sm_cl(1 - (t - 8) / 15));
    case SB_SWAP_BACK: return b ? sm_cl(1 - t / 14) : sm_cl(t / 14);
    case SB_CUS_OK: return b ? (t < 10 ? 1 : t >= 24 ? 0 : sm_cl(1 - (t - 9) / 15)) : 0;
    case SB_CUS_IN: return b ? sm_cl((t + 1) / 15) : 0;
    }
    return 0;
}
// boxes at SM_BS of the BT3 size, anchored to the side's outer edge; inner layout in 1x units scaled from the box origin
static const float SM_BS = 0.72f, SM_BW = 224 * SM_BS;
static void sm_bq(const BhTex* t, float bx, float by, float x, float y, float w, float h, uint32_t col) {
    if (!t) return;
    if (w < 0) { w = (float)t->ow; h = (float)t->oh; }
    sm_at(t, bx + x * SM_BS, by + y * SM_BS, w * SM_BS, h * SM_BS, col);
}
static void sm_bt(const BhTex* t, float bx, float by, float x, float y, float w, float h, uint32_t col) { sm_text_at(t, bx + x * SM_BS, by + y * SM_BS, w * SM_BS, h * SM_BS, col); }
static void sm_box_title(const char* a, const char* b, float bx, float by, uint32_t col) {   // titles at the colour title's 26 high, own aspect
    const BhTex* t = sm_piece(sm_lang() ? b : a);
    if (t) sm_bq(t, bx, by, 4, 4, 26.0f * t->ow / t->oh, 26, col);
}
static void sm_box_col(const Ctx& c, int k, float bx, float al) {
    SmSide& d = sm.s[k];
    const RosterCell& cl = sm_cell(d.pos);
    int m = cl.ids[d.form < cl.n ? d.form : 0], l = sm_lang();
    uint32_t ent = 0, ca = sm_a(al);
    int av = net_online_ranked() ? 0 : cs_av_of(c, m, d.col, &ent) & 6, nc = sm_ncol(m);
    float by = 46, bh = av ? 110.0f : 76.0f;
    sm_bq(sm_piece("color_box"), bx, by, 0, 0, 224, bh, ca);
    sm_box_title("color_title_en", "color_title_es", bx, by, ca);
    float wob = (sm_frame / 8) & 1 ? 2.0f : 0.0f;
    for (int row = 0; row < (av ? 2 : 1); row++) {
        float rx = 8, ry = 38 + 34 * row;
        bool on = d.foc == row && d.st == SM_COLOR;
        sm_bq(sm_piece(on ? "color_row_on" : "color_row"), bx, by, rx, ry, -1, -1, ca);
        if (on) { sm_bq(sm_piece("arrow_l"), bx, by, rx + 4 + wob, ry + 7, -1, -1, ca); sm_bq(sm_piece("arrow_r"), bx, by, rx + 182 - wob, ry + 7, -1, -1, ca); }
        std::string t;
        if (!row) {
            std::string cn = ent && nc > 1 ? psp_colour_name(ent, l) : std::string();
            char b[40]; snprintf(b, sizeof b, "Color %d", d.col + 1);
            t = cn.empty() ? std::string(b) : cn;
        } else {
            bool two = (av & 6) == 6;
            t = d.dmg == 0 ? "Normal" : two ? (l ? (d.dmg == 1 ? "Dañado 1" : "Dañado 2") : (d.dmg == 1 ? "Damaged 1" : "Damaged 2")) : (l ? "Dañado" : "Damaged");
        }
        sm_bt(sm_text(t, 1, 160, 16, 1), bx, by, rx + 24, ry + 7, 160, 16, ca);
    }
}
static void sm_box_cus(int k, float bx, float al) {   // the game's customize page (state 4): Default, Custom 1..3; original rows and labels when present
    SmSide& d = sm.s[k];
    int l = sm_lang();
    uint32_t ca = sm_a(al);
    float by = 42;
    sm_bq(sm_piece("color_box"), bx, by, 0, 0, 224, 168, ca);
    sm_box_title("custom_title_en", "custom_title_es", bx, by, ca);
    for (int row = 0; row < 4; row++) {
        float rx = 8, ry = 36 + 32 * row;
        bool on = d.cus == row;
        const BhTex* rp = sm_piece(on ? "custom_row_on" : "custom_row");
        sm_bq(rp ? rp : sm_piece(on ? "color_row_on" : "color_row"), bx, by, rx, ry, 208, 30, ca);
        char nm[32];
        snprintf(nm, sizeof nm, "custom_opt%d%s_%s", row, on ? "_on" : "", l ? "es" : "en");
        if (rp) if (const BhTex* t = sm_piece(nm)) { float h = 18, w = h * t->ow / t->oh; if (w > 196) { w = 196; h = w * t->oh / t->ow; } sm_bq(t, bx, by, rx + 104 - w / 2, ry + 15 - h / 2, w, h, ca); continue; }
        char b[40];
        if (!row) snprintf(b, sizeof b, "%s", l ? "Predeterminado" : "Default");
        else snprintf(b, sizeof b, "%s %d", l ? "Personalizado" : "Custom", row);
        sm_bt(sm_text(b, 1, 160, 16, 1), bx, by, rx + 24, ry + 7, 160, 16, ca);
    }
}
static void sm_side_draw(const Ctx& c, int k) {
    SmSide& d = sm.s[k];
    SmAnim& a = sm_an[k];
    const RosterCell& cl = sm_cell(d.pos);
    int f = d.form < cl.n ? d.form : 0, m = cl.ids[f], n = sm_cells(), l = sm_lang();
    bool top = d.team == 0, rnd = sm_random(d.pos);
    float dir = top ? -1.0f : 1.0f;
    bool waiting = d.st != SM_DONE && !sm_act(k);   // a slot whose turn has not come: reel off, no cursor
    if (a.wait && !waiting) a.on = sm_now;
    a.wait = waiting;
    // name and form plates (main movie: plates in 8 frames from 14, texts in 7 from 22; form in yellow, inside the plate)
    float pd = (1 - sm_ent(14, 8)) * 240 * dir, te = sm_ent(22, 7), td = (1 - te) * 194 * dir;
    sm_at(sm_piece(top ? "name_plate_l" : "name_plate_r"), (top ? 0 : 248) + pd, SM_NAME);
    sm_at(sm_piece(top ? "form_plate_l" : "form_plate_r"), (top ? 0 : 272) + pd, SM_FORMY);
    std::string nm = rnd ? (l ? "Aleatorio" : "Random") : sm_name(c, m), fm = rnd ? "" : sm_form(m);
    if (te > 0) {
        sm_text_at(sm_text(nm, 0, 200, 22, top ? 2 : 0), (top ? 20 : 260) + td, SM_NAME + 2, 200, 22, sm_a(te));
        if (!fm.empty()) if (const BhTex* t = sm_text(fm, 1, 170, 14, top ? 2 : 0))
            bh_quad(t, 0, (float)t->ow, 0, (float)t->oh, (top ? 20.0f : 290.0f) + td, (top ? 190.0f : 460.0f) + td, SM_FORMY + 2, SM_FORMY + 16, sm_a(te, 0x00F0FFu));
    }
    // reel (fl_in: 14 frames from the side's edge; fl_out: 14 frames, then 15 to the edge; gone while DONE)
    float rq = 1;
    if (a.out >= 0) { float t = sm_now - a.out; rq = t < 14 ? 1 : sm_cl(1 - (t - 14) / 15); }
    else if (a.in >= 0) rq = sm_cl((sm_now - a.in) / 14);
    if (d.st == SM_DONE && (a.out < 0 || sm_now - a.out >= 29)) rq = 0;
    if (rq > 0) {
        float rx = (1 - rq) * 488 * dir, sy = top ? SM_TOP : SM_BOT;
        uint32_t ca = sm_a(rq);
        sm_at(sm_piece(waiting ? "strip_off" : top ? "strip_p1" : "strip_p2"), rx, sy, -1, -1, ca);
        char tg[16];
        if (d.human) snprintf(tg, sizeof tg, "tag_p%d_%s", d.pad + 1, l ? "es" : "en"); else snprintf(tg, sizeof tg, "tag_com");
        if (const BhTex* t = sm_piece(tg)) {
            float h = 24, w = h * t->ow / t->oh;
            if (w > 60) { w = 60; h = w * t->oh / t->ow; }
            float cx = (top ? 35.0f : 445.0f) + rx, cy = sy + 20;
            sm_at(t, cx - w / 2, cy - h / 2, w, h, ca);
        }
        // chips: fl_reel_up/down = rows slide one strip in 8 frames; fl_form = window closes right to left in 17 frames, contents swap, opens in 17
        float L = (top ? 70.0f : 24.0f) + rx, R = L + 56 * 7, cx0 = -1e4f, cx1 = 1e4f, cy0 = -1e4f, cy1 = 1e4f;
        bool forms = sm_forms(d), plain = false;
        float wt = sm_now - a.wipe;
        if (a.wipe >= 0 && wt < 34) {
            cx1 = wt < 17 ? R - (R - L) * wt / 17 : L + (R - L) * (wt - 17) / 17;
            if (wt < 18) { forms = a.wforms; plain = true; }
        }
        float rt = sm_now - a.row, dy = 0;
        bool sliding = a.row >= 0 && rt < 8 && !forms && !plain;
        if (sliding) { dy = a.rdir * 40 * (1 - rt / 8); cy0 = sy; cy1 = sy + 40; }
        float sc, scur;
        sm_chip_s(a, sc, scur);
        int cur = forms ? f : d.pos % 7;
        auto chip = [&](int mi, int i, float y, float s, float s2, bool cursor) {
            float x = L + 56 * i, ccx = x + 25, ccy = y + 17;
            auto q = [&](const BhTex* t, float x0, float y0, float w, float h, float ss) {
                if (!t) return;
                if (w < 0) { w = (float)t->ow; h = (float)t->oh; }
                float X0 = ccx + (x0 - ccx) * ss, X1 = ccx + (x0 + w - ccx) * ss, Y0 = ccy + (y0 - ccy) * ss, Y1 = ccy + (y0 + h - ccy) * ss;
                sm_qc(t, 0, (float)t->ow, 0, (float)t->oh, X0, X1, Y0, Y1, ca, cx0, cx1, cy0, cy1);
            };
            if (mi == -1) q(sm_piece("cell_x"), x + 2, y + 2, -1, -1, s);
            else q(sm_small(mi), x + 2, y + 2, 46, 30, s);
            q(sm_piece("cell_frame"), x, y, -1, -1, s);
            if (cursor) q(sm_piece("cursor"), x - 2, y - 2, -1, -1, s2);
        };
        auto row_mi = [&](bool fr, int r0, int i) { return fr ? (i < cl.n ? cl.ids[i] : -1) : (r0 + i < n ? sm_cell(r0 + i).ids[0] : -2); };
        int r0 = forms ? 0 : d.pos / 7 * 7;
        if (plain && !forms) r0 = d.pos / 7 * 7;
        if (sliding) for (int i = 0; i < 7; i++) { int mi = row_mi(false, a.rold, i); if (mi != -2) chip(mi, i, sy + 3 + dy - a.rdir * 40, 1, 1, false); }
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < 7; i++) {
                bool isc = i == cur && !plain && !waiting;
                if ((pass == 1) != isc) continue;
                int mi = row_mi(forms, r0, i);
                if (mi == -2) continue;
                float s = 1;
                if (isc) s = sc;
                else if (!plain && !waiting && i == a.offi && a.off >= 0 && sm_now - a.off < 4) s = 1.16f - 0.04f * (sm_now - a.off);
                chip(mi, i, sy + 3 + dy, s, scur, isc);
            }
    }
    // colour and customize boxes slide 232 toward the side's edge with their alpha
    float q0 = sm_box_q(d, a, 0), q1 = sm_box_q(d, a, 1), bx = top ? 4.0f : 476.0f - SM_BW, tr = SM_BW + 8;
    if (q0 > 0.01f) sm_box_col(c, k, bx + (1 - q0) * tr * dir, q0);
    if (q1 > 0.01f) sm_box_cus(k, bx + (1 - q1) * tr * dir, q1);
}
static bool sm_draw(const Ctx& c) {
    if (sm_masked && !sm.on && sm_gp(sm.w) && c.r[4] == psp_l32(sm.w + 0x84)) return true;   // grid page: nothing until the port's menu starts
    if (!sm.on || c.r[4] != psp_l32(sm.w + 0x84)) return false;
    sm_frame++;
    static uint32_t lvb = 0;
    uint32_t vb = psp_l32(0x08A78E14u), dvb = lvb && vb > lvb && vb - lvb < 8 ? vb - lvb : 1;
    lvb = vb;
    sm_now += (float)dvb;
    if (!bh_vbuf) bh_vbuf = psp_reserve(4 * 0x8000);
    static bool said = false;
    if (!said) { said = true; printf("sel81: dibujo del menu BT3 activo\n"); }
    bh_begin();
    if (const BhTex* t = sm_map((int)psp_l32(sm.w + 0xB8 + 0x214))) {   // the map's big render behind, down to the button bar
        float a = (float)t->ow / t->oh, sa = 480.0f / SM_BAR, u0 = 0, u1 = (float)t->ow, v0 = 0, v1 = (float)t->oh;
        if (a > sa) { float cw = t->oh * sa; u0 = (t->ow - cw) / 2; u1 = u0 + cw; } else { float chh = t->ow / sa; v0 = (t->oh - chh) / 2; v1 = v0 + chh; }
        bh_quad(t, u0, u1, v0, v1, 0, 480, 0, SM_BAR);
    }
    for (int sd = 0; sd < 2; sd++) sm_side_render(sm.vis[sd], dvb);
    if (const BhTex* wt = sm_white()) bh_quad(wt, 0, 8, 0, 8, 239, 241, 0, SM_BAR, 0xFF000000u);   // dividing line between the sides
    for (int sd = 0; sd < 2; sd++) sm_side_draw(c, sm.vis[sd]);
    bh_end();
    return true;
}
