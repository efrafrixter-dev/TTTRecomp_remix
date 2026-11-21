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
struct SmSide { int row = -1, pad = 0, st = SM_GRID, pos = 0, form = 0, col = 0, dmg = 0, foc = 0, cus = 0; bool human = false; };
struct SmMenu { bool on = false, ret = false; uint32_t w = 0, drv = 0; int last = -1, shown = 0, logs = 0, hide = 0; SmSide s[2]; };
static SmMenu sm;
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
    for (int j = 0; j < 2; j++) if (sm.s[j].pad == 0) { if (first < 0 && sm.s[j].st != SM_DONE) first = j; last = j; }
    return k == (first >= 0 ? first : last);
}
static int sm_prev0(int k) {
    for (int j = k - 1; j >= 0; j--) if (sm.s[j].pad == 0) return j;
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
    printf("sel81: los dos lados listos (filas %d %d): abre la seleccion de mapa\n", sm.s[0].row, sm.s[1].row);
    mpm_call(c, psp_l32(vt + 0xCC), pg + (uint32_t)(int16_t)psp_l16(vt + 0xC8), 0, o);
}
static void sm_back_out(Ctx& c) {
    uint32_t o = sm.drv, pg = psp_l32(o + 0x12C), vt = sm_gp(pg) ? psp_l32(pg + 0x40) : 0;
    sm.on = false;
    printf("sel81: atras desde J1: vuelve como el original\n");
    if (vt) mpm_call(c, psp_l32(vt + 0xCC), pg + (uint32_t)(int16_t)psp_l16(vt + 0xC8), 1, o);
}
static void sm_input(Ctx& c, int k) {
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
            int j = sm_prev0(k);
            if (d.pad == 0 && j >= 0) { mpm_sfx(c, 1, 0); sm.s[j].st = SM_CUSTOM; sm_show(j); sm_write(c, j); sm_log(j, "vuelve"); }
            else if (k == 0 || (d.pad == 0 && j < 0)) { mpm_sfx(c, 1, 0); sm_back_out(c); }
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
static bool sm_begin(uint32_t w, uint32_t o) {
    int rows[2] = {-1, -1}, cnt[2] = {0, 0};
    uint32_t b = w + 0xB8;
    for (int r = 0; r < 4; r++) {
        uint32_t row = b + 4 + 0x6C * r;
        if (!psp_l32(row + 0x44)) continue;
        int t = psp_l32(row + 0x40) ? 1 : 0;
        if (!cnt[t]++) rows[t] = r;
    }
    if (cnt[0] != 1 || cnt[1] != 1) {
        static uint32_t lw = 0;
        if (lw != w) { lw = w; printf("sel81: estilo BT3 sin soporte para %d contra %d todavia: seleccion original\n", cnt[0], cnt[1]); }
        return false;
    }
    sm.on = true; sm.w = w; sm.drv = o; sm.shown = 0;
    for (int k = 0; k < 2; k++) {
        SmSide& d = sm.s[k];
        int r = rows[k], pl = lmp_cfg_on ? mpm_row_player(r) : (k == 0 ? 0 : -1);
        if (d.row != r || !sm.ret) d = SmSide();
        d.row = r; d.human = pl >= 0; d.pad = pl >= 0 ? pl : 0;
        uint32_t rec = sm_rec(r);
        if (!sm.ret) {
            uint32_t cell = psp_l32(rec);
            d.pos = cell == 99 ? 0 : sm_pos_of_cell(cell);
            d.st = SM_GRID;
        } else d.st = SM_DONE;
    }
    if (sm.ret) for (int k = 1; k >= 0; k--) if (sm.s[k].pad == 0 || sm.s[k].human) { sm.s[k].st = SM_CUSTOM; if (sm.s[k].pad == 0) break; }
    sm.ret = false;
    sm.hide = 8;
    if (sm_rng == 0x9E3779B9u) sm_rng ^= (uint32_t)psp_time_us();
    printf("sel81: estilo BT3 activo en W 0x%08X: lado 0 fila %d (%s), lado 1 fila %d (%s)\n", w, sm.s[0].row, sm.s[0].human ? "humano" : "COM", sm.s[1].row, sm.s[1].human ? "humano" : "COM");
    return true;
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
            bool turn[2] = {sm_turn(0), sm_turn(1)};   // decided before any input: a confirm that ends J1's turn is not read again by the COM side
            for (int k = 0; k < 2 && sm.on; k++) if (turn[k]) { if (k0 < 0) k0 = k; sm_input(c, k); }
            if (sm.on && sm.s[0].st == SM_DONE && sm.s[1].st == SM_DONE) sm_finish(c);
            else if (sm.on && k0 >= 0 && !sm.s[k0].human && sm.shown != k0) sm_show(k0);
            if (sm_probe_on()) sm_probe(o);
        }
        return;
    }
    orig(c);
    if (sm_probe_on()) sm_probe(o);
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
    if (now == 1 && sm_begin(w, o)) { for (int k = 0; k < 2; k++) sm_write(c, k); sm_show(0); }
}

// ---- drawing (GE sprites in the menu's 2D list, in place of the grid page's draw 0x089F9F34): pieces games/ttt/select_bt3, renders, texts
bool psp_sel_small(int menu, std::vector<uint32_t>& px, int* w, int* h);
bool psp_sel_map_big(int map, std::vector<uint32_t>& px, int* w, int* h);
int psp_form_row(int menu);
std::string psp_assets_dir();
std::string psp_shared_rel(const std::string& legacy);
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
        bool ok = j.second(px, &w, &h) && w > 0 && h > 0 && w <= 1024 && h <= 1024 && px.size() >= (size_t)w * h;
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
static const BhTex* sm_piece(const char* name) {
    std::string k = std::string("p:") + name;
    return sm_get(k, [n = std::string(name)](std::vector<uint32_t>& px, int* w, int* h) {
        bool ok = sm_file("../../games/ttt/select_bt3/" + n + ".png", px, w, h);
        if (!ok) printf("sel81: falta games/ttt/select_bt3/%s.png\n", n.c_str());
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
static void sm_text_at(const BhTex* t, float x0, float y0, float cw, float ch) {   // the 2x text image in a cw x ch cell
    if (t) bh_quad(t, 0, (float)t->ow, 0, (float)t->oh, x0, x0 + cw, y0, y0 + ch);
}
static const BhTex* sm_white() {
    return sm_get("w", [](std::vector<uint32_t>& px, int* w, int* h) { px.assign(64, 0xFFFFFFFFu); *w = *h = 8; return true; });
}
// layout in 480x272 units, room left at the bottom for the button bar (y 251)
static const float SM_TOP = 0, SM_BOT = 211, SM_NAME = 164, SM_FORMY = 191, SM_RY0 = 40, SM_RY1 = 209, SM_BAR = 251;
static float sm_alpha[2];
static int sm_akey[2] = {-1, -1};
static void sm_side_render(int k, uint32_t dvb) {   // big render of the side over the map, in its half; fades in on change (BT3 face +0.075 a frame)
    SmSide& d = sm.s[k];
    const RosterCell& cl = sm_cell(d.pos);
    int f = d.form < cl.n ? d.form : 0, m = cl.ids[f];
    bool top = k == 0;
    if (sm_random(d.pos)) return;
    int col = d.st >= SM_COLOR ? d.col + (d.dmg == 2 ? 64 : 0) : 0, key = m * 256 + col;
    const BhTex* b = sm_big(m, col);
    if (key != sm_akey[k]) { sm_akey[k] = key; sm_alpha[k] = 0; }
    if (!b) return;
    sm_alpha[k] = std::min(1.0f, sm_alpha[k] + 0.075f * (float)dvb);
    float hh = SM_RY1 - SM_RY0, ww = hh * b->ow / (float)b->oh, cx = top ? 120.0f : 360.0f;
    float x0 = cx - ww / 2, x1 = cx + ww / 2, lo = top ? 0.0f : 241.0f, hi = top ? 239.0f : 480.0f;
    float u0 = 0, u1 = (float)b->ow, k0 = (float)b->ow / ww;
    if (x0 < lo) { u0 += (lo - x0) * k0; x0 = lo; }
    if (x1 > hi) { u1 -= (x1 - hi) * k0; x1 = hi; }
    if (!top) { float t = u0; u0 = (float)b->ow - u1; u1 = (float)b->ow - t; std::swap(u0, u1); }
    bh_quad(b, u0, u1, 0, (float)b->oh, x0, x1, SM_RY0, SM_RY1, (uint32_t)(sm_alpha[k] * 255) << 24 | 0xFFFFFFu);
}
static void sm_side_draw(const Ctx& c, int k) {
    SmSide& d = sm.s[k];
    const RosterCell& cl = sm_cell(d.pos);
    int f = d.form < cl.n ? d.form : 0, m = cl.ids[f], n = sm_cells(), l = sm_lang();
    bool top = k == 0, rnd = sm_random(d.pos);
    bool solo = (sm.s[0].human || sm.s[1].human) && !sm.s[1].human;   // one pad: J1's reel goes away on the COM turn
    bool hidden = top && solo && sm.s[0].st == SM_DONE && sm.s[1].st != SM_DONE;
    bool waiting = !top && solo && sm.s[0].st != SM_DONE;
    // name and form plates (form in yellow, inside the plate before its slanted end)
    sm_at(sm_piece(top ? "name_plate_l" : "name_plate_r"), top ? 0 : 248, SM_NAME);
    sm_at(sm_piece(top ? "form_plate_l" : "form_plate_r"), top ? 0 : 272, SM_FORMY);
    std::string nm = rnd ? (l ? "Aleatorio" : "Random") : sm_name(c, m), fm = rnd ? "" : sm_form(m);
    sm_text_at(sm_text(nm, 0, 200, 22, top ? 2 : 0), top ? 20 : 260, SM_NAME + 2, 200, 22);
    if (!fm.empty()) if (const BhTex* t = sm_text(fm, 1, 170, 14, top ? 2 : 0)) bh_quad(t, 0, (float)t->ow, 0, (float)t->oh, top ? 20.0f : 290.0f, top ? 190.0f : 460.0f, SM_FORMY + 2, SM_FORMY + 16, 0xFF00F0FFu);
    if (hidden) return;
    // reel: the cursor's row, or the cell's forms from the form reel on (unused positions X)
    float sy = top ? SM_TOP : SM_BOT;
    sm_at(sm_piece(waiting ? "strip_off" : top ? "strip_p1" : "strip_p2"), 0, sy);
    bool forms = d.st >= SM_FORM && cl.n > 1 && !rnd;
    int r0 = forms ? 0 : d.pos / 7 * 7, cur = forms ? f : d.pos % 7;
    for (int i = 0; i < 7; i++) {
        float x = top ? 70.0f + 56 * i : 24.0f + 56 * i, y = sy + 3;
        int mi = forms ? (i < cl.n ? cl.ids[i] : -1) : (r0 + i < n ? sm_cell(r0 + i).ids[0] : -2);
        if (mi == -2) continue;
        if (mi == -1) sm_at(sm_piece("cell_x"), x + 2, y + 2);
        else sm_at(sm_small(mi), x + 2, y + 2, 46, 30);
        sm_at(sm_piece("cell_frame"), x, y);
        if (i == cur && !waiting) sm_at(sm_piece("cursor"), x - 2, y - 2);
    }
    char tg[16];
    if (d.human) snprintf(tg, sizeof tg, "tag_p%d_%s", d.pad + 1, l ? "es" : "en"); else snprintf(tg, sizeof tg, "tag_com");
    if (const BhTex* t = sm_piece(tg)) {
        float h = 24, w = h * t->ow / t->oh;
        if (w > 60) { w = 60; h = w * t->oh / t->ow; }
        float cx = top ? 35.0f : 445.0f, cy = sy + 20;
        sm_at(t, cx - w / 2, cy - h / 2, w, h);
    }
    if (d.st == SM_CUSTOM) {   // the game's customize page (state 4): Default, Custom 1..3, vertical
        float bx = top ? 8.0f : 248.0f, by = 42;
        sm_at(sm_piece("color_box"), bx, by, 224, 168);
        sm_at(sm_piece(l ? "custom_title_es" : "custom_title_en"), bx + 4, by + 4);
        for (int row = 0; row < 4; row++) {
            float rx = bx + 8, ry = by + 36 + 32 * row;
            sm_at(sm_piece(d.cus == row ? "color_row_on" : "color_row"), rx, ry);
            char b[40];
            if (!row) snprintf(b, sizeof b, "%s", l ? "Predeterminado" : "Default");
            else snprintf(b, sizeof b, "%s %d", l ? "Personalizado" : "Custom", row);
            sm_text_at(sm_text(b, 1, 160, 16, 1), rx + 24, ry + 7, 160, 16);
        }
        return;
    }
    // colour box
    if (d.st != SM_COLOR) return;
    uint32_t ent = 0;
    int av = net_online_ranked() ? 0 : cs_av_of(c, m, d.col, &ent) & 6, nc = sm_ncol(m);
    float bx = top ? 8.0f : 248.0f, by = 46, bh = av ? 110.0f : 76.0f;
    sm_at(sm_piece("color_box"), bx, by, 224, bh);
    sm_at(sm_piece(l ? "color_title_es" : "color_title_en"), bx + 4, by + 4);
    float wob = (sm_frame / 8) & 1 ? 2.0f : 0.0f;
    for (int row = 0; row < (av ? 2 : 1); row++) {
        float rx = bx + 8, ry = by + 38 + 34 * row;
        bool on = d.foc == row;
        sm_at(sm_piece(on ? "color_row_on" : "color_row"), rx, ry);
        if (on) { sm_at(sm_piece("arrow_l"), rx + 4 + wob, ry + 7); sm_at(sm_piece("arrow_r"), rx + 182 - wob, ry + 7); }
        std::string t;
        if (!row) {
            std::string cn = ent && nc > 1 ? psp_colour_name(ent, l) : std::string();
            char b[40]; snprintf(b, sizeof b, "Color %d", d.col + 1);
            t = cn.empty() ? std::string(b) : cn;
        } else {
            bool two = (av & 6) == 6;
            t = d.dmg == 0 ? "Normal" : two ? (l ? (d.dmg == 1 ? "Dañado 1" : "Dañado 2") : (d.dmg == 1 ? "Damaged 1" : "Damaged 2")) : (l ? "Dañado" : "Damaged");
        }
        sm_text_at(sm_text(t, 1, 160, 16, 1), rx + 24, ry + 7, 160, 16);
    }
}
static bool sm_draw(const Ctx& c) {
    if (!sm.on || c.r[4] != psp_l32(sm.w + 0x84)) return false;
    sm_frame++;
    static uint32_t lvb = 0;
    uint32_t vb = psp_l32(0x08A78E14u), dvb = lvb && vb > lvb && vb - lvb < 8 ? vb - lvb : 1;
    lvb = vb;
    if (!bh_vbuf) bh_vbuf = psp_reserve(4 * 0x8000);
    static bool said = false;
    if (!said) { said = true; printf("sel81: dibujo del menu BT3 activo\n"); }
    bh_begin();
    if (const BhTex* t = sm_map((int)psp_l32(sm.w + 0xB8 + 0x214))) {   // the map's big render behind, down to the button bar
        float a = (float)t->ow / t->oh, sa = 480.0f / SM_BAR, u0 = 0, u1 = (float)t->ow, v0 = 0, v1 = (float)t->oh;
        if (a > sa) { float cw = t->oh * sa; u0 = (t->ow - cw) / 2; u1 = u0 + cw; } else { float chh = t->ow / sa; v0 = (t->oh - chh) / 2; v1 = v0 + chh; }
        bh_quad(t, u0, u1, v0, v1, 0, 480, 0, SM_BAR);
    }
    for (int k = 0; k < 2; k++) sm_side_render(k, dvb);
    if (const BhTex* wt = sm_white()) bh_quad(wt, 0, 8, 0, 8, 239, 241, 0, SM_BAR, 0xFF000000u);   // dividing line between the sides
    for (int k = 0; k < 2; k++) sm_side_draw(c, k);
    bh_end();
    return true;
}
