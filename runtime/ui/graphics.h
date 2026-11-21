#pragma once
// Graphics settings: values, graphics.cfg (boot applies it at startup) and the life bar choices. Included from menu.h

void net_discord_enable(bool on);
static const char* GW_SIZE_C[] = {"480x272", "960x544", "1280x720", "1440x816", "1600x900", "1920x1080", "1920x1200", "2560x1440"};
static const float GW_SHARP[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
static const float GW_SMOOTH[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
static const float GW_VFXMAP[] = {0.3f, 0.45f, 0.6f, 0.8f, 1.0f};
static const int GW_MSAA[] = {0, 0, 2, 4, 8, 4}, GW_FXAA[] = {0, 1, 0, 0, 0, 1};
static const int GW_CURSOR_S[] = {0, 1, 3, 5, 10};
static const int GW_WATER_V[] = {0, 2, 1, 3};   // TTT_WATER: 0 off, 1 full (the old on), 2 BT3, 3 enhanced (BT3 + water shader)

struct GwValues { int size, scale, filter, sharpness, outline, lod, gpu, vsync, fps, floating, bars, charge, discord, names, soft, cel, bolts, water, smooth, bloom, vfxmap, olmode, msaa, texf, mblur, lip, blink, dyncol, sel3d, genr, genq, skipcr, trwait, blastend, glow, camsil, bt3fx, atm, sun, cursor, hud, barsback, bt3cam, trwheel, map3d, genm, textn, celglow, sndw, selv, storm, bubbles, haze, rain, wet, chg2, smotion, impact, fpsc, selentry, selstyle; };
static GwValues gw_v, gw_start, gw_prev;
static bool gw_loaded = false;   // gw_start read once (this window or the settings menu)
static int gw_prev_fit = 2; static bool gw_prev_fullscreen = false;
static const GwValues GW_DEFAULT = {0, 1, 2, 1, 2, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 3, 2, 2, 1, 1, 1, 0, 0, 1, 0, 3, 1, 0, 0, 1, 1, 1, 2, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0};
static std::string gw_size_other;

static int gw_env(const char* k, int def) { const char* e = getenv(k); return e && *e ? atoi(e) : def; }

static void gw_read() {
    const char* e = getenv("TTT_WINDOW");
    gw_v.size = 2; gw_size_other.clear();
    if (e && *e) {
        gw_v.size = -1;
        for (int i = 0; i < 8; i++) if (!strcmp(e, GW_SIZE_C[i])) gw_v.size = i;
        if (gw_v.size < 0) { gw_size_other = e; gw_v.size = 8; }
    }
    e = getenv("TTT_SCALE");
    gw_v.scale = !e || !*e || e[0] == 'a' || e[0] == 'A' ? 0 : std::max(1, std::min(10, atoi(e)));
    gw_v.filter = std::max(0, std::min(2, filtro_modo()));
    float n = sharpness(); gw_v.sharpness = 0;
    for (int i = 1; i < 5; i++) if (fabsf(GW_SHARP[i] - n) < fabsf(GW_SHARP[gw_v.sharpness] - n)) gw_v.sharpness = i;
    const char* b = getenv("TTT_OUTLINE");
    gw_v.outline = std::max(0, std::min(3, (int)((b && *b ? atof(b) : 2.0) + 0.5)));
    gw_v.lod = gw_env("TTT_LOD", 4) != 0;
    gw_v.gpu = gw_env("TTT_GLRAST", 1) != 0;
    gw_v.vsync = gw_env("TTT_VSYNC", 0) != 0;
    gw_v.fps = gw_env("TTT_BATTLE_FPS", 30) == 60;
    gw_v.floating = gw_env("TTT_FLOAT", 1) != 0;
    gw_v.bars = std::max(0, std::min(3, gw_env("TTT_HUD_BT3", 0)));
    gw_v.charge = gw_env("TTT_CHARGE_LIST", 1) != 0;
    gw_v.discord = gw_env("TTT_DISCORD", 1) != 0;
    gw_v.names = gw_env("TTT_BAR_NAMES", 1) != 0;
    gw_v.soft = gw_env("TTT_SOFT_SHADOWS", 0) != 0;
    gw_v.cel = std::max(0, std::min(4, gw_env("TTT_CEL", 0)));
    gw_v.bolts = gw_env("TTT_BOLTS", 0) != 0;
    gw_v.water = std::max(0, std::min(3, gw_env("TTT_WATER", 2)));
    gw_v.bloom = std::max(0, std::min(2, gw_env("TTT_FX_BLOOM", 0)));
    gw_v.olmode = gw_env("TTT_OUTLINE_MODE", 1) != 0;
    auto near5 = [](const char* k, float def, const float* t) { const char* e = getenv(k); float v = e && *e ? (float)atof(e) : def; int b = 0; for (int i = 1; i < 5; i++) if (fabsf(t[i] - v) < fabsf(t[b] - v)) b = i; return b; };
    gw_v.smooth = near5("TTT_CEL_SMOOTH", 0.0f, GW_SMOOTH);
    gw_v.vfxmap = near5("TTT_FX_VFXMAP", 0.6f, GW_VFXMAP);
    int ms = gw_env("TTT_MSAA", 0), fx = gw_env("TTT_FXAA", 0) != 0; ms = ms >= 8 ? 8 : ms >= 4 ? 4 : ms >= 2 ? 2 : 0;
    gw_v.msaa = 0;
    for (int i = 0; i < 6; i++) if (GW_MSAA[i] == ms && GW_FXAA[i] == fx) gw_v.msaa = i;
    if (ms && fx && ms != 4) gw_v.msaa = 5;
    gw_v.texf = std::max(0, std::min(2, gw_env("TTT_TEXFILTER", 0)));
    gw_v.mblur = std::max(0, std::min(3, gw_env("TTT_FX_MBLUR", 2)));
    gw_v.lip = gw_env("TTT_LIPSYNC", 1) != 0;
    gw_v.blink = gw_env("TTT_BLINK", 1) != 0;
    gw_v.dyncol = gw_env("TTT_DYN_COLORS", 1) != 0;
    gw_v.sel3d = gw_env("TTT_SELECT_3D", 0) != 0;
    gw_v.genr = gw_env("TTT_GEN_RENDERS", 0) != 0;
    int q = gw_env("TTT_GEN_RENDERS_Q", 2); gw_v.genq = q >= 4 ? 2 : q >= 2 ? 1 : 0;
    gw_v.skipcr = gw_env("TTT_SKIP_CREDITS", 1) != 0;
    gw_v.trwait = std::max(0, std::min(4, gw_env("TTT_TRANSFORM_WAIT", 2) + 1));
    gw_v.blastend = gw_env("TTT_BLAST_END", 1) != 0;
    gw_v.glow = gw_env("TTT_TEAM_GLOW", 0) != 0;
    gw_v.camsil = gw_env("TTT_CAM_SILHOUETTE", 0) != 0;
    gw_v.bt3fx = gw_env("TTT_CEL_BT3FX", 1) != 0;
    gw_v.atm = std::max(0, std::min(2, gw_env("TTT_FX_ATM", 0)));
    gw_v.sun = std::max(0, std::min(2, gw_env("TTT_FX_SUN", 0)));
    int cs = gw_env("TTT_HIDE_CURSOR", 3); gw_v.cursor = 0;
    for (int i = 1; i < 5; i++) if (abs(GW_CURSOR_S[i] - cs) < abs(GW_CURSOR_S[gw_v.cursor] - cs)) gw_v.cursor = i;
    gw_v.hud = gw_env("TTT_BATTLE_HUD", 1) != 0;
    gw_v.barsback = gw_env("TTT_BARS_BEHIND", 0) != 0;
    gw_v.bt3cam = gw_env("TTT_BT3_CAM", 0) != 0;
    gw_v.trwheel = gw_env("TTT_TR_WHEEL", 1) != 0;
    gw_v.map3d = gw_env("TTT_MAP_3D", 0) != 0;
    gw_v.genm = gw_env("TTT_GEN_MAP_RENDERS", 0) != 0;
    gw_v.textn = gw_env("TTT_TEXT_NAMES", 1) != 0;
    gw_v.celglow = gw_env("TTT_CEL_GLOW", 1) != 0;
    gw_v.sndw = gw_env("TTT_SND_WATER", 1) != 0;
    gw_v.selv = gw_env("TTT_SEL_VOICE", 1) != 0;
    gw_v.selentry = gw_env("TTT_SEL3D_ENTRY", 0) == 1 ? 1 : 0;
    gw_v.selstyle = gw_env("TTT_SEL_STYLE", 0) == 1 ? 1 : 0;
    gw_v.storm = gw_env("TTT_BT3_STORM", 1) != 0;
    gw_v.bubbles = gw_env("TTT_BT3_BUBBLES", 1) != 0;
    gw_v.haze = gw_env("TTT_BT3_HAZE", 1) != 0;
    gw_v.rain = gw_env("TTT_RAIN", 0) != 0;
    gw_v.wet = gw_env("TTT_WET", 0) != 0;
    gw_v.chg2 = gw_env("TTT_CHARGE2", 0) != 0;
    gw_v.smotion = std::min(2, std::max(0, gw_env("TTT_SMOOTH", 0)));
    gw_v.fpsc = std::min(4, std::max(0, gw_env("TTT_FPS_COUNTER", 0)));
    { int im = gw_env("TTT_FX_IMPACT", 1), sk = gw_env("TTT_FX_SHAKE", 1); gw_v.impact = im >= 2 ? 3 : im == 1 ? 2 : sk ? 1 : 0; }
}

static void gw_save() {
    FILE* f = fopen("graphics.cfg", "w");
    if (!f) return;
    const char* size = gw_v.size == 8 ? gw_size_other.c_str() : GW_SIZE_C[gw_v.size];
    if (gw_v.scale) fprintf(f, "TTT_SCALE=%d\n", gw_v.scale); else fprintf(f, "TTT_SCALE=auto\n");
    fprintf(f, "TTT_WINDOW=%s\nTTT_FILTER=%d\nTTT_SHARPNESS=%.2f\nTTT_OUTLINE=%d\nTTT_LOD=%d\nTTT_GLRAST=%d\nTTT_VSYNC=%d\nTTT_BATTLE_FPS=%d\nTTT_FLOAT=%d\nTTT_HUD_BT3=%d\nTTT_CHARGE_LIST=%d\nTTT_DISCORD=%d\nTTT_BAR_NAMES=%d\nTTT_SOFT_SHADOWS=%d\nTTT_CEL=%d\nTTT_BOLTS=%d\nTTT_WATER=%d\nTTT_CEL_SMOOTH=%.2f\nTTT_FX_BLOOM=%d\nTTT_FX_VFXMAP=%.2f\nTTT_OUTLINE_MODE=%d\nTTT_MSAA=%d\nTTT_FXAA=%d\nTTT_TEXFILTER=%d\nTTT_FX_MBLUR=%d\nTTT_LIPSYNC=%d\nTTT_BLINK=%d\nTTT_DYN_COLORS=%d\nTTT_SELECT_3D=%d\n",
            size, gw_v.filter, GW_SHARP[gw_v.sharpness], gw_v.outline, gw_v.lod ? 4 : 0, gw_v.gpu, gw_v.vsync, gw_v.fps ? 60 : 30, gw_v.floating, gw_v.bars, gw_v.charge, gw_v.discord, gw_v.names, gw_v.soft, gw_v.cel, gw_v.bolts, gw_v.water,
            GW_SMOOTH[gw_v.smooth], gw_v.bloom, GW_VFXMAP[gw_v.vfxmap], gw_v.olmode, GW_MSAA[gw_v.msaa], GW_FXAA[gw_v.msaa], gw_v.texf, gw_v.mblur, gw_v.lip, gw_v.blink, gw_v.dyncol, gw_v.sel3d);
    _putenv_s("TTT_FXAA", GW_FXAA[gw_v.msaa] ? "1" : "0");
    char nb[16];
    snprintf(nb, sizeof(nb), "%.2f", GW_SMOOTH[gw_v.smooth]); _putenv_s("TTT_CEL_SMOOTH", nb);
    snprintf(nb, sizeof(nb), "%.2f", GW_VFXMAP[gw_v.vfxmap]); _putenv_s("TTT_FX_VFXMAP", nb);
    { char bb[8]; snprintf(bb, sizeof(bb), "%d", gw_v.bloom); _putenv_s("TTT_FX_BLOOM", bb); }
    _putenv_s("TTT_OUTLINE_MODE", gw_v.olmode ? "1" : "0");
    _putenv_s("TTT_FLOAT", gw_v.floating ? "1" : "0");
    _putenv_s("TTT_HUD_BT3", std::to_string(gw_v.bars).c_str());
    _putenv_s("TTT_CHARGE_LIST", gw_v.charge ? "1" : "0");
    _putenv_s("TTT_DISCORD", gw_v.discord ? "1" : "0");
    _putenv_s("TTT_BAR_NAMES", gw_v.names ? "1" : "0");
    _putenv_s("TTT_SOFT_SHADOWS", gw_v.soft ? "1" : "0");
    { char cb[8]; snprintf(cb, sizeof(cb), "%d", gw_v.cel); _putenv_s("TTT_CEL", cb); }
    _putenv_s("TTT_BOLTS", gw_v.bolts ? "1" : "0");
    { char wb[4]; snprintf(wb, sizeof(wb), "%d", gw_v.water); _putenv_s("TTT_WATER", wb); }
    snprintf(nb, sizeof(nb), "%d", gw_v.mblur); _putenv_s("TTT_FX_MBLUR", nb);
    _putenv_s("TTT_LIPSYNC", gw_v.lip ? "1" : "0");
    _putenv_s("TTT_BLINK", gw_v.blink ? "1" : "0");
    _putenv_s("TTT_DYN_COLORS", gw_v.dyncol ? "1" : "0");
    _putenv_s("TTT_SELECT_3D", gw_v.sel3d ? "1" : "0");
    _putenv_s("TTT_GEN_RENDERS", gw_v.genr ? "1" : "0");
    _putenv_s("TTT_GEN_RENDERS_Q", gw_v.genq == 2 ? "4" : gw_v.genq == 1 ? "2" : "1");
    fprintf(f, "TTT_GEN_RENDERS=%d\nTTT_GEN_RENDERS_Q=%d\nTTT_SKIP_CREDITS=%d\n", gw_v.genr, gw_v.genq == 2 ? 4 : gw_v.genq == 1 ? 2 : 1, gw_v.skipcr);
    _putenv_s("TTT_SKIP_CREDITS", gw_v.skipcr ? "1" : "0");
    fprintf(f, "TTT_TRANSFORM_WAIT=%d\n", gw_v.trwait - 1);
    snprintf(nb, sizeof(nb), "%d", gw_v.trwait - 1); _putenv_s("TTT_TRANSFORM_WAIT", nb);
    fprintf(f, "TTT_BLAST_END=%d\n", gw_v.blastend);
    _putenv_s("TTT_BLAST_END", gw_v.blastend ? "1" : "0");
    fprintf(f, "TTT_TEAM_GLOW=%d\n", gw_v.glow);
    _putenv_s("TTT_TEAM_GLOW", gw_v.glow ? "1" : "0");
    fprintf(f, "TTT_CAM_SILHOUETTE=%d\n", gw_v.camsil);
    _putenv_s("TTT_CAM_SILHOUETTE", gw_v.camsil ? "1" : "0");
    fprintf(f, "TTT_CEL_BT3FX=%d\n", gw_v.bt3fx);
    _putenv_s("TTT_CEL_BT3FX", gw_v.bt3fx ? "1" : "0");
    fprintf(f, "TTT_FX_ATM=%d\nTTT_FX_SUN=%d\n", gw_v.atm, gw_v.sun);
    { char ab[8]; snprintf(ab, sizeof(ab), "%d", gw_v.atm); _putenv_s("TTT_FX_ATM", ab); snprintf(ab, sizeof(ab), "%d", gw_v.sun); _putenv_s("TTT_FX_SUN", ab); }
    fprintf(f, "TTT_HIDE_CURSOR=%d\n", GW_CURSOR_S[gw_v.cursor]);
    { char hb[8]; snprintf(hb, sizeof(hb), "%d", GW_CURSOR_S[gw_v.cursor]); _putenv_s("TTT_HIDE_CURSOR", hb); }
    fprintf(f, "TTT_BATTLE_HUD=%d\n", gw_v.hud);
    _putenv_s("TTT_BATTLE_HUD", gw_v.hud ? "1" : "0");
    fprintf(f, "TTT_BARS_BEHIND=%d\n", gw_v.barsback);
    _putenv_s("TTT_BARS_BEHIND", gw_v.barsback ? "1" : "0");
    fprintf(f, "TTT_BT3_CAM=%d\n", gw_v.bt3cam);
    _putenv_s("TTT_BT3_CAM", gw_v.bt3cam ? "1" : "0");
    fprintf(f, "TTT_TR_WHEEL=%d\n", gw_v.trwheel);
    _putenv_s("TTT_TR_WHEEL", gw_v.trwheel ? "1" : "0");
    fprintf(f, "TTT_MAP_3D=%d\n", gw_v.map3d);
    _putenv_s("TTT_MAP_3D", gw_v.map3d ? "1" : "0");
    fprintf(f, "TTT_GEN_MAP_RENDERS=%d\n", gw_v.genm);
    _putenv_s("TTT_GEN_MAP_RENDERS", gw_v.genm ? "1" : "0");
    fprintf(f, "TTT_TEXT_NAMES=%d\n", gw_v.textn);
    _putenv_s("TTT_TEXT_NAMES", gw_v.textn ? "1" : "0");
    fprintf(f, "TTT_CEL_GLOW=%d\n", gw_v.celglow);
    _putenv_s("TTT_CEL_GLOW", gw_v.celglow ? "1" : "0");
    fprintf(f, "TTT_SND_WATER=%d\n", gw_v.sndw);
    _putenv_s("TTT_SND_WATER", gw_v.sndw ? "1" : "0");
    fprintf(f, "TTT_SEL_VOICE=%d\n", gw_v.selv);
    _putenv_s("TTT_SEL_VOICE", gw_v.selv ? "1" : "0");
    fprintf(f, "TTT_SEL3D_ENTRY=%d\n", gw_v.selentry);
    _putenv_s("TTT_SEL3D_ENTRY", gw_v.selentry ? "1" : "0");
    fprintf(f, "TTT_SEL_STYLE=%d\n", gw_v.selstyle);
    _putenv_s("TTT_SEL_STYLE", gw_v.selstyle ? "1" : "0");
    fprintf(f, "TTT_BT3_STORM=%d\nTTT_BT3_BUBBLES=%d\nTTT_BT3_HAZE=%d\n", gw_v.storm, gw_v.bubbles, gw_v.haze);
    _putenv_s("TTT_BT3_STORM", gw_v.storm ? "1" : "0"); _putenv_s("TTT_BT3_BUBBLES", gw_v.bubbles ? "1" : "0"); _putenv_s("TTT_BT3_HAZE", gw_v.haze ? "1" : "0");
    fprintf(f, "TTT_RAIN=%d\nTTT_WET=%d\nTTT_CHARGE2=%d\n", gw_v.rain, gw_v.wet, gw_v.chg2);
    _putenv_s("TTT_CHARGE2", gw_v.chg2 ? "1" : "0");
    _putenv_s("TTT_RAIN", gw_v.rain ? "1" : "0"); _putenv_s("TTT_WET", gw_v.wet ? "1" : "0");
    fprintf(f, "TTT_SMOOTH=%d\nTTT_FPS_COUNTER=%d\n", gw_v.smotion, gw_v.fpsc);
    _putenv_s("TTT_FPS_COUNTER", std::to_string(gw_v.fpsc).c_str());
    _putenv_s("TTT_SMOOTH", gw_v.smotion == 2 ? "2" : gw_v.smotion ? "1" : "0");
    {   // Impact effects: Off, Shake, Lines + shake, Flash frames + shake
        const char* im = gw_v.impact == 3 ? "2" : gw_v.impact == 2 ? "1" : "0";
        fprintf(f, "TTT_FX_IMPACT=%s\nTTT_FX_SHAKE=%d\n", im, gw_v.impact > 0 ? 1 : 0);
        _putenv_s("TTT_FX_IMPACT", im); _putenv_s("TTT_FX_SHAKE", gw_v.impact > 0 ? "1" : "0");
    }
    net_discord_enable(gw_v.discord != 0);
    fclose(f);
}

static bool gw_hudmod(const char* rel) {   // an enabled mod carries this HUD file
    if (mods_root_global.empty()) return false;
    std::error_code ec;
    for (auto& m : mods_list(mods_root_global)) if (m.enabled && std::filesystem::exists(std::filesystem::u8path(mods_root_global + "/" + m.id + "/" + rel), ec)) return true;
    return false;
}
// "Life bars" entries: Original, Original (Full), then BT3 / SZ only with their mod installed
static int gw_bars_v[4], gw_bars_n = 0;
static void gw_bars_list() {
    gw_bars_n = 0;
    for (int v = 0; v < 4; v++)
        if (v < 2 || (v == 2 && gw_hudmod("BT3Hud/common/gauge_06.png")) || (v == 3 && gw_hudmod("SZHud/layout.txt"))) { gw_bars_v[gw_bars_n++] = v; }
}
static int gw_bars_sel() { for (int i = 0; i < gw_bars_n; i++) if (gw_bars_v[i] == gw_v.bars) return i; return 1; }
// settings read only at startup: if they differ from the startup values a restart is needed
static bool gw_needs_restart() {
    const GwValues& a = gw_v; const GwValues& b = gw_start;
    return a.size != b.size || a.scale != b.scale || a.outline != b.outline || a.lod != b.lod || a.gpu != b.gpu || a.vsync != b.vsync || a.fps != b.fps || GW_MSAA[a.msaa] != GW_MSAA[b.msaa] || a.texf != b.texf || a.dyncol != b.dyncol || a.sel3d != b.sel3d;
}
