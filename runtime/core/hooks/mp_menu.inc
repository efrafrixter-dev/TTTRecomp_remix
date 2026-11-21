// Multiplayer submenu (item 20e): page 0 of scene 8 (overlay 56_38) shows Local / LAN / Online (AGS 97 of
// 57_menu_multiplayer.pak) before the WLAN room list; native page and widget methods called through port vtables
void psp_local_join(int on);
void psp_local_manual(int on);
int psp_local_free_pressed();
void psp_local_assign(int d, int k);
void psp_local_release(int k);
void psp_local_remap(const int* map);
int psp_local_dev(int k, int* family);

static const uint32_t MPM_NAT = 0x0BFF0000u;
enum { MN_PG_ENTER, MN_PG_UPDATE, MN_PG_EXIT, MN_W_LOAD, MN_W_ANIM, MN_W_DRAW, MN_W_PAK, MN_PG_DRAW, MN_PX_UPDATE, MN_PX_DRAW };
enum { MP_OFF, MP_SUB, MP_ORIG, MP_GONE };
enum { MA_NONE, MA_LAN, MA_BACK, MA_LOCAL, MA_TOLOCAL, MA_TOSUB, MA_TOONLINE, MA_TOCREATE, MA_TOROOM, MA_ROOMGO, MA_TOSTART, MA_TOSET };
enum { PH_LOAD, PH_OPEN, PH_NORMAL, PH_CLOSE };
enum { H_BG, H_LAYERS, H_FIRE, H_ROOT, H_ROW0, H_ROW1, H_ROW2, H_BOX, L_ROOT, L_ROW0, L_TEAM0 = L_ROW0 + 4, L_LABEL0 = L_TEAM0 + 4, L_BOX = L_LABEL0 + 4,
       O_ROOT, O_TAB0, O_ROW0 = O_TAB0 + 3, O_BTN0 = O_ROW0 + 6, O_UP = O_BTN0 + 2, O_DOWN, O_BOX, O_ICON0, O_TAB3 = O_ICON0 + 6, O_KROW0, O_RICON0 = O_KROW0 + 6, O_RTITLE = O_RICON0 + 6, O_RTICON, O_AV, O_OPT0,
       K_ROOT = O_OPT0 + 3, K_KEY0, K_SPEC0 = K_KEY0 + 40, C_ROOT = K_SPEC0 + 4, C_ROOTP, C_OPT0, C_BTN = C_OPT0 + 6, C_BOX, R_ROOT, R_ROW0, R_LABEL0 = R_ROW0 + 4, R_BOX = R_LABEL0 + 4,
       R_RICON, R_AV0, R_DLG = R_AV0 + 4, R_DBTN0, R_DBTN1, A_ROOT, A_TAB0, A_ROW0 = A_TAB0 + 2, A_BOX = A_ROW0 + 5, A_INFO, A_DLG, A_DBTN0, A_DBTN1, B_OPT, B_TURTLE, B_ROSHI, H_N };
enum { S_HEAD, S_GUIDE, S_SETSUMEI, S_MENU1, S_MENU2, S_MENU3, S_TEXT, S_PLAYER1, S_LABEL = S_PLAYER1 + 4, S_TEAM, S_DEVICE, S_N };
static int mpm_ags(int k) { if (k >= B_OPT) return k == B_OPT ? 181 : 188; if (k >= A_ROOT) return 160; if (k >= R_ROOT) return k == R_RICON || (k >= R_AV0 && k < R_DLG) ? 125 : k >= R_LABEL0 && k < R_BOX ? 45 : 153; if (k >= O_ROOT) return k < K_ROOT ? 125 : k < C_ROOT ? 139 : 146; return k < L_ROOT ? (k == H_BG ? 67 : k == H_LAYERS ? 69 : k == H_FIRE ? 71 : 97) : k >= L_LABEL0 && k < L_BOX ? 45 : 111; }
static int mpm_group(int k) { return k < H_ROOT ? 0 : k < L_ROOT ? 1 : k < O_ROOT ? 2 : k < K_ROOT ? 3 : k < C_ROOT ? 4 : k < R_ROOT ? 5 : k < R_DLG ? 6 : k < A_ROOT ? 7 : k < A_DLG ? 8 : k < B_OPT ? 9 : 10; }
static bool mpm_owner(int k) { return k < O_ROOT || k == O_ROOT || k == K_ROOT || k == C_ROOT || k == R_ROOT || k == A_ROOT || k == B_OPT || k == B_TURTLE; }
// a holder without its own load shares the handle of the nearest earlier owner of the same AGS
static int mpm_src(int k) { for (int j = k - 1; j >= 0; j--) if (mpm_owner(j) && mpm_ags(j) == mpm_ags(k)) return j; return k - 1; }
// state range of each holder (one AGS instance per state)
static void mpm_range(int k, int* first, int* last) {
    static const int R[][2] = {{0, 2}, {9, 14}, {4, 8}, {15, 22}, {23, 24}, {3, 3}, {0, 2}, {4, 5}, {6, 13}, {0, 2}, {4, 15}, {10, 11}, {3, 3}, {16, 18},
                               {0, 2}, {4, 6}, {0, 3}, {3, 3}, {7, 7}, {8, 9}};
    if (k >= O_ICON0 && k < O_TAB3) { *first = 25; *last = 26; return; }
    if (k == O_TAB3) { *first = 27; *last = 28; return; }
    if (k >= O_KROW0 && k < O_RICON0) { *first = 29; *last = 31; return; }
    if ((k >= O_RICON0 && k < O_RTITLE) || k == R_RICON) { *first = 32; *last = 32; return; }
    if (k == O_RTITLE) { *first = 33; *last = 33; return; }
    if (k == O_AV) { *first = 34; *last = 34; return; }
    if (k >= O_OPT0 && k < K_ROOT) { *first = 13; *last = 14; return; }
    if (k >= R_AV0 && k < R_DLG) { *first = *last = 35 + (k - R_AV0); return; }
    if (k == O_RTICON) { *first = 25; *last = 26; return; }
    if (k == C_OPT0 + 5) { *first = 4; *last = 4; return; }
    if (k >= B_OPT) { *first = 0; *last = k == B_TURTLE ? 4 : 1; return; }
    if (k >= A_ROOT) { static const int A[7][2] = {{0, 2}, {4, 5}, {6, 9}, {3, 3}, {10, 10}, {11, 12}, {13, 13}}; int g = k == A_ROOT ? 0 : k < A_ROW0 ? 1 : k < A_BOX ? 2 : k == A_BOX ? 3 : k == A_INFO ? 6 : k == A_DLG ? 4 : 5; *first = A[g][0]; *last = A[g][1]; return; }
    if (k >= R_ROOT) { int r = k == R_ROOT ? 14 : k < R_LABEL0 ? 15 : k < R_BOX ? 16 : k == R_BOX ? 17 : k == R_DLG ? 18 : 19; *first = R[r][0]; *last = R[r][1]; return; }
    int g = k == O_ROOT ? 0 : k < O_ROW0 ? 1 : k < O_BTN0 ? 2 : k < O_UP ? 3 : k < O_BOX ? 4 : k == O_BOX ? 5 : k == K_ROOT ? 6 : k < K_SPEC0 ? 7 : k < C_ROOT ? 8 : k == C_ROOT ? 9 : k == C_ROOTP ? 13 : k < C_BTN ? 10 : k == C_BTN ? 11 : 12;
    *first = R[g][0]; *last = R[g][1];
}
static int mpm_last(int a) { return a == 67 ? 1 : a == 69 ? 3 : a == 71 ? 4 : a == 97 ? 16 : a == 111 ? 15 : 3; }

static uint32_t mpm_mem = 0, mpm_page = 0, mpm_vt_orig = 0, mpm_w = 0, mpm_loader = 0, mpm_hdr = 0, mpm_nav = 0;
static uint32_t mpm_hnd[H_N], mpm_hld[H_N], mpm_root_inst = 0;
static int mpm_mode = MP_OFF, mpm_act = MA_NONE, mpm_phase = PH_LOAD, mpm_sel = 0, mpm_next = MA_NONE;
static bool mpm_from_lan = false;
static int mpm_scr = 0, mpm_lcur = 0, mpm_lmsg = 0, mpm_lteam[4], mpm_ldev[4];
static uint32_t mpm_lprev[4];
static int mpm_lwait[4];
static uint32_t mpm_box_ptr = 0;
static int mpm_battle = 0, mpm_back_local = 0, mpm_back_online = 0;
static uint32_t mpm_pg_vt[8], mpm_pg0 = 0;
static bool mpm_surv_end = false;
static int mpm_after_surv = 0;
void rs_run_end(const char* why);
static int mpm_net_reset = 0, mpm_lan_visits = 0;
static uint32_t mpm_local_scene();
static void mpm_scene_prep(const Ctx& c, uint32_t mgr, uint32_t s);
const char* net_online_account();
static bool mpm_online_locked() { return !*net_online_account(); }

static uint32_t mpm_pvt() { return mpm_mem; }
static uint32_t mpm_wvt() { return mpm_mem + 0x80; }
static uint32_t mpm_str(int k) { return mpm_mem + (k < 128 ? 0x800 + 0x10 * k : 0xC000 + 0x10 * (k - 128)); }
static uint32_t mpm_tab() { return mpm_mem + 0x600; }
static uint32_t mpm_navo() { return mpm_mem + 0x240; }
static uint32_t mpm_navr(bool host) { return mpm_mem + (host ? 0x280 : 0x2A0); }
static void mo_names();
static void mpm_destroy(const Ctx& c);
static void ac_names();
static void mo_rows(const Ctx& c);
static void mo_attach(const Ctx& c);
static bool mo_host();
static uint32_t mpm_navp() { return mpm_mem + 0x1E0; }

static uint32_t mpm_call(const Ctx& c, uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t t0 = 0, uint32_t t1 = 0) {
    return cl_call(c, fn, a0, a1, a2, a3, t0, t1);
}

static uint32_t mpm_root() { return psp_l32(0x08A7F768u); }
static uint32_t mpm_lang(const Ctx& c) { return mpm_call(c, 0x0884F328u); }

static void mpm_sfx(const Ctx& c, uint32_t bank, uint32_t id) { mpm_call(c, 0x0881B170u, bank, id); }

static void mpm_callf(const Ctx& c, uint32_t fn, uint32_t a0, uint32_t a1, float f) {
    Ctx c2 = c; c2.r[4] = a0; c2.r[5] = a1; c2.f[12].f = f; c2.r[31] = 0;
    psp_call(c2, fn);
}

static uint32_t mpm_net_state(const Ctx& c) {
    uint32_t net = mpm_call(c, 0x08811A58u);
    return net ? psp_l32(mpm_call(c, 0x088113B8u, net)) : 0;
}

static bool mpm_net_idle(uint32_t st) { return st == 0x12 || st == 3 || st == 0; }

static bool mpm_reset_on() {
    const char* e = getenv("TTT_MPM_NETRESET");
    return !(e && *e == '0');
}

// after LAN: the network object is rebuilt like the overlay task does between scenes (0x08A81598 / 0x08A814F8)
static void mpm_reset_tick(const Ctx& c) {
    static int waits = 0;
    uint32_t st = mpm_net_state(c);
    if (!mpm_net_idle(st)) { if (waits++ % 60 == 0) printf("menu: red, esperando cierre para reiniciarla (estado %u)\n", st); return; }
    mpm_call(c, 0x08811AE4u);
    mpm_call(c, 0x08811A20u);
    mpm_call(c, 0x088119CCu);
    mpm_call(c, 0x08811A9Cu);
    mpm_net_reset = 0; waits = 0;
    printf("menu: red reiniciada tras LAN (estado al cerrar %u, objeto nuevo 0x%08X, estado nuevo %u)\n", st, mpm_call(c, 0x08811A58u), mpm_net_state(c));
}

static float mpm_f(uint32_t a) { uint32_t v = psp_l32(a); float f; memcpy(&f, &v, 4); return f; }
static void mpm_wf(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); psp_s32(a, v); }

static uint32_t mpm_alloc(const Ctx& c, uint32_t n) { return mpm_call(c, 0x089BFDF4u, n); }

static void mpm_setup_mem() {
    if (mpm_mem) return;
    mpm_mem = psp_reserve(0x10000);
    static const char* names[S_N] = {"head", "guide", "setsumei", "menu_01", "menu_02", "menu_03", "text_setsumei",
                                     "player_01", "player_02", "player_03", "player_04", "label", "team", "text_device"};
    for (int k = 0; k < S_N; k++) memcpy(psp_p(mpm_str(k)), names[k], strlen(names[k]) + 1);
    memcpy(psp_p(mpm_wvt()), psp_p(0x08A70E08u), 0xB0);
    psp_s32(mpm_wvt() + 0x14, MPM_NAT + 4 * MN_W_LOAD);
    psp_s32(mpm_wvt() + 0x64, MPM_NAT + 4 * MN_W_ANIM);
    psp_s32(mpm_wvt() + 0x6C, MPM_NAT + 4 * MN_W_DRAW);
    uint32_t p = mpm_navp();
    psp_s32(p, 6); psp_s32(p + 4, 1); mpm_wf(p + 8, 1.0f);
    psp_s32(p + 12, 5); psp_s32(p + 16, 0); mpm_wf(p + 20, 1.0f);
    p = mpm_navo();
    psp_s32(p, 6); psp_s32(p + 4, 1); mpm_wf(p + 8, 1.0f);
    psp_s32(p + 12, 5); psp_s32(p + 16, 0); mpm_wf(p + 20, 1.0f);
    psp_s32(p + 24, 13); psp_s32(p + 28, 11); mpm_wf(p + 32, 1.0f);
    psp_s32(p + 36, 7); psp_s32(p + 40, 12); mpm_wf(p + 44, 1.0f);
    p = mpm_navr(true);
    psp_s32(p, 6); psp_s32(p + 4, 1); mpm_wf(p + 8, 1.0f);
    psp_s32(p + 12, 5); psp_s32(p + 16, 10); mpm_wf(p + 20, 1.0f);
    p = mpm_navr(false);
    psp_s32(p, 6); psp_s32(p + 4, 1); mpm_wf(p + 8, 1.0f);
    mo_names();
    ac_names();
}

// holder state as the game sets it (training 0x08A87648): step, frame 0, instance of state st
static void mpm_state(const Ctx& c, uint32_t h, int st) {
    if (!h) return;
    if (h == mpm_hld[C_ROOTP] && st < 3) st += 16;
    uint32_t step = mpm_call(c, 0x08837A0Cu);
    mpm_wf(h + 0x10, (float)(int32_t)step);
    uint32_t cur = psp_l32(h);
    if (cur) mpm_wf(cur + 0x2C, 0.0f);
    uint32_t list = psp_l32(h + 4), n = psp_l32(h + 0xC), inst = 0;
    for (uint32_t i = 0; i < n; i++) if ((int)psp_l32(list + 8 * i) == st) { inst = psp_l32(list + 8 * i + 4); break; }
    psp_s32(h, inst);
    if (inst) mpm_wf(inst + 0x2C, 0.0f);
}

static int mpm_cur_state(uint32_t h) {
    if (!h || !psp_l32(h)) return -1;
    uint32_t list = psp_l32(h + 4), n = psp_l32(h + 0xC), cur = psp_l32(h);
    for (uint32_t i = 0; i < n; i++) if (psp_l32(list + 8 * i + 4) == cur) return (int)psp_l32(list + 8 * i);
    return -1;
}

static void mpm_set(const Ctx& c, int k, int st) { if (mpm_cur_state(mpm_hld[k]) != st) mpm_state(c, mpm_hld[k], st); }

static bool mpm_loops(const Ctx& c, uint32_t inst) {
    uint32_t o = psp_l32(psp_l32(inst + 0x30)), e = psp_l32(o) + 0x70;
    return mpm_call(c, psp_l32(e + 4), o + (uint32_t)(int32_t)(int16_t)psp_l16(e)) != 0;
}

static bool mpm_ended(const Ctx& c, uint32_t h) {
    uint32_t inst = h ? psp_l32(h) : 0;
    if (!inst || mpm_loops(c, inst)) return false;
    int n = (int)psp_l32(psp_l32(psp_l32(psp_l32(inst + 0x30)) + 0x198) + 0x3C);
    return (int)((float)n - 1.0f) == (int)mpm_f(inst + 0x2C);
}

static void mpm_advance(const Ctx& c, uint32_t h) {
    uint32_t inst = h ? psp_l32(h) : 0;
    if (!inst) return;
    mpm_call(c, 0x089D19D0u, inst);
    if (mpm_ended(c, h)) return;
    mpm_wf(inst + 0x2C, mpm_f(inst + 0x2C) + mpm_f(h + 0x10));
}

static uint32_t mpm_slot(const Ctx& c, uint32_t h, int name) {
    uint32_t inst = h ? psp_l32(h) : 0;
    if (!inst || !psp_l32(inst + 0x30)) return 0;
    return mpm_call(c, 0x08840DC4u, psp_l32(inst + 0x30), mpm_str(name));
}

// holder -> instance copy and draw (training 0x08A88584)
static int mpm_pk_drawn = 0;
static void mpm_draw_holder(const Ctx& c, uint32_t h) {
    uint32_t i = h ? psp_l32(h) : 0;
    if (!i || !psp_l8(h + 0x15)) return;
    mpm_pk_drawn++;
    psp_s32(i + 0x34, psp_l32(h + 8));
    psp_s32(i + 8, psp_l32(h + 0x18)); psp_s32(i + 0xC, psp_l32(h + 0x1C));
    psp_s32(i, psp_l32(h + 0x2C)); psp_s32(i + 4, psp_l32(h + 0x30));
    psp_s8(i + 0x4C, 1);
    for (int k = 0; k < 3; k++) psp_s32(i + 0x40 + 4 * k, psp_l32(h + 0x34 + 4 * k));
    psp_s32(i + 0x20, psp_l32(h + 0x20)); psp_s32(i + 0x24, psp_l32(h + 0x24));
    psp_s32(i + 0x28, psp_l32(h + 0x28));
    for (int k = 0; k < 4; k++) psp_s32(i + 0x10 + 4 * k, psp_l32(h + 0x40 + 4 * k));
    psp_s32(i + 0x50, psp_l32(h + 0x50));
    mpm_call(c, 0x089D1B94u, i);
}

static void mpm_hslot(const Ctx& c, int k, uint32_t parent, int name) { if (mpm_hld[k]) psp_s32(mpm_hld[k] + 8, mpm_slot(c, parent, name)); }

static bool mo_create_public();
static int mo_navx();
static int mpm_rootk() { static const int R[7] = {H_ROOT, L_ROOT, O_ROOT, C_ROOT, R_ROOT, A_ROOT, A_ROOT}; return mpm_scr == 3 && mo_create_public() ? C_ROOTP : R[mpm_scr]; }

// navbar entries: first time immediate (0x08A10CFC), same entries only re-attach, else the game's crossfade (0x08A10DE8, state 3)
static int mpm_nav_n = -1, mpm_hdr_t = -1;
static uint8_t mpm_nav_tb[96];
static void mpm_nav_set(const Ctx& c, uint32_t slot, int n, uint32_t tb) {
    if (!mpm_nav || !slot) return;
    uint8_t cur[96] = {0};
    for (int i = 0; i < n * 12 && i < 96; i++) cur[i] = psp_l8(tb + i);
    uint32_t h = psp_l32(mpm_nav + 0x80);
    if (mpm_nav_n < 0) mpm_call(c, 0x08A10CFCu, mpm_nav, slot, (uint32_t)n, tb);
    else if (n == mpm_nav_n && !memcmp(cur, mpm_nav_tb, sizeof cur) && h) psp_s32(h + 8, slot);
    else mpm_call(c, 0x08A10DE8u, mpm_nav, slot, (uint32_t)n, tb);
    mpm_nav_n = n;
    memcpy(mpm_nav_tb, cur, sizeof cur);
}

static void mpm_attach(const Ctx& c) {
    uint32_t r = mpm_hld[mpm_rootk()], ri = r ? psp_l32(r) : 0;
    mo_attach(c);
    if (mpm_scr >= 2) {
    } else if (!mpm_scr) {
        for (int k = 0; k < 3; k++) mpm_hslot(c, H_ROW0 + k, r, S_MENU1 + k);
        mpm_hslot(c, H_BOX, r, S_SETSUMEI);
    } else {
        for (int k = 0; k < 4; k++) {
            mpm_hslot(c, L_ROW0 + k, r, S_PLAYER1 + k);
            mpm_hslot(c, L_LABEL0 + k, mpm_hld[L_ROW0 + k], S_LABEL);
            mpm_hslot(c, L_TEAM0 + k, mpm_hld[L_ROW0 + k], S_TEAM);
        }
        mpm_hslot(c, L_BOX, r, S_SETSUMEI);
    }
    static int navx = -1;
    int nx = mpm_scr == 2 ? mo_navx() : 0;
    if (ri == mpm_root_inst) {
        uint32_t g = mpm_slot(c, r, 1);
        if (nx != navx && g) mpm_nav_set(c, g, 3 + nx, mpm_navo());
        navx = nx;
        return;
    }
    mpm_root_inst = ri;
    navx = nx;
    uint32_t sh = mpm_slot(c, r, 0), sg = mpm_slot(c, r, 1);
    if (mpm_hdr && sh) {
        int t = mpm_scr < 2 ? mpm_scr : mpm_scr >= 5 ? mpm_scr - 2 : 2;
        uint32_t hh = psp_l32(mpm_hdr + 0xE4);
        if (t == mpm_hdr_t && hh) psp_s32(hh + 8, sh);   // same title: re-attach only, a restart replays the widget's intro
        else mpm_call(c, 0x08A2E0BCu, mpm_hdr, (uint32_t)t, sh);
        mpm_hdr_t = t;
    }
    if (sg) {
        if (mpm_scr == 4) mpm_nav_set(c, sg, mo_host() ? 2 : 1, mpm_navr(mo_host()));
        else mpm_nav_set(c, sg, mpm_scr == 2 || mpm_scr == 5 ? 3 + nx : 2, mpm_scr == 2 || mpm_scr == 5 ? mpm_navo() : mpm_navp());
    }
    static int logs = 0;
    if (logs++ < 6)
        printf("menu: raiz estado %d, slots head 0x%08X guide 0x%08X menu 0x%08X 0x%08X 0x%08X setsumei 0x%08X texto 0x%08X\n",
               mpm_cur_state(r), sh, sg, mpm_hld[H_ROW0] ? psp_l32(mpm_hld[H_ROW0] + 8) : 0, mpm_hld[H_ROW1] ? psp_l32(mpm_hld[H_ROW1] + 8) : 0,
               mpm_hld[H_ROW2] ? psp_l32(mpm_hld[H_ROW2] + 8) : 0, mpm_hld[H_BOX] ? psp_l32(mpm_hld[H_BOX] + 8) : 0,
               mpm_slot(c, mpm_hld[H_BOX], 6));
}

static void mpm_rows(const Ctx& c) {
    mo_rows(c);
    if (mpm_scr >= 2) return;
    if (!mpm_scr) {
        bool lock = mpm_online_locked();
        static int logged = -1;
        if (logged != (int)lock) { logged = lock; printf("menu: submenu, En linea %s\n", lock ? "bloqueado (sin cuenta de Steam, estado 12)" : "disponible"); }
        if (lock && mpm_sel == 2) mpm_sel = 0;
        for (int k = 0; k < 3; k++) mpm_set(c, H_ROW0 + k, k == 2 && lock ? 12 : 6 + 2 * k + (mpm_sel == k));
        mpm_set(c, H_BOX, 5);
        return;
    }
    for (int r = 0; r < 4; r++) {
        int on = mpm_lcur == r;
        mpm_set(c, L_ROW0 + r, 4 + 2 * mpm_lteam[r] + on);
        mpm_set(c, L_TEAM0 + r, 10 + 2 * mpm_lteam[r] + on);
        mpm_set(c, L_LABEL0 + r, r);
    }
    mpm_set(c, L_BOX, 3);
}

// description texts drawn with the game font (like Entrenamiento's box), UTF-16 in port memory
static uint32_t mpm_utf16(const char* src, int buf) {
    uint32_t a = mpm_mem + 0x1000 + 0x100 * buf;
    const unsigned char* p = (const unsigned char*)src;
    int n = 0;
    while (*p && n < 120) {
        uint32_t u = *p++;
        if (u >= 0xC0 && *p) u = ((u & 0x1F) << 6) | (*p++ & 0x3F);
        psp_s16(a + 2 * n++, u);
    }
    psp_s16(a + 2 * n, 0);
    return a;
}

static int mpm_li(uint32_t lang) { return lang == 2 ? 1 : 0; }   // 0 en, 1 es (French is not offered)


static uint32_t mpm_text(int sel, uint32_t lang) {
    static const char* T[2][3] = {
        {"Play on this same computer in split screen, up to 4 players.",
         "Play with other computers connected to your local network.",
         "Play over the internet in public or private rooms."},
        {"Juega en este mismo equipo con pantalla dividida, hasta 4 jugadores.",
         "Juega con otros equipos conectados a tu misma red local.",
         "Juega por internet en salas p\u00FAblicas o privadas."}
    };
    return mpm_utf16(T[mpm_li(lang)][sel], sel);
}


static uint32_t mpm_local_text(int r, uint32_t lang) {
    static const char* T[2][5] = {{"Keyboard", "Xbox controller %d", "PlayStation controller", "Controller", "CPU"},
                                  {"Teclado", "Mando Xbox %d", "Mando PlayStation", "Mando", "CPU"}
    };
    const char* const* t = T[mpm_li(lang)];
    char b[64] = "";
    int d = mpm_ldev[r], fam = 0;
    if (mpm_lteam[r] == 2 || (r == 0 && d < 0)) return 0;
    if (d < 0) snprintf(b, sizeof(b), "%s", t[4]);
    else if (d == 0) snprintf(b, sizeof(b), "%s", t[0]);
    else if (d <= 4) snprintf(b, sizeof(b), t[1], d);
    else { psp_local_dev(r, &fam); snprintf(b, sizeof(b), "%s", fam == 2 ? t[2] : t[3]); }
    return mpm_utf16(b, 4 + r);
}

static int mpm_lmsg_id = 1;

static const char* mpm_local_hint(uint32_t lang) {
    static const char* T[2][3] = {
        {"Press a button on a controller or the keyboard to join.", "Each team needs at least one fighter.", "At least 2 players are needed."},
        {"Pulsa un bot\u00F3n de un mando o del teclado para unirte.", "Cada equipo necesita al menos un luchador.", "Se necesitan al menos 2 jugadores."}
    };
    return T[mpm_li(lang)][mpm_lmsg > 0 ? mpm_lmsg_id : 0];
}

// box text in lines of up to 28 characters split at spaces, 18 px apart (text_setsumei is 203 x 45)
static void mpm_font_lines(const Ctx& c, uint32_t font, uint32_t slot, const char* src, int maxc = 28, int buf0 = 8) {
    if (!slot || !src) return;
    int x = (int)psp_l32(slot + 0xE4), y = (int)psp_l32(slot + 0xE8), n = (int)strlen(src), at = 0;
    for (int line = 0; line < 3 && at < n; line++) {
        int end = at, chars = 0, cut = -1;
        while (end < n && chars < maxc) {
            if (src[end] == ' ') cut = end;
            end += ((unsigned char)src[end] >= 0xC0) ? 2 : 1;
            chars++;
        }
        if (end < n && cut > at) end = cut;
        char b[96];
        int len = end - at < 95 ? end - at : 95;
        memcpy(b, src + at, len); b[len] = 0;
        mpm_call(c, 0x08832530u, font, (uint32_t)x, (uint32_t)(y + 18 * line), mpm_utf16(b, buf0 + line), 0xFFFFFFFFu, 0xFFFFFFFFu);
        at = end;
        while (at < n && src[at] == ' ') at++;
    }
}

static void mpm_font_at(const Ctx& c, uint32_t font, uint32_t slot, uint32_t str) {
    if (!slot || !str) return;
    mpm_call(c, 0x08832530u, font, psp_l32(slot + 0xE4), psp_l32(slot + 0xE8), str, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

#include "core/hooks/avatars.inc"
#include "core/hooks/mp_online.inc"
#include "core/hooks/mp_account.inc"

static void mpm_draw_text(const Ctx& c) {
    if (mpm_phase != PH_NORMAL) return;
    uint32_t font = psp_l32(0x08A78EECu), lang = mpm_lang(c);
    if (!font) return;
    mpm_call(c, 0x08832448u, font);
    if (mpm_scr >= 2) mo_draw_text(c, font, lang);
    else if (!mpm_scr) mpm_font_at(c, font, mpm_slot(c, mpm_hld[H_BOX], S_TEXT), mpm_text(mpm_sel, lang));
    else {
        for (int r = 0; r < 4; r++) mpm_font_at(c, font, mpm_slot(c, mpm_hld[L_ROW0 + r], S_DEVICE), mpm_local_text(r, lang));
        mpm_font_lines(c, font, mpm_slot(c, mpm_hld[L_BOX], S_TEXT), mpm_local_hint(lang));
    }
    mpm_call(c, 0x08832500u, font);
    static int logs[2] = {0, 0};
    if (mpm_scr < 2 && logs[mpm_scr]++ < 1) {
        uint32_t s0 = mpm_slot(c, mpm_hld[mpm_scr ? L_ROW0 : H_BOX], mpm_scr ? S_DEVICE : S_TEXT);
        printf("menu: textos con la fuente del juego, pantalla %d, primero en (%d, %d)\n", mpm_scr, s0 ? (int)psp_l32(s0 + 0xE4) : -1, s0 ? (int)psp_l32(s0 + 0xE8) : -1);
    }
}

// pak loaded (callback through the loader, 0x089D1858): pieces, header and button bar
static void mpm_loaded(const Ctx& c, uint32_t w) {
    uint32_t pak = psp_l32(mpm_loader + 0x14), lang = mpm_lang(c);
    if (!psp_l32(0x08B48A08u)) { psp_s32(0x08B48A08u, 1); mpm_call(c, 0x088402C0u, 0x08B48200u); }
    for (int k = 0; k < H_N; k++) {
        if (k >= B_OPT && (mpm_scr < 5 || ac_boot == 1)) { mpm_hnd[k] = mpm_hld[k] = 0; continue; }
        int a = mpm_ags(k), first = 0, last = mpm_last(a);
        if (k >= O_ROOT) mpm_range(k, &first, &last);
        mpm_hnd[k] = mpm_owner(k) ? mpm_call(c, 0x088404D8u, 0x08B48200u, pak, a, a == 97 || a == 111 || a == 45 || a >= 125 ? a + 1 + lang : a + 1) : mpm_hnd[mpm_src(k)];
        uint32_t h = mpm_alloc(c, 0x54);
        if (h) mpm_call(c, 0x089D1EE8u, h, mpm_hnd[k], (uint32_t)first, (uint32_t)last);
        mpm_hld[k] = h;
    }
    av_pak57_loaded(pak, 126 + lang);
    uint32_t t = mpm_tab();
    static const int titles[5] = {104, 118, 132, 167, 174};
    for (int i = 0; i < 5; i++) { psp_s32(t + 16 * i, titles[i]); psp_s32(t + 16 * i + 4, titles[i] + 1 + lang); psp_s32(t + 16 * i + 8, 0); psp_s32(t + 16 * i + 12, 0); }
    mpm_hdr = ac_boot == 1 ? 0 : mpm_alloc(c, 0x1FC);
    if (mpm_hdr) { mpm_call(c, 0x08A2D7B4u, mpm_hdr, pak, 74, 75, t, 5); mpm_call(c, 0x089D0310u, w, mpm_hdr, 0x80, 0); }
    mpm_nav_n = mpm_hdr_t = -1;
    mpm_nav = mpm_alloc(c, 0x25C);
    if (mpm_nav) { mpm_call(c, 0x08A104B4u, mpm_nav, pak, ac_boot == 1 ? 195 : 73); mpm_call(c, 0x089D0310u, w, mpm_nav, 0x80, 0); }
    ac_bg_start(c);
    mpm_state(c, mpm_hld[H_BG], 0);
    mpm_state(c, mpm_hld[H_LAYERS], 0);
    mpm_state(c, mpm_hld[H_FIRE], 0);
    mpm_state(c, mpm_hld[mpm_rootk()], 0);
    mpm_rows(c);
    mpm_phase = PH_OPEN;
    printf("menu: submenu multijugador cargado, pak 0x%08X idioma %u, AGS 97 handle 0x%08X, AGS 111 handle 0x%08X, AGS 45 handle 0x%08X, fondo 0x%08X, cabecera 0x%08X, botones 0x%08X\n",
           pak, lang, mpm_hnd[H_ROOT], mpm_hnd[L_ROOT], mpm_hnd[L_LABEL0], mpm_hnd[H_BG], mpm_hdr, mpm_nav);
    printf("menu: en linea, AGS 125 handle 0x%08X, 139 handle 0x%08X, 146 handle 0x%08X, 153 handle 0x%08X (sala, estados %s), %d piezas\n", mpm_hnd[O_ROOT], mpm_hnd[K_ROOT], mpm_hnd[C_ROOT],
           mpm_hnd[R_ROOT], mpm_has_state(mpm_hld[R_DBTN0], 9) ? "completos" : "incompletos: pak sin aplicar", H_N - O_ROOT);
    printf("menu: cuenta, AGS 160 handle 0x%08X (estados %s)\n", mpm_hnd[A_ROOT], mpm_has_state(mpm_hld[A_DBTN0], 12) && mpm_has_state(mpm_hld[A_ROW0], 9) ? "completos" : "incompletos: pak sin aplicar");
}

static void mpm_input(const Ctx& c) {
    uint32_t e = psp_l32(0x08B4E990u + 0x14);
    int n = mpm_online_locked() ? 2 : 3;
    if (mpm_sel >= n) mpm_sel = 0;
    if (e & 0x10) { mpm_sel = (mpm_sel + n - 1) % n; mpm_sfx(c, 1, 0); }
    else if (e & 0x40) { mpm_sel = (mpm_sel + 1) % n; mpm_sfx(c, 1, 0); }
    else if (e & 0x4000) {
        mpm_sfx(c, 1, 6);
        mpm_next = mpm_sel == 0 ? MA_TOLOCAL : mpm_sel == 1 ? MA_LAN : MA_TOONLINE;
        if (mpm_next == MA_TOLOCAL) {
            psp_local_join(1); psp_local_manual(1);
            mpm_lteam[0] = 0; mpm_lteam[1] = 1; mpm_lteam[2] = mpm_lteam[3] = 2;
            for (int r = 0; r < 4; r++) { mpm_ldev[r] = -1; mpm_lprev[r] = 0; }
            mpm_lcur = 0; mpm_lmsg = 0;
        }
    } else if (e & 0x2000) { mpm_sfx(c, 2, 0); mpm_next = MA_BACK; }
    if (mpm_next != MA_NONE) { mpm_state(c, mpm_hld[H_ROOT], 2); mpm_phase = PH_CLOSE; }
}

// local select (AGS 111): rows 0..3 = 1P..4P, team 0 A, 1 B, 2 empty; row 0 is J1 (the device that chose Local)
static int mpm_count(int t, int except) {
    int n = 0;
    for (int r = 0; r < 4; r++) if (r != except && mpm_lteam[r] == t) n++;
    return n;
}

static bool mpm_cycle(const Ctx& c, int r, int dir) {
    bool empty_ok = r != 0 && mpm_ldev[r] < 0;
    int t = mpm_lteam[r];
    for (int i = 0; i < 2; i++) {
        t = (t + dir + 3) % 3;
        if (t == 2 ? !empty_ok : mpm_count(t, r) >= 2) continue;
        mpm_lteam[r] = t; mpm_sfx(c, 1, 0);
        return true;
    }
    mpm_sfx(c, 1, 3);
    return false;
}

static void mpm_local_claim(const Ctx& c) {
    if (mpm_ldev[0] >= 0) return;
    int d = psp_local_free_pressed();
    if (d < 0) return;
    psp_local_assign(d, 0); mpm_ldev[0] = d;
    printf("menu: local, J1 reclamado por el dispositivo %d\n", d);
}

static void mpm_local_join(const Ctx& c) {
    int d = psp_local_free_pressed();
    if (d < 0) return;
    for (int r = 1; r < 4; r++) {
        if (mpm_ldev[r] >= 0) continue;
        if (mpm_lteam[r] == 2) {
            int a = mpm_count(0, r), b = mpm_count(1, r);
            if (a >= 2 && b >= 2) continue;
            mpm_lteam[r] = b < a || a >= 2 ? 1 : 0;
        }
        psp_local_assign(d, r); mpm_ldev[r] = d;
        mpm_lwait[r] = 20;
        mpm_sfx(c, 1, 6);
        printf("menu: local, el dispositivo %d se une en la fila %d (equipo %d)\n", d, r + 1, mpm_lteam[r]);
        return;
    }
}

static void mpm_local_restore() {
    psp_local_join(1); psp_local_manual(1);
    for (int k = 0; k < 4; k++) psp_local_release(k);
    for (int r = 0; r < 4; r++) { mpm_lwait[r] = 20; mpm_lprev[r] = 0; if (mpm_ldev[r] >= 0) psp_local_assign(mpm_ldev[r], r); }
    mpm_lcur = 0; mpm_lmsg = 0;
    printf("menu: local, vuelve a la seleccion local con J1 dispositivo %d, J2 %d, J3 %d, J4 %d\n", mpm_ldev[0], mpm_ldev[1], mpm_ldev[2], mpm_ldev[3]);
}

static int mpm_lslot[4] = {-1, -1, -1, -1};

static void mpm_local_commit() {
    int g0 = mpm_lteam[0], cnt[2] = {0, 0}, slot[4], map[4] = {-1, -1, -1, -1}, n = 0;
    for (int r = 0; r < 4; r++) {
        slot[r] = -1;
        if (mpm_lteam[r] == 2) continue;
        int g = mpm_lteam[r] == g0 ? 0 : 1;
        slot[r] = 2 * g + cnt[g]++;
    }
    for (int r = 0; r < 4; r++) if ((r == 0 || mpm_ldev[r] >= 0) && slot[r] >= 0) { map[r] = n; lmp_cfg_slot[n++] = slot[r]; }
    psp_local_remap(map);
    for (int r = 0; r < 4; r++) mpm_lslot[r] = slot[r];
    lmp_cfg_cnt[0] = cnt[0]; lmp_cfg_cnt[1] = cnt[1]; lmp_cfg_n = n; lmp_cfg_on = 1;
    printf("menu: local confirmado, %d contra %d (equipo de J1 primero), %d humanos\n", cnt[0], cnt[1], n);
    for (int r = 0; r < 4; r++)
        printf("menu: local, fila %d equipo %c %s, luchador %d, jugador %d\n", r + 1, "ABV"[mpm_lteam[r]], mpm_lteam[r] == 2 ? "vacia" : r == 0 || mpm_ldev[r] >= 0 ? "humano" : "CPU", slot[r], map[r] + 1);
}

static void mpm_local_input(const Ctx& c) {
    if (mpm_lmsg > 0) mpm_lmsg--;
    mpm_local_claim(c);
    mpm_local_join(c);
    uint32_t e = psp_l32(0x08B4E990u + 0x14);
    if (e & 0x10) { mpm_lcur = (mpm_lcur + 3) % 4; mpm_sfx(c, 1, 0); }
    else if (e & 0x40) { mpm_lcur = (mpm_lcur + 1) % 4; mpm_sfx(c, 1, 0); }
    else if (e & 0xA0) {
        if (mpm_lcur != 0 && mpm_ldev[mpm_lcur] >= 0) mpm_sfx(c, 1, 3);
        else mpm_cycle(c, mpm_lcur, e & 0x20 ? 1 : -1);
    } else if (e & 0x4000) {
        int humans = 1;
        for (int r = 1; r < 4; r++) humans += mpm_ldev[r] >= 0 && mpm_lteam[r] != 2;
        if (mpm_count(0, -1) < 1 || mpm_count(1, -1) < 1) { mpm_sfx(c, 1, 3); mpm_lmsg = 180; mpm_lmsg_id = 1; }
        else if (humans < 2) { mpm_sfx(c, 1, 3); mpm_lmsg = 180; mpm_lmsg_id = 2; }
        else { mpm_sfx(c, 1, 6); mpm_local_commit(); mpm_next = MA_LOCAL; }
    } else if (e & 0x2000) { mpm_sfx(c, 2, 0); psp_local_join(0); mpm_next = MA_TOSUB; }
    for (int r = 1; r < 4 && mpm_next == MA_NONE; r++) {
        if (mpm_ldev[r] < 0) continue;
        uint32_t w = 0; uint8_t x, y;
        psp_input_local(r, &w, &x, &y);
        uint32_t ed = mpm_lwait[r] > 0 ? 0 : w & ~mpm_lprev[r];
        if (mpm_lwait[r] > 0) mpm_lwait[r] = w ? 8 : mpm_lwait[r] - 1;
        mpm_lprev[r] = w;
        if (ed & 0xA0) mpm_cycle(c, r, ed & 0x20 ? 1 : -1);
        else if (ed & 0x2000) {
            psp_local_release(r); mpm_ldev[r] = -1; mpm_sfx(c, 2, 0);
            printf("menu: local, la fila %d queda libre (CPU)\n", r + 1);
        }
    }
    if (mpm_next != MA_NONE) { mpm_state(c, mpm_hld[L_ROOT], 2); mpm_phase = PH_CLOSE; }
}

static bool mpm_shown(int k) {
    static const int G[7] = {1, 2, 3, 5, 6, 8, 8};
    int g = mpm_group(k);
    if (g == 10 || (g == 0 && (ac_boot == 1 || mpm_hld[B_OPT]))) return false;
    return g == 0 || g == G[mpm_scr] || (g == 4 && mo_kph != KP_OFF) || (g == 7 && mpm_scr == 4 && mo_dlg != 0) || (g == 9 && mpm_scr >= 5 && ac_dlg != 0);
}

// the multiplayer screens of the port have no music of their own: like the WLAN lobby they play 0x112B (ost_sm_1) when none plays
static void mpm_bgm(const Ctx& c) {
    static int wait = 0;
    uint32_t snd = psp_l32(0x08A78EE8u);
    if (mpm_scr > 4 || mpm_phase != PH_NORMAL || mpm_battle || mpm_act != MA_NONE || mpm_next != MA_NONE || !snd) { wait = 0; return; }
    if (mpm_call(c, 0x0883A7F4u, snd, 0)) { wait = 0; return; }
    if (++wait < 30) return;
    wait = 0;
    Ctx c2 = c; c2.r[4] = snd; c2.r[5] = 0; c2.r[6] = 0; c2.r[7] = 0x112B; c2.r[8] = 0; c2.r[9] = 0; c2.f[12].f = 0.0f; c2.r[31] = 0;
    psp_call(c2, 0x0883A430u);
    printf("menu: multijugador sin musica: suena la del lobby (0x112B)\n");
}

static void mpm_w_anim(Ctx& c) {
    if (mpm_phase == PH_LOAD || !mpm_hld[H_ROOT]) return;
    mpm_bgm(c);
    uint32_t root = mpm_hld[mpm_rootk()];
    if (mpm_next == MA_TOLOCAL || mpm_scr == 1) mpm_local_claim(c);
    if (mpm_scr >= 2) mo_kanim(c);
    if (mpm_scr == 4) mo_room_tick(c);
    mo_discord_frame(c);
    av_frame();
    ac_bg_anim(c);
    if (mpm_phase == PH_OPEN && mpm_ended(c, root)) {
        mpm_state(c, root, 1); mpm_set(c, H_BG, 1); mpm_phase = PH_NORMAL;
    } else if (mpm_phase == PH_NORMAL) { if (mpm_scr >= 2) mo_input(c); else if (mpm_scr) mpm_local_input(c); else mpm_input(c); }
    else if (mpm_phase == PH_CLOSE && mpm_ended(c, root) && mpm_act == MA_NONE) {
        if (mpm_next == MA_TOLOCAL || mpm_next == MA_TOSUB || mpm_next == MA_TOONLINE || mpm_next == MA_TOCREATE || mpm_next == MA_TOROOM || mpm_next == MA_TOSTART || mpm_next == MA_TOSET) {
            int from = mpm_scr;
            mpm_scr = mpm_next == MA_TOLOCAL ? 1 : mpm_next == MA_TOONLINE ? 2 : mpm_next == MA_TOCREATE ? 3 : mpm_next == MA_TOROOM ? 4 : mpm_next == MA_TOSTART ? 5 : mpm_next == MA_TOSET ? 6 : 0;
            mpm_next = MA_NONE;
            mpm_state(c, mpm_hld[mpm_rootk()], 0);
            mpm_phase = PH_OPEN;
            if (mpm_scr == 2) mo_enter(c, from == 0);
            if (mpm_scr == 4) mo_room_enter(c);
            static const char* sn[7] = {"submenu", "seleccion local", "en linea", "crear sala", "sala", "cuenta", "ajustes de cuenta"};
            printf("menu: pantalla %s\n", sn[mpm_scr]);
        } else {
            static const char* names[10] = {"", "LAN", "atras", "local", "", "", "", "", "", "batalla en linea"};
            printf("menu: elegido %s\n", names[mpm_next]);
            mpm_act = mpm_next;
        }
    }
    mpm_rows(c);
    int ls = mpm_cur_state(mpm_hld[H_LAYERS]), fs = mpm_cur_state(mpm_hld[H_FIRE]);
    if (ls >= 0 && mpm_ended(c, mpm_hld[H_LAYERS])) mpm_state(c, mpm_hld[H_LAYERS], ls == 1 ? 2 : 1);
    if (fs >= 0 && mpm_ended(c, mpm_hld[H_FIRE])) mpm_state(c, mpm_hld[H_FIRE], (fs + 1) % 4);
    for (int k = 0; k < H_N; k++) if (mpm_shown(k)) mpm_advance(c, mpm_hld[k]);
    for (int k = O_ROOT; k < H_N; k++) {
        if (k == O_ROOT || k == K_ROOT || k == C_ROOT || k == C_ROOTP || k == R_ROOT || k == A_ROOT || !mpm_shown(k)) continue;
        uint32_t h = mpm_hld[k], inst = h ? psp_l32(h) : 0;
        if (inst && mpm_ended(c, h)) mpm_wf(inst + 0x2C, 0.0f);
    }
    if (mpm_scr == 1) for (int r = 0; r < 4; r++) {
        uint32_t h = mpm_hld[L_ROW0 + r], inst = h ? psp_l32(h) : 0;
        if (inst && (mpm_cur_state(h) & 1) && mpm_ended(c, h)) mpm_wf(inst + 0x2C, 0.0f);
    }
    mpm_attach(c);
}

static int mpm_pk_wd = 0;
static void mpm_w_draw(Ctx& c) {
    mpm_pk_wd++;
    if (mpm_phase == PH_LOAD) return;
    ac_bg_draw(c);
    for (int k = 0; k < H_N; k++) {
        if (!mpm_shown(k) || mpm_group(k) == 4 || mpm_group(k) == 7 || mpm_group(k) == 9 || (k >= O_ROOT && mo_hide[k]) || (((k >= L_TEAM0 && k < L_LABEL0) || (k >= R_LABEL0 && k < R_BOX) || k == O_AV || (k >= R_AV0 && k < R_DLG)) && !(mpm_hld[k] && psp_l32(mpm_hld[k] + 8)))) continue;
        mpm_draw_holder(c, mpm_hld[k]);
    }
    mpm_draw_text(c);
    if (mpm_scr == 4 && mo_dlg && mpm_phase == PH_NORMAL) {
        for (int k = R_DLG; k <= R_DBTN1; k++) if (!mo_hide[k]) mpm_draw_holder(c, mpm_hld[k]);
        uint32_t font = psp_l32(0x08A78EECu);
        if (font) { mpm_call(c, 0x08832448u, font); mo_draw_dlg_text(c, font, mpm_lang(c)); mpm_call(c, 0x08832500u, font); }
    }
    if (mpm_scr >= 5 && ac_dlg && mpm_phase == PH_NORMAL) {
        for (int k = A_DLG; k <= A_DBTN1; k++) if (!mo_hide[k]) mpm_draw_holder(c, mpm_hld[k]);
        uint32_t font = psp_l32(0x08A78EECu);
        if (font) { mpm_call(c, 0x08832448u, font); ac_draw_dlg(c, font, mpm_lang(c)); mpm_call(c, 0x08832500u, font); }
    }
    if (mo_kph == KP_OFF) return;
    for (int k = K_ROOT; k < C_ROOT; k++) mpm_draw_holder(c, mpm_hld[k]);
    uint32_t font = psp_l32(0x08A78EECu);
    if (!font) return;
    mpm_call(c, 0x08832448u, font);
    mo_draw_kb_text(c, font, mpm_lang(c));
    mpm_call(c, 0x08832500u, font);
}

static void mpm_destroy(const Ctx& c) {
    if (!mpm_w) return;
    uint32_t root = mpm_root(), w = mpm_w;
    mpm_call(c, 0x089CFD08u, root, w);
    for (uint32_t ch : {mpm_hdr, mpm_nav}) {
        if (!ch) continue;
        mpm_call(c, 0x089CFD08u, w, ch);
        uint32_t v = psp_l32(ch + 0x40) + 8;
        mpm_call(c, psp_l32(v + 4), ch + (uint32_t)(int32_t)(int16_t)psp_l16(v), 3);
    }
    for (int k = 0; k < H_N; k++) {
        if (mpm_hld[k]) mpm_call(c, 0x089D248Cu, mpm_hld[k], 3);
        if (mpm_hnd[k] && mpm_owner(k)) mpm_call(c, 0x08840640u, 0x08B48200u, mpm_hnd[k]);
        mpm_hld[k] = mpm_hnd[k] = 0;
    }
    if (mpm_loader) mpm_call(c, 0x089D27D0u, mpm_loader, 3);
    psp_s32(w + 0x60, 0);
    mpm_call(c, 0x089D1590u, w, 3);
    mpm_w = mpm_loader = mpm_hdr = mpm_nav = mpm_root_inst = 0;
    mpm_nav_n = mpm_hdr_t = -1;
    mo_kph = KP_OFF; mo_kb = KB_NONE;
    av_drop_menu();
    printf("menu: submenu destruido\n");
}

static void mpm_pg_enter(Ctx& c) {
    uint32_t page = c.r[4];
    if (mpm_mode != MP_SUB) { mpm_call(c, psp_l32(mpm_vt_orig + 0x14), page); return; }
    psp_s32(page + 8, 0); psp_s32(page + 0x10, 0);
    mpm_after_surv = 0;
    mpm_act = MA_NONE; mpm_next = MA_NONE; mpm_phase = PH_LOAD; mpm_sel = 0; mpm_scr = 0;
    if (mpm_back_local) { mpm_back_local = 0; mpm_scr = 1; lmp_cfg_on = 0; mpm_local_restore(); }
    if (mpm_back_online) { mpm_back_online = 0; mpm_scr = 2; mo_enter(c, true); }
    else if (mo_room_resume(c)) mpm_scr = 4;
    if (ac_entry) mpm_scr = ac_entry == AE_SETTINGS ? 6 : 5;
    uint32_t w = mpm_alloc(c, 0x80);
    if (!w) { mpm_mode = MP_ORIG; mpm_call(c, psp_l32(mpm_vt_orig + 0x14), page); return; }
    mpm_call(c, 0x089D1538u, w);
    psp_s32(w + 0x40, mpm_wvt());
    mpm_w = w;
    mpm_call(c, 0x089D0310u, mpm_root(), w, 0x80, 0);
    printf("menu: submenu multijugador abierto (widget 0x%08X)\n", w);
}

static void mpm_probe();
static void mpm_pg_update(Ctx& c) {
    uint32_t page = c.r[4];
    mpm_probe();
    if (mpm_mode == MP_GONE) return;
    if (mpm_mode == MP_SUB) {
        uint32_t net = mpm_call(c, 0x08811A58u);
        if (net) mpm_call(c, 0x088113DCu, net);
        if (mpm_mode == MP_SUB && mpm_net_reset) mpm_reset_tick(c);
    }
    if (mpm_mode != MP_SUB) {
        uint32_t scr = psp_l32(page + 0x10);
        if (mpm_box_ptr && scr) {
            uint32_t box = mpm_box_ptr, par = box ? psp_l32(box + 0xC) : 0;
            mpm_box_ptr = 0;
            if (box) {
                // the list child at +0x70 owns the box in +0x1E0 and deletes it in its dtor (0x08AAC948)
                bool owned = par == scr + 0x70 && psp_l32(par + 0x1E0) == box;
                if (!owned) {
                    if (par) mpm_call(c, 0x089CFD08u, par, box);
                    uint32_t vb = psp_l32(box + 0x40) + 8;
                    mpm_call(c, psp_l32(vb + 4), box + (uint32_t)(int32_t)(int16_t)psp_l16(vb), 3);
            }
            printf("menu: caja de salida %s\n", owned ? "borrada por la lista (dueña en +0x1E0)" : "borrada aparte (sin dueño conocido)");
            }
            mpm_call(c, 0x089CFD08u, mpm_root(), scr);
            uint32_t v = psp_l32(scr + 0x40) + 8;
            mpm_call(c, psp_l32(v + 4), scr + (uint32_t)(int32_t)(int16_t)psp_l16(v), 3);
            psp_s32(page + 0x10, 0);
            psp_s32(page + 8, 0);
            mpm_from_lan = false;
            mpm_mode = MP_SUB;
            net_online_stop();
            uint32_t net = mpm_call(c, 0x08811A58u);
            if (mpm_reset_on() && net) { mpm_call(c, 0x08811568u, net); mpm_net_reset = 1; }
            printf("menu: salida de la lista de salas, vuelve al submenu sin preguntar (caja 0x%08X padre 0x%08X, red estado %u, reinicio %d)\n", box, par, mpm_net_state(c), mpm_net_reset);
            mpm_pg_enter(c);
            return;
        }
        mpm_call(c, psp_l32(mpm_vt_orig + 0x1C), page);
        return;
    }
    mpm_call(c, 0x089D06ECu, mpm_root(), 0);
    if (mpm_act == MA_NONE) return;
    if (mpm_act == MA_LAN && mpm_net_reset) return;
    int act = mpm_act;
    mpm_net_reset = 0;
    mpm_destroy(c);
    mpm_mode = MP_ORIG;
    if (act == MA_ROOMGO) {
        int pg = mo_room_handover(c);
        mpm_call(c, 0x089C34A4u, psp_l32(0x08A7EA50u), (uint32_t)pg);
        psp_s32(page + 8, 3);
        printf("menu: sala, pide la pagina %d como la pantalla original (red estado %u)\n", pg, mpm_net_state(c));
        return;
    }
    if (act == MA_LAN) { mpm_from_lan = true; printf("menu: LAN, sigue el flujo WLAN original (visita %d, red 0x%08X estado %u)\n", ++mpm_lan_visits, mpm_call(c, 0x08811A58u), mpm_net_state(c)); mpm_call(c, psp_l32(mpm_vt_orig + 0x14), page); return; }
    uint32_t net = mpm_call(c, 0x08811A58u);
    if (net) mpm_call(c, 0x08811568u, net);
    if (act == MA_LOCAL) { mpm_battle = 1; printf("menu: local, sale al menu principal para entrar a Batalla como el juego\n"); }
    Ctx c2 = c; c2.r[4] = 0; c2.r[5] = 0; c2.f[12].f = 1.0f; c2.r[31] = 0;
    psp_call(c2, 0x089BFA48u);
    psp_s32(page + 8, 2);
    printf("menu: atras, salida de la escena multijugador\n");
}

static void mpm_pg_exit(Ctx& c) {
    uint32_t page = c.r[4];
    mpm_destroy(c);
    if (psp_l32(page + 0x10)) mpm_call(c, psp_l32(mpm_vt_orig + 0x2C), page);
    else if (!ac_back_opt) mpm_call(c, 0x0883A5A4u, psp_l32(0x08A78EE8u), 0);
    if (mpm_page == page) psp_s32(page + 4, mpm_vt_orig);
    mpm_mode = MP_OFF; mpm_page = 0; mpm_net_reset = 0; mpm_lan_visits = 0;
}

static void cr_native(Ctx& c, int k);
void mpm_native(Ctx& c, uint32_t dir) {
    if ((int)((dir - MPM_NAT) / 4) >= 40) { cr_native(c, (int)((dir - MPM_NAT) / 4)); c.r[2] = 0; return; }
    switch ((dir - MPM_NAT) / 4) {
        case MN_PG_ENTER: mpm_pg_enter(c); break;
        case MN_PG_UPDATE: mpm_pg_update(c); break;
        case MN_PG_EXIT: mpm_pg_exit(c); break;
        case MN_PG_DRAW: mpm_call(c, psp_l32(mpm_vt_orig + 0x24), c.r[4]); break;
        case MN_PX_UPDATE: {
            uint32_t pg = c.r[4], i = psp_l32(pg) & 7;
            mpm_probe();
            // page 6 = the original survival records (0x08AB9020): a ranked run skips it and leaves like it does on close, scene request 1
            if (i == 6 && (mpm_surv_end || mpm_after_surv == 1)) {
                if (mpm_surv_end) { mpm_surv_end = false; mpm_after_surv = 1; rs_run_end("pagina 6"); mpm_call(c, 0x089C34ACu, psp_l32(0x08A7EA50u), 1); printf("menu: supervivencia clasificatoria terminada: sin la tabla original, pide la escena 1\n"); }
                break;
            }
            mpm_call(c, psp_l32(mpm_pg_vt[i] + 0x1C), pg); mo_page_tick(pg, (int)i); break;
        }
        case MN_PX_DRAW: { uint32_t i = psp_l32(c.r[4]) & 7; if (i == 6 && mpm_after_surv == 1) break; mpm_call(c, psp_l32(mpm_pg_vt[i] + 0x24), c.r[4]); break; }
        case MN_W_LOAD:
            mpm_loader = mpm_alloc(c, 0x28);
            if (mpm_loader) {
                mpm_call(c, 0x089D26A0u, mpm_loader);
                mpm_call(c, 0x089D2828u, mpm_loader, 56);
                mpm_call(c, 0x089D28A0u, mpm_loader, MPM_NAT + 4 * MN_W_PAK, c.r[4]);
                psp_s32(c.r[4] + 0x60, mpm_loader);
            }
            break;
        case MN_W_PAK: mpm_loaded(c, c.r[4]); break;
        case MN_W_ANIM: mpm_w_anim(c); break;
        case MN_W_DRAW: mpm_w_draw(c); break;
    }
    c.r[2] = 0;
}

// yes/no box of 56_38 (0x08A827C8 -> 0x08A3B28C): asked by the room list when Circle is pressed after LAN; the page update
// removes it with the list and opens the submenu again
void mpm_box(const Ctx& c) {
    uint32_t pad = 0x08B4E990u, circ = (psp_l32(pad + 0x14) | psp_l32(pad + 0x2C)) & 0x2000;
    static int n = 0;
    if (n++ < 10) printf("menu: caja si/no id %u desde 0x%08X, modo %d, desde LAN %d, circulo %d\n", psp_l32(c.r[5] + 4), c.r[31] - 8, mpm_mode, mpm_from_lan, circ ? 1 : 0);
    if (mpm_mode == MP_ORIG && mpm_from_lan && c.r[31] - 8 == 0x08A8283Cu && circ) mpm_box_ptr = c.r[4];
}

// free battle team screen (24_18, scene 3) skipped; TTT_LOCAL_TEAMSCREEN=1 keeps it
static bool mpm_skip_teams() {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_LOCAL_TEAMSCREEN"); on = !(e && *e == '1'); }
    return on != 0;
}

// scene after Local (TTT_LOCAL_SCENE): 3 like the main menu's Batalla, whose overlay (24_18) then requests 2
static uint32_t mpm_local_scene() {
    const char* e = getenv("TTT_LOCAL_SCENE");
    return e && *e ? (uint32_t)atoi(e) : 3u;
}


// 24_18 keeps team A in rows 0-1 and team B in rows 2-3 (the local slot is the row) and starts at the character select
// when 0x08B63D88+0x2E8 is 0 (as after a battle); its init copies the rows from 0x08B63D88+0xC4 (0x6C each)
static void mpm_team_rows(const int* slot) {
    const uint32_t G = 0x08B63D88u, S = G + 0x2D4;
    bool fresh = psp_l32(G + 0xC0) == 0xFFFFFFFFu;
    if (fresh) {
        for (int p = 0; p < 4; p++) {
            uint32_t b = G + 0x30 * p, f = G + 0x274 + 0x18 * p;
            memset(psp_p(b), 0, 0x30);
            psp_s32(b + 0x20, 2); psp_s32(b + 0x28, 1); psp_s32(b + 0x2C, 1);
            if (p) { psp_s32(f, 0x63); psp_s32(f + 4, 0x46); psp_s32(f + 8, 0); psp_s32(f + 0xC, 0); psp_s32(f + 0x10, 0); psp_s32(f + 0x14, 1); }
        }
        psp_s32(S + 8, 2); psp_s32(S + 0xC, 1); psp_s32(S + 0x10, 1);
    }
    psp_s32(G + 0xC0, 0);
    bool on[4] = {false, false, false, false};
    for (int k = 0; k < 4; k++) if (slot[k] >= 0 && slot[k] < 4) on[slot[k]] = true;
    for (int p = 0; p < 4; p++) {
        uint32_t r = G + 0xC4 + 0x6C * p;
        psp_s32(r + 0x40, p >= 2); psp_s32(r + 0x44, on[p]); psp_s32(r + 0x48, 1); psp_s32(r + 0x4C, p ? 1 : 0);
        int32_t lv = (int32_t)psp_l32(r + 0x50);
        if (lv < -1 || lv > 4) psp_s32(r + 0x50, 2);
        psp_s32(r + 0x54, p ? 0 : 1); psp_s32(r + 0x58, p ? 0xFFFFFFFFu : 0); psp_s32(r + 0x5C, 1);
    }
    psp_s32(G + 0x2E8, 0);
    printf("menu: local, pantalla de equipos omitida (config %s): filas activas %d %d | %d %d, entra directo a la seleccion de personajes\n",
           fresh ? "nueva" : "previa", on[0], on[1], on[2], on[3]);
}

// what the main menu does before a scene change (17_11 0x08A82C50): preload of the scene (table 0x08A84908 of 17_11,
// copied here because the overlay is not loaded), flag 0x08B63D88 + 0x2E8, start page 0
static void mpm_scene_prep(const Ctx& c, uint32_t mgr, uint32_t s) {
    static const uint32_t pre[8] = {0x089C4A20u, 0x089C4F08u, 0x089C4E60u, 0x089C4FF4u, 0x089C5170u, 0x089C51D8u, 0x089C5240u, 0x089C494Cu};
    if (s >= 1 && s <= 8) mpm_call(c, pre[s - 1], 0);
    psp_s32(0x08B63D88u + 0x2E8, 1);
    psp_s32(mgr + 0x14, 0);
}

// Back from the character select of 24_18 (state 1 -> 0, its team screen) goes to the local select: widget found by its
// vtable (+0x40 0x08A83C50, root page +0x80 vtable 0x08A745F0), state of player 1's cursor object +0x70 (+0x78 now, +0x7C next)
static int mpm_fb_off = 0, mpm_fb_new = 0;

// 24_18 rows W+0xBC+r*0x6C (+0x4C CPU, +0x50 level), cursor objects W+0x70+4p (+0x78 state, +0x80 pad entry, +0x88 fighter);
// TTT_LOCAL_OWNPICK=0 keeps vanilla (J1 picks every fighter, CPU level offered on every row but J1's)
static bool mpm_own_on() {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_LOCAL_OWNPICK"); on = !(e && *e == '0'); }
    return on != 0;
}
static bool mpm_selp_on() {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_LOCAL_PROBE"); on = e && *e == '1'; }
    return on != 0;
}
static int mpm_row_player(int row) {
    for (int k = 0; k < lmp_cfg_n && k < 4; k++) if (lmp_cfg_slot[k] == row) return k;
    return -1;
}
static uint32_t mpm_cur(uint32_t w, int p) {
    uint32_t o = psp_l32(w + 0x70 + 4 * p);
    return o >= 0x08800000u && o < 0x10000000u ? o : 0;
}

static void mpm_sel_probe(uint32_t w, int frame) {
    static uint32_t lw = 0;
    static std::string lc, lr;
    static int lines = 0;
    static uint32_t held[4];
    if (w != lw) {
        lw = w; lc.clear(); lr.clear(); memset(held, 0, sizeof held); lines = 0;
        uint32_t o = mpm_cur(w, 0);
        printf("select: W 0x%08X, cursor 0 0x%08X, datos [c+0x68] 0x%08X (W+0xB8 0x%08X), jugadores %d:", w, o, o ? psp_l32(o + 0x68) : 0, w + 0xB8, lmp_cfg_n);
        for (int k = 0; k < lmp_cfg_n && k < 4; k++) printf(" J%d fila %d", k + 1, lmp_cfg_slot[k]);
        printf("\n");
    }
    if (lines > 400) return;
    char b[512]; int n = 0;
    for (int p = 0; p < 4; p++) {
        uint32_t o = mpm_cur(w, p);
        if (!o) { n += snprintf(b + n, sizeof b - n, " | c%d -", p); continue; }
        uint32_t st = psp_l32(o + 0x78);
        int v = st < 12 ? (int)psp_l32(o + 0x8C + 4 * st) : -9;
        n += snprintf(b + n, sizeof b - n, " | c%d st %d>%d pad %d luch %d val %d", p, (int)st, (int)psp_l32(o + 0x7C), (int)psp_l32(o + 0x80), (int)psp_l32(o + 0x88), v);
    }
    if (lc != b) { lc = b; lines++; printf("select: f%d cursores%s\n", frame, b); }
    n = 0;
    for (int r = 0; r < 4; r++) {
        uint32_t row = w + 0xBC + 0x6C * r;
        int m = 0;
        for (int i = 0; i < 4; i++) if (psp_l32(row + 0x5C + 4 * i)) m |= 1 << i;
        n += snprintf(b + n, sizeof b - n, " | r%d eq %d act %d 48 %d cpu %d niv %d hum %d pad %d m%X", r, (int)psp_l32(row + 0x40), (int)psp_l32(row + 0x44),
                      (int)psp_l32(row + 0x48), (int)psp_l32(row + 0x4C), (int)psp_l32(row + 0x50), (int)psp_l32(row + 0x54), (int)psp_l32(row + 0x58), m);
    }
    if (lr != b) { lr = b; lines++; printf("select: f%d filas%s\n", frame, b); }
    for (int k = 0; k < 4; k++) {
        uint32_t e = 0x08B4E990u + 0x58u * k, pr = psp_l32(e + 0x30);
        if (pr && pr != held[k]) {
            int who = -1;
            for (int p = 0; p < 4; p++) { uint32_t o = mpm_cur(w, p); if (o && psp_l32(o + 0x80) == (uint32_t)k) who = p; }
            lines++;
            printf("select: f%d entrada %d pulsa 0x%X, cursor con esa entrada %d\n", frame, k, pr, who);
        }
        held[k] = pr;
    }
}

// pick order: humans J1..J4, then CPU rows (J1 picks them); "Sala de juego" (state 5) is skipped through the page callback
static int mpm_sel_order(uint32_t w, int* ord) {
    int n = 0;
    for (int k = 0; k < lmp_cfg_n && k < 4; k++) {
        int r = lmp_cfg_slot[k];
        if (r >= 0 && r < 4 && psp_l32(w + 0xBC + 0x6C * r + 0x44)) ord[n++] = r;
    }
    for (int r = 0; r < 4; r++) if (mpm_row_player(r) < 0 && psp_l32(w + 0xBC + 0x6C * r + 0x44)) ord[n++] = r;
    return n;
}

static void mpm_sel_label(uint32_t w) {
    int lab[4], used = 0;
    for (int r = 0; r < 4; r++) { lab[r] = mpm_row_player(r); if (lab[r] >= 0) used |= 1 << lab[r]; }
    for (int r = 0, f = 0; r < 4; r++) if (lab[r] < 0) { while (used >> f & 1) f++; lab[r] = f; used |= 1 << f; }
    int inv[4];
    for (int r = 0; r < 4; r++) inv[lab[r]] = r;
    static uint32_t done = 0, rtb[2], lw = 0;
    static int orig[2][16];
    if (w != lw) { lw = w; rtb[0] = rtb[1] = 0; }
    for (int i = 0; i < 2; i++) {
        uint32_t pg = psp_l32(w + 0x84 + 4 * i);
        if (pg < 0x08800000u || pg >= 0x10000000u) continue;
        int slot = (int)psp_l32(pg + 0x68);
        uint32_t h = psp_l32(pg + 0x44C);
        if (slot < 0 || slot > 3 || h < 0x08800000u || h >= 0x10000000u) continue;
        uint32_t tb = psp_l32(h + 4), n = psp_l32(h + 0xC), want = 0;
        if (n < 4 || n > 16 || tb < 0x08800000u || tb >= 0x10000000u) continue;
        // the page picks the instance whose state is the slot: renumber J1..J4 so its own pick is already the player's label
        if (rtb[i] != tb) { rtb[i] = tb; for (uint32_t j = 0; j < n; j++) orig[i][j] = (int)psp_l32(tb + 8 * j); }
        for (uint32_t j = 0; j < n; j++) if (orig[i][j] >= 0 && orig[i][j] < 4) psp_s32(tb + 8 * j, (uint32_t)inv[orig[i][j]]);
        for (uint32_t j = 0; j < n; j++) if ((int)psp_l32(tb + 8 * j) == slot) want = psp_l32(tb + 8 * j + 4);
        if (!want || psp_l32(h) == want) continue;
        psp_s32(h, want);
        uint32_t key = pg ^ (uint32_t)slot << 28;
        if (done != key) { done = key; printf("select: etiqueta de la fila %d -> J%d (pagina W+0x%X)\n", slot, lab[slot] + 1, 0x84 + 4 * i); }
    }
}

static void mpm_sel_open(const Ctx& c, uint32_t o, int row) {
    uint32_t pg = psp_l32(o + 0x12C);
    if (pg < 0x08800000u || pg >= 0x10000000u) return;
    uint32_t vt = psp_l32(pg + 0x40);
    psp_s32(o + 0x8C + 4 * 5, (uint32_t)row);
    mpm_call(c, psp_l32(vt + 0xCC), pg + (uint32_t)(int16_t)psp_l16(vt + 0xC8), 0, o);
}

static bool sm_busy();
static void mpm_sel_own(const Ctx& c, uint32_t w) {
    if (!mpm_own_on() || sm_busy()) return;
    static uint32_t lw = 0;
    static int logs = 0, pend[4];
    if (w != lw) { lw = w; logs = 0; for (int p = 0; p < 4; p++) pend[p] = 0; }
    for (int r = 0; r < 4; r++) {
        uint32_t row = w + 0xBC + 0x6C * r;
        if (mpm_row_player(r) >= 1 && psp_l32(row + 0x4C)) {
            psp_s32(row + 0x4C, 0);
            if (logs++ < 24) printf("select: fila %d de J%d sin nivel de CPU\n", r, mpm_row_player(r) + 1);
        }
    }
    mpm_sel_label(w);
    uint32_t back = psp_l32(0x08A793D0u);
    for (int p = 0; p < 4; p++) {
        uint32_t o = mpm_cur(w, p);
        if (!o) continue;
        int st = (int)psp_l32(o + 0x78), nx = (int)psp_l32(o + 0x7C), pad = (int)psp_l32(o + 0x80);
        if (st >= 1 && st <= 4 && nx == 5) {
            pend[p] = pad >= 0 && pad < 4 && (psp_l32(0x08B4E990u + 0x58u * pad + 0x30) & back) ? 2 : 1;
            continue;
        }
        if (st >= 6 && nx == 5) { pend[p] = 3; continue; }   // back from the stage select: the order starts again at J1
        if (st == 5 && nx == 5 && pend[p] == 3) {
            pend[p] = 0;
            int ord[4], n = mpm_sel_order(w, ord), to = n > 0 ? ord[0] : p;
            if (logs++ < 24) printf("select: atras desde la seleccion de mapa, abre la fila %d\n", to);
            mpm_sel_open(c, o, to);
            continue;
        }
        if (st != 5) { if (st != nx || st < 0) pend[p] = 0; continue; }
        if (nx != 5 || !pend[p]) continue;
        bool bk = pend[p] == 2;
        pend[p] = 0;
        int ord[4], n = mpm_sel_order(w, ord), i = 0;
        while (i < n && ord[i] != p) i++;
        int to = i >= n ? -1 : bk ? (i > 0 ? ord[i - 1] : -1) : (i + 1 < n ? ord[i + 1] : 4);
        if (to < 0) {
            if (!bk || i >= n) continue;
            for (int q = 0; q < 4; q++) if (mpm_cur(w, q)) psp_s32(mpm_cur(w, q) + 0x7C, 0);
            if (logs++ < 24) printf("select: atras desde la primera fila: vuelve a la seleccion local\n");
            continue;
        }
        if (logs++ < 24) printf("select: fila %d %s, abre %s %d\n", p, bk ? "atras" : "lista", to == 4 ? "la seleccion de mapa" : "la fila", to);
        mpm_sel_open(c, o, to);
    }
    for (int p = 0; p < 4; p++) {
        uint32_t o = mpm_cur(w, p);
        if (!o) continue;
        int st = (int)psp_l32(o + 0x78), pad = (int)psp_l32(o + 0x80), k = st >= 1 && st <= 4 ? mpm_row_player(p) : -1;
        if (k >= 1 && pad == 0) {
            psp_s32(o + 0x80, (uint32_t)k); psp_s32(o + 0x84, (uint32_t)k);
            if (logs++ < 24) printf("select: J%d elige su luchador (cursor %d, entrada %d)\n", k + 1, p, k);
        } else if (pad >= 1 && pad <= 3 && k != pad) {
            psp_s32(o + 0x80, 0); psp_s32(o + 0x84, 0);
            if (logs++ < 24) printf("select: cursor %d vuelve a J1 (entrada %d -> 0, estado %d)\n", p, pad, st);
        }
    }
}

static void mpm_fb_watch(const Ctx& c) {
    static uint32_t w = 0, last = 0, old = 0;
    static int tries = 0, seen = 0, frame = 0;
    int b = psp_bank_current();
    if (!lmp_cfg_on || !mpm_skip_teams() || b < 0 || BANKS[b].indice != 23) { w = 0; last = 0; tries = seen = frame = mpm_fb_off = 0; return; }
    if (mpm_fb_new) {   // the select pak loads again (back from a battle): new widget, the old one is stale
        mpm_fb_new = 0;
        printf("menu: local, la seleccion de personajes se vuelve a cargar (menu anterior 0x%08X, %s): busca el menu de nuevo\n", w ? w : old, mpm_fb_off ? "desactivado" : "activo");
        if (w) old = w;
        w = 0; last = 0; tries = seen = frame = mpm_fb_off = 0;
    }
    if (mpm_fb_off) return;
    if (!w) {
        if (tries >= 30 || (++frame & 7)) return;
        tries++;
        uint32_t stale = 0;
        for (uint32_t a = 0x08A90040u; a < 0x0C000000u; a += 4) {
            if (psp_l32(a) != 0x08A83C50u) continue;
            uint32_t root = psp_l32(a + 0x40), pl = psp_l32(a + 0x30);
            if (root < 0x08800000u || root >= 0x0C000000u || pl < 0x08800000u || pl >= 0x0C000000u) continue;
            if (psp_l32(root + 0x40) != 0x08A745F0u) continue;
            if (a - 0x40 == old) { stale = old; continue; }
            w = a - 0x40;
            break;
        }
        if (!w && stale && tries >= 3) w = stale;
        if (w) printf("menu: local, batalla libre 24_18 en 0x%08X (intento %d)\n", w, tries);
        return;
    }
    frame++;
    mpm_sel_own(c, w);
    if (mpm_selp_on()) mpm_sel_probe(w, frame);
    uint32_t pl = psp_l32(w + 0x70), now = psp_l32(pl + 0x78), next = psp_l32(pl + 0x7C), prev = last;
    last = now;
    if (now == 1) seen = 1;
    if (!seen || next != 0 || (prev != 1 && prev != 5)) return;
    w = 0; seen = 0; tries = 30;
    uint32_t mgr = psp_l32(0x08A7EA50u);
    printf("menu: local, atras en la seleccion de personajes (estado %u -> 0): no muestra la pantalla de equipos\n", now);
    mpm_call(c, 0x088512ACu, 0);
    mpm_call(c, 0x089C34ACu, mgr, 1);
}

// network refusals: the game's own rejection text (room box 0x08AA6714 asks 0x1AB, 0x1A9 or the notice 0x1A7) becomes the mods message
void psp_window_notice(const char* utf8);
static std::string mpn_full, mpn_det[3];
static int mpn_kind = 0;
static bool mpn_on = false, mpn_served = false, mpn_built = false;
static uint64_t mpn_t = 0, mpn_last = 0;
static uint32_t mpn_buf = 0;

void psp_net_notice(const char* full, const char* missing, const char* extra, const char* version, int kind) {
    mpn_full = full ? full : "";
    mpn_det[0] = missing ? missing : ""; mpn_det[1] = extra ? extra : ""; mpn_det[2] = version ? version : "";
    mpn_kind = kind; mpn_on = true; mpn_served = false; mpn_built = false; mpn_t = psp_time_us();
}

static void mpn_build(uint32_t lang) {
    static const char* T[2][7] = {
        {"Character/map mods differ from the host's.", "Missing: ", "Not used by the host: ", "Different version: ", "The host uses them.",
         "The order or files differ.", "Use the host's character/map mods in the same order and restart."},
        {"Tus mods de personajes/mapas no coinciden.", "Faltan: ", "El anfitri\u00F3n no usa: ", "Otra versi\u00F3n: ", "El anfitri\u00F3n s\u00ED los usa.",
         "El orden o los archivos difieren.", "Usa sus mods de personajes/mapas en el mismo orden y reinicia."}
    };
    const char* const* t = T[mpm_li(lang)];
    static const char* R[2][2] = {{"Ranked allows cosmetic mods only.", "Turn the others off (F1 > Mods)."},
                                  {"En clasificatoria solo se permiten mods est\u00E9ticos.", "Desactiva los dem\u00E1s (F1 > Mods)."}};
    std::string m = mpn_kind == 3 ? R[mpm_li(lang)][0] : t[0];
    if (mpn_kind == 3) {}
    else if (mpn_kind == 1) m += std::string(" ") + t[4];
    else if (mpn_kind == 2) m += std::string(" ") + t[5];
    else for (int k = 0; k < 3; k++) if (!mpn_det[k].empty()) m += std::string(" ") + t[1 + k] + mpn_det[k];
    if (m.size() > 70) { size_t k = 67; while (k && ((unsigned char)m[k] & 0xC0) == 0x80) k--; m = m.substr(0, k) + "..."; }
    m += "\n" + std::string(mpn_kind == 3 ? R[mpm_li(lang)][1] : t[6]);
    if (!mpn_buf) mpn_buf = psp_reserve(0x200);
    const unsigned char* p = (const unsigned char*)m.c_str();
    int n = 0;
    while (*p && n < 0xFE) {
        uint32_t u = *p++;
        if (u >= 0xE0 && p[0] && p[1]) { u = ((u & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
        else if (u >= 0xC0 && *p) u = ((u & 0x1F) << 6) | (*p++ & 0x3F);
        psp_s16(mpn_buf + 2 * n++, u);
    }
    psp_s16(mpn_buf + 2 * n, 0);
}

static bool mpn_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    uint32_t id = c.r[4];
    if (!mpn_on || (id != 0x1ABu && id != 0x1A9u && id != 0x1A7u)) return false;
    if (!mpn_built) { mpn_build(mpm_lang(c)); mpn_built = true; }
    if (!mpn_served) printf("net: aviso de mods en la caja del juego (texto 0x%X desde 0x%08X)\n", id, c.r[31] - 8);
    mpn_served = true; mpn_last = psp_time_us();
    c.r[2] = mpn_buf;
    return true;
}

static void mpn_tick() {
    if (!mpn_on) return;
    uint64_t now = psp_time_us();
    if (mpn_served) { if (now - mpn_last > 1500000u) { mpn_on = false; printf("net: aviso de mods cerrado\n"); } return; }
    if (now - mpn_t < 8000000u) return;
    mpn_on = false;
    printf("net: aviso de mods sin caja del juego en 8 s, se muestra la ventana\n");
    psp_window_notice(mpn_full.c_str());
}

// Local: the multiplayer scene leaves like Back (scene 1); once the main menu (17_11) runs, its Batalla request is
// repeated (a request of scene 3 from the multiplayer scene leaves the screen black)
static void mv_lang_tick(const Ctx& c);
// TTT_MENU_PROBE=1: per frame for 40 frames after any menu change, what was drawn and what the GPU presented
void psp_gpu_kinds(int* tex, int* flat, int* clr, uint64_t* n, float* lum);
static void mpm_probe() {
    static int on = -1, left = 0, lines = 0, f = 0;
    static uint32_t key[8];
    if (on < 0) { const char* e = getenv("TTT_MENU_PROBE"); on = e && *e == '1'; }
    if (!on || lines >= 3000) return;
    int b = psp_bank_current();
    uint32_t mgr = psp_l32(0x08A7EA50u);
    uint32_t k[8] = {(uint32_t)(b >= 0 ? BANKS[b].indice : -1), mgr ? psp_l32(mgr + 0x10) : 0, mgr ? psp_l32(mgr + 0x18) : 0, (uint32_t)mpm_mode,
                     (uint32_t)mpm_phase, (uint32_t)mpm_scr, (uint32_t)mo_tab, (uint32_t)(mpm_w != 0) | (uint32_t)ac_entry << 1};
    int tex, flat, clr; uint64_t n; float lum;
    psp_gpu_kinds(&tex, &flat, &clr, &n, &lum);
    static uint64_t pn = 0;
    if (n == pn) return;
    int pres = (int)(n - pn); pn = n;
    if (memcmp(k, key, sizeof k)) { memcpy(key, k, sizeof k); left = 40; f = 0; }
    if (left <= 0) { mpm_pk_drawn = mpm_pk_wd = 0; return; }
    left--; lines++;
    uint32_t r = mpm_w ? mpm_hld[mpm_rootk()] : 0, ri = r ? psp_l32(r) : 0, bi = mpm_w && mpm_hld[H_BG] ? psp_l32(mpm_hld[H_BG]) : 0;
    float fa[3];
    for (int i = 0; i < 3; i++) fa[i] = mpm_f(0x08B40CE0u + 0x60 * i + 0xC);
    printf("sonda menu: f%d banco %d escena %u pide %u modo %d fase %d pant %d pest %d w %u | dib %d holders %d raiz %d %.1f fondo %d %.1f | fundido %.0f %.0f %.0f | gpu tex %d plano %d borrado %d presenta %d luz %.0f\n",
           f++, (int)k[0], k[1], k[2], mpm_mode, mpm_phase, mpm_scr, mo_tab, k[7], mpm_pk_wd, mpm_pk_drawn, r ? mpm_cur_state(r) : -1, ri ? mpm_f(ri + 0x2C) : -1.0f,
           bi ? mpm_cur_state(mpm_hld[H_BG]) : -1, bi ? mpm_f(bi + 0x2C) : -1.0f, fa[0], fa[1], fa[2], tex, flat, clr, pres, lum);
    mpm_pk_drawn = mpm_pk_wd = 0;
}

void mpm_frame(const Ctx& c) {
    static int frames = 0;
    mpm_probe();
    mv_lang_tick(c);
    mpn_tick();
    ac_frame(c);
    av_frame();
    if (!(mpm_mode == MP_SUB && mpm_w)) mo_discord_frame(c);
    mpm_fb_watch(c);
    // the game's own post-battle exit reaches the main menu without passing through our room: close the online session there
    static int idle = 0;
    int bk = psp_bank_current();
    if ((net_online_on() || mo_role) && !(mpm_mode == MP_SUB && mpm_w) && bk >= 0 && BANKS[bk].indice == 16) {
        if (++idle >= 4) { printf("menu: menu principal con la sesion en linea abierta (sala %s): se cierra\n", mo_rid.c_str()); net_online_stop(); mo_role = 0; mo_dlg = DL_NONE; mo_rph = RP_CONNECT; idle = 0; }
    } else idle = 0;
    // after a ranked survival the main menu opens En linea again (same path as a Discord invite)
    static int back = 0;
    if (mpm_after_surv && !net_online_on() && !mo_role && bk >= 0 && BANKS[bk].indice == 16 && !(mpm_mode == MP_SUB && mpm_w)) {
        if (++back >= 4) {
            back = 0; mpm_after_surv = 0;
            uint32_t mgr = psp_l32(0x08A7EA50u);
            mpm_back_online = 1;
            mpm_callf(c, 0x0883A6E8u, psp_l32(0x08A78EE8u), 0, 1000.0f);
            mpm_scene_prep(c, mgr, 4);
            printf("menu: menu principal tras la supervivencia clasificatoria, abre En linea\n");
            mpm_call(c, 0x089C34ACu, mgr, 4);
            return;
        }
    } else back = 0;
    if (!mpm_battle && mpm_back_local != 1) { frames = 0; return; }
    int b = psp_bank_current();
    if (b < 0 || BANKS[b].indice != 16) { frames = 0; return; }
    if (++frames < 4) return;
    uint32_t mgr = psp_l32(0x08A7EA50u), sc = mpm_battle ? mpm_local_scene() : 4u;
    if (mpm_battle) mpm_battle = 0; else mpm_back_local = 2;
    frames = 0;
    mpm_callf(c, 0x0883A6E8u, psp_l32(0x08A78EE8u), 0, 1000.0f);
    mpm_scene_prep(c, mgr, sc);
    if (sc == 3 && mpm_skip_teams()) mpm_team_rows(mpm_lslot);
    printf("menu: local, menu principal activo, pide la escena %u (%s)\n", sc, sc == 4 ? "multijugador, seleccion local" : "Batalla");
    mpm_call(c, 0x089C34ACu, mgr, sc);
}

// local multiplayer: the free battle paks of the character select (24..29, ja en es fr de it) become the ones the LAN
// game uses (57..62, menu_free_battle_multiplayer); TTT_LOCAL_MPPAK=0 keeps them
void mpm_pak(uint32_t dir, Ctx& c) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_LOCAL_MPPAK"); on = !(e && *e == '0'); }
    if (!on || !lmp_cfg_on) return;
    int r = dir == 0x0882FFA4u ? 6 : 5;
    int16_t idx = (int16_t)c.r[r];
    if (idx < 24 || idx > 29) return;
    c.r[r] = (uint32_t)(int32_t)(idx + 33);
    mpm_fb_new = 1;
    static int logs = 0;
    if (logs++ < 12) printf("menu: local, pak %d -> %d (batalla libre multijugador) desde 0x%08X\n", idx, idx + 33, c.r[31] - 8);
}

static bool mpm_wrap(uint32_t dir, Ctx& c, void (*orig)(Ctx&)) {
    if (dir == 0x089C34ACu) {
        uint32_t s = c.r[5];
        if (s == 1 && (ac_opt == 2 || ac_back_opt)) psp_gpu_blackout(true);
        if (s == 0 || s == 2) mpm_fb_off = 1;
        if (s == 1 && lmp_cfg_on && !mpm_battle) { mpm_back_local = 1; printf("menu: local, atras desde la seleccion de personajes: vuelve a la seleccion local\n"); }
        else if ((s == 1 && !mpm_battle) || (s == 4 && !mpm_back_local)) { psp_local_join(0); lmp_cfg_on = 0; mpm_back_local = 0; }
        static int n = 0;
        if (n++ < 40) printf("menu: escena -> %u (desde 0x%08X), actual %u, pagina inicial %u\n", s, c.r[31] - 8, psp_l32(c.r[4] + 0x10), psp_l32(c.r[4] + 0x14));
        orig(c);
        return true;
    }
    if (dir == 0x089C4214u) {
        uint32_t out = c.r[4], start = c.r[5];
        orig(c);
        if (!patch_mp_menu.val) return true;
        if (!mpm_back_local) psp_local_join(0);
        uint32_t sc = psp_l32(out);
        if (!sc || start != 0) { printf("menu: escena multijugador desde la pagina %u, sin submenu\n", start); return true; }
        mpm_setup_mem();
        for (uint32_t p = psp_l32(sc); p && p < psp_l32(sc + 4); p += 4) {
            uint32_t pg = psp_l32(p);
            uint32_t i = pg ? psp_l32(pg) : 0, cp = mpm_mem + 0xF000 + 0x40 * (i & 7);
            if (!pg || i == 0 || i > 7) { if (pg && i == 0) mpm_pg0 = pg; continue; }
            if (psp_l32(pg + 4) != cp) mpm_pg_vt[i] = psp_l32(pg + 4);
            memcpy(psp_p(cp), psp_p(mpm_pg_vt[i]), 0x40);
            psp_s32(cp + 0x1C, MPM_NAT + 4 * MN_PX_UPDATE);
            psp_s32(cp + 0x24, MPM_NAT + 4 * MN_PX_DRAW);
            psp_s32(pg + 4, cp);
        }
        for (uint32_t p = psp_l32(sc); p && p < psp_l32(sc + 4); p += 4) {
            uint32_t pg = psp_l32(p);
            if (!pg || psp_l32(pg) != 0) continue;
            mpm_vt_orig = psp_l32(pg + 4);
            memcpy(psp_p(mpm_pvt()), psp_p(mpm_vt_orig), 0x40);
            psp_s32(mpm_pvt() + 0x14, MPM_NAT + 4 * MN_PG_ENTER);
            psp_s32(mpm_pvt() + 0x1C, MPM_NAT + 4 * MN_PG_UPDATE);
            psp_s32(mpm_pvt() + 0x2C, MPM_NAT + 4 * MN_PG_EXIT);
            psp_s32(mpm_pvt() + 0x24, MPM_NAT + 4 * MN_PG_DRAW);
            psp_s32(pg + 4, mpm_pvt());
            mpm_page = pg; mpm_mode = MP_SUB;
            printf("menu: escena multijugador, pagina 0x%08X con submenu (vtable 0x%08X)\n", pg, mpm_vt_orig);
            break;
        }
        return true;
    }
    return false;
}
