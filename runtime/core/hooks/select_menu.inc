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
enum { SM_GRID, SM_FORM, SM_COLOR, SM_DONE };
struct SmSide { int row = -1, pad = 0, st = SM_GRID, pos = 0, form = 0, col = 0, dmg = 0, foc = 0; bool human = false; };
struct SmMenu { bool on = false, ret = false; uint32_t w = 0, drv = 0; int last = -1, shown = 0, logs = 0; SmSide s[2]; };
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
}
static void sm_log(int k, const char* what) {
    SmSide& d = sm.s[k];
    if (sm.logs++ < 300) printf("sel81: lado %d (fila %d, entrada %d%s) %s: estado %d, casilla %d, menu %d, forma %d, color %d, danado %d\n", k, d.row, d.pad,
                               d.human ? "" : ", COM", what, d.st, d.pos, sm_cell(d.pos).ids[d.form < sm_cell(d.pos).n ? d.form : 0], d.form, d.col, d.dmg);
}
// the TTT pages show the side being edited (big render, name, label and grid frame follow the record of their slot)
static void sm_show(int k) {
    sm.shown = k;
    int row = sm.s[k].row;
    for (int i = 0; i < 2; i++) { uint32_t pg = psp_l32(sm.w + 0x84 + 4 * i); if (sm_gp(pg)) psp_s32(pg + 0x68, (uint32_t)row); }
    uint32_t g = psp_l32(sm.w + 0x84);
    if (sm_gp(g)) {
        int rows = (sm_cells() + 3) / 4, top = sm.s[k].pos / 4 - 2, mx = rows - 6 > 0 ? rows - 6 : 0;
        psp_s32(g + 0x1B8, (uint32_t)(top < 0 ? 0 : top > mx ? mx : top));
    }
}
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
            int p = d.pos, step = L ? -1 : R ? 1 : U ? -7 : 7;
            for (int t = 0; t < n; t++) {
                p += step;
                if (step == 1 || step == -1) p = (p + n) % n;
                else if (p < 0 || p >= n) { p = step < 0 ? (d.pos % 7) + ((n - 1 - d.pos % 7) / 7) * 7 : d.pos % 7; if (p >= n) p = n - 1; }
                if (sm_open(p)) break;
            }
            if (p != d.pos) { d.pos = p; d.form = 0; mpm_sfx(c, 1, 3); sm_write(c, k); sm_log(k, "mueve"); }
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
            if (d.pad == 0 && j >= 0) { mpm_sfx(c, 1, 3); sm.s[j].st = SM_COLOR; sm_show(j); sm_write(c, j); sm_log(j, "vuelve"); }
            else if (k == 0 || (d.pad == 0 && j < 0)) { mpm_sfx(c, 1, 3); sm_back_out(c); }
        }
        break;
    case SM_FORM:
        if (L || R) { d.form = (d.form + (L ? cl->n - 1 : 1)) % cl->n; mpm_sfx(c, 1, 3); sm_write(c, k); sm_log(k, "forma"); }
        else if (ok) { d.st = SM_COLOR; int nc = sm_ncol(cl->ids[d.form]); if (d.col >= nc) d.col = 0; d.dmg = 0; d.foc = 0; mpm_sfx(c, 1, 6); sm_write(c, k); sm_log(k, "confirma"); }
        else if (bk) { d.st = SM_GRID; d.form = 0; mpm_sfx(c, 1, 3); sm_write(c, k); sm_log(k, "atras"); }
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
        } else if (ok) { d.st = SM_DONE; mpm_sfx(c, 1, 6); sm_write(c, k); sm_log(k, "listo"); }
        else if (bk) { d.st = cl->n > 1 && !sm_random(d.pos) ? SM_FORM : SM_GRID; d.foc = 0; mpm_sfx(c, 1, 3); sm_write(c, k); sm_log(k, "atras"); }
        break;
    }
    case SM_DONE:
        if (bk) { d.st = SM_COLOR; mpm_sfx(c, 1, 3); sm_log(k, "atras"); }
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
    if (sm.ret) for (int k = 1; k >= 0; k--) if (sm.s[k].pad == 0 || sm.s[k].human) { sm.s[k].st = SM_COLOR; if (sm.s[k].pad == 0) break; }
    sm.ret = false;
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
            int k0 = -1;
            for (int k = 0; k < 2 && sm.on; k++) if (sm_turn(k)) { if (k0 < 0) k0 = k; sm_input(c, k); }
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
