#pragma once
// in-game settings menu (F1 or Start+Select): the game's own menu pieces drawn over the last frame; the game is frozen while it is
// open, except in network sessions (drawn over the running game, the local input goes to the menu). F2 = modding tools
#include <map>
#include <functional>

bool psp_ui_piece(const char* name, int ent, int tex, int rw, int rh, int* w, int* h, std::vector<uint32_t>* px);
bool psp_ui_text(int style, float size, const std::string& s, int cw, int ch, int q, int align, int* w, int* h, std::vector<uint32_t>* px);
float psp_ui_text_w(int style, float size, const std::string& s);
int psp_net_live();
static bool enfocada();
static void cursor_update();
static void ol_leave();
static void ol_open_id(const std::string& id);
static void ol_confirm(int c);
static void ol_publish(const std::string& folder);
static bool ov_lib_page();

struct OvImg { int w = 0, h = 0; std::vector<uint32_t> px; };
struct OvTex { GLuint t = 0; int w = 0, h = 0; };
struct OvPiece { const char* pk; int ent, tex, x, y, w, h; uint32_t fb; };
enum { OP_PANEL, OP_DIALOG, OP_TITLE, OP_TAB_OFF, OP_TAB_ON, OP_ROW_OFF, OP_ROW_ON, OP_ARROW_L, OP_ARROW_R, OP_CURSOR, OP_BUTTON, OP_SB_TRACK, OP_SB_KNOB, OP_N };
static const char* OV_BUI = "battle_ui_en.pak";
static const char* OV_MP = "menu_multiplayer.pak";
static const OvPiece OV_P[OP_N] = {
    {OV_BUI, 172, 1, 0, 0, 32, 154, 0xE0802010u}, {OV_MP, 60, 0, 2, 4, 60, 141, 0xE0802010u}, {OV_BUI, 170, 1, 0, 0, 64, 21, 0xFFC06020u},
    {OV_MP, 25, 0, 0, 40, 256, 24, 0xFF904018u}, {OV_MP, 25, 0, 0, 16, 256, 24, 0xFF20B0F0u}, {OV_BUI, 178, 1, 0, 0, 433, 23, 0xFF904018u},
    {OV_BUI, 178, 1, 0, 25, 433, 23, 0xFF20B0F0u}, {OV_MP, 31, 0, 0, 0, 16, 16, 0xFFFFFFFFu}, {OV_MP, 31, 0, 16, 0, 16, 16, 0xFFFFFFFFu},
    {OV_BUI, 174, 0, 1, 1, 62, 20, 0x8040E0FFu}, {OV_MP, 66, 0, 0, 34, 64, 20, 0xFF20B0F0u},
    {OV_BUI, 184, 0, 2, 0, 4, 32, 0xFF904018u}, {OV_BUI, 184, 0, 8, 0, 8, 24, 0xFF20B0F0u}};

// pages: rows built from the live values (headers, values changed with left/right, actions)
enum { RK_HEAD, RK_VALUE, RK_ACTION, RK_BIND, RK_MOD };
struct OvRow {
    int kind = RK_VALUE;
    const char* en = "";
    const char* es = "";
    std::function<int()> get;
    std::function<void(int)> set;
    std::vector<std::string> names;
    std::function<void()> act;
    std::string tag;
    bool restart = false, on = true;
    int tag2 = 0;   // 1 experimental, 2 not recommended, 3 destructive
    int action = -1;   // RK_BIND: input action
};
struct OvTabName { const char* en; const char* es; };
static const OvTabName OV_TABS[] = {{"System", "Sistema"}, {"Graphics", "Gráficos"}, {"Sound", "Sonido"}, {"Controls", "Controles"}, {"Mods", "Mods"}, {"Developer", "Desarrollo"}};
static const OvTabName OV_SUBS[] = {{"Display", "Pantalla"}, {"Effects", "Efectos"}, {"Characters", "Personajes"}, {"Battle", "Combate"}, {"Renders", "Renders"}, {"Performance", "Rendimiento"}};
static const OvTabName OV_MSUBS[] = {{"Installed", "Instalados"}, {"Library", "Biblioteca"}};
static const OvTabName OV_CSUBS[] = {{"Battle", "Combate"}, {"Specials", "Especiales"}, {"Menus", "Menús"}, {"Story", "Historia"}, {"Rumble", "Vibración"}};
enum { OV_NTABS = 6, OV_TAB_GFX = 1, OV_TAB_CTL = 3 };
static int ov_subs_t[OV_NTABS];

static bool ov_open = false, ov_net = false, ov_hold = false;
static int ov_tab = 0, ov_sub = 0, ov_row = 0, ov_confirm = 0, ov_yes = 0, ov_lang_boot = 0;   // ov_row: rows.size() restart button; confirm 2 quit, 4 restart needed
static float ov_scroll = 0;
// where the menu was left: it opens there again during this session
static bool ov_mem = false;
static int ov_mem_tab = 0, ov_mem_sub = 0, ov_mem_row = 0, ov_mem_subs[OV_NTABS];
static float ov_mem_scroll = 0;
static bool ov_mods_changed = false;
static bool ov_queued[2];
static uint32_t ov_prev = 0;
static unsigned ov_rep_t = 0, ov_t0 = 0;
static bool ov_combo_prev = true, ov_f1_prev = true, ov_lb_prev = true, ov_rb_prev = true;
static uint32_t ov_kprev = 0;
static int ov_rs_prev = 0;
static unsigned ov_rs_t = 0;
static POINT ov_mouse_last = {-1, -1};
static bool ov_mouse = false;   // mouse mode: only the hovered row is highlighted
static int ov_hover = -1;
static float ov_fx = 0, ov_fy = 0, ov_fs = 1;
static GLuint ov_prog = 0, ov_white = 0;
static GLint ov_u_r = -1, ov_u_k = -1;
static std::map<std::string, OvTex> ov_cache;
static OvImg ov_src[OP_N];
static bool ov_src_done[OP_N];

static bool ov_es() { return (ov_lang_boot ? ov_lang_boot : language) == 3; }
static bool ov_need_restart() { return ov_mods_changed || (ov_lang_boot && language != ov_lang_boot) || (gw_loaded && gw_needs_restart()); }
static const char* ov_s(const char* en, const char* es) { return ov_es() ? es : en; }

static const OvImg& ov_piece(int i) {
    if (ov_src_done[i]) return ov_src[i];
    ov_src_done[i] = true;
    const OvPiece& p = OV_P[i];
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    OvImg& o = ov_src[i];
    if (psp_ui_piece(p.pk, p.ent, p.tex, p.x + p.w, p.y + p.h, &w, &h, &px)) {
        o.w = p.w; o.h = p.h; o.px.resize((size_t)p.w * p.h);
        for (int y = 0; y < p.h; y++) memcpy(&o.px[(size_t)y * p.w], &px[(size_t)(p.y + y) * w + p.x], (size_t)p.w * 4);
    } else {
        printf("ajustes: pieza %d no disponible (%s entrada %d textura %d), color plano\n", i, p.pk, p.ent, p.tex);
        o.w = p.w; o.h = p.h; o.px.assign((size_t)p.w * p.h, p.fb);
    }
    return o;
}

// stretch to W x H: l/r columns kept (mirror: right = left flipped, centre = columns c0..c1), t/b rows kept, middle rows tiled
static OvImg ov_slice(const OvImg& s, int W, int H, int l, int t, int r, int b, bool mirror, int c0, int c1) {
    OvImg o; o.w = W; o.h = H; o.px.assign((size_t)W * H, 0);
    if (!s.w || !s.h || W < 1 || H < 1) return o;
    if (mirror) r = l;
    if (l + r > W) { l = W / 2; r = W - l; }
    if (t + b > H) { t = H / 2; b = H - t; }
    int mh = s.h - t - b;
    for (int X = 0; X < W; X++) {
        int sx;
        if (X < l) sx = X;
        else if (X >= W - r) sx = mirror ? W - 1 - X : s.w - (W - X);
        else if (mirror) sx = c0 + (int)((long long)(X - l) * (c1 - c0 + 1) / std::max(1, W - l - r));
        else sx = l + (int)((long long)(X - l) * std::max(1, s.w - l - r) / std::max(1, W - l - r));
        sx = sx < 0 ? 0 : sx >= s.w ? s.w - 1 : sx;
        for (int Y = 0; Y < H; Y++) {
            int sy = Y < t ? Y : Y >= H - b ? s.h - (H - Y) : mh > 0 ? t + (Y - t) % mh : t;
            sy = sy < 0 ? 0 : sy >= s.h ? s.h - 1 : sy;
            o.px[(size_t)Y * W + X] = s.px[(size_t)sy * s.w + sx];
        }
    }
    return o;
}

static OvTex ov_upload(const OvImg& im) {
    OvTex t; t.w = im.w; t.h = im.h;
    glGenTextures(1, &t.t);
    glBindTexture(GL_TEXTURE_2D, t.t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::vector<uint32_t> pm(im.px.size());   // premultiplied: the white of transparent texels never bleeds into the edges
    for (size_t i = 0; i < pm.size(); i++) {
        uint32_t v = im.px[i], a = v >> 24;
        pm[i] = ((v & 255) * a / 255) | ((v >> 8 & 255) * a / 255) << 8 | ((v >> 16 & 255) * a / 255) << 16 | a << 24;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pm.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    return t;
}

// a piece composed at w x h (game pixels)
static const OvTex& ov_sprite(int i, int w, int h) {
    std::string k = "p" + std::to_string(i) + "|" + std::to_string(w) + "x" + std::to_string(h);
    auto it = ov_cache.find(k);
    if (it != ov_cache.end()) return it->second;
    const OvImg& s = ov_piece(i);
    OvImg o;
    int nw = h > 0 && s.h > 0 ? std::max(1, (int)lroundf((float)w * s.h / h)) : w;   // strips: composed at their own height, drawn scaled
    switch (i) {
        case OP_PANEL: o = ov_slice(s, w, h, 15, 15, 4, 4, false, 0, 0); break;
        case OP_DIALOG: o = ov_slice(s, w, h, 14, 15, 4, 6, false, 0, 0); break;
        case OP_TITLE: o = ov_slice(s, nw, s.h, 61, 0, 0, 0, true, 61, 63); break;
        case OP_TAB_OFF: case OP_TAB_ON: o = ov_slice(s, nw, s.h, 28, 0, 16, 0, false, 0, 0); break;
        case OP_ROW_OFF: case OP_ROW_ON: o = ov_slice(s, nw, s.h, 24, 0, 40, 0, false, 0, 0); break;
        case OP_CURSOR: o = ov_slice(s, nw, s.h, 51, 0, 0, 0, true, 52, 61); break;
        case OP_BUTTON: o = ov_slice(s, nw, s.h, 64, 0, 0, 0, true, 63, 63); break;
        default: o = s; break;
    }
    return ov_cache[k] = ov_upload(o);
}

static const OvTex* ov_text(int style, float size, const std::string& s, int cw, int ch, int q, int align) {
    std::string k = "t" + std::to_string(style) + "|" + std::to_string((int)(size * 100)) + "|" + std::to_string(align) + "|" + std::to_string(cw) + "x" + std::to_string(ch) + "|" + std::to_string(q) + "|" + s;
    auto it = ov_cache.find(k);
    if (it != ov_cache.end()) return it->second.t ? &it->second : nullptr;
    OvImg im;
    if (!psp_ui_text(style, size, s, cw, ch, q, align, &im.w, &im.h, &im.px)) { ov_cache[k] = OvTex(); return nullptr; }
    return &(ov_cache[k] = ov_upload(im));
}

// button prompts: the port's adaptive icons, keyboard or controller by the device in use
static const OvTex* ov_icon(uint32_t code) {
    if (!code) return nullptr;
    int fam = pad_family;
    std::string k = "i" + std::to_string(code) + "|" + std::to_string(fam);
    auto it = ov_cache.find(k);
    if (it != ov_cache.end()) return it->second.t ? &it->second : nullptr;
    std::lock_guard<std::recursive_mutex> lock_(icon_mx);
    int om = ic_mode, of = ic_fam;
    ic_mode = (code >> 24) == 5; ic_fam = fam;
    const int H = 40;
    int W = H;
    uint32_t t = code >> 24, v = code & 0xFFFFFF;
    wchar_t lab[64]; icon_label(code, lab, 64);
    if (t == 1) W = std::max(H, (int)(icon_text_w(lab, (int)(H * 0.62f)) + H * 0.45f) + 2);
    else if (t == 5 && ((v >= P_L1 && v <= P_R2) || v == P_SELECT || v == P_START)) W = std::max((int)(H * 1.5f), (int)(icon_text_w(lab, (int)(H * 0.6f)) + H * 0.7f) + 2);
    OvImg im; im.w = W; im.h = H; im.px.assign((size_t)W * H, 0);
    Cv c{W, H, im.px.data()};
    icon_draw(c, 0, 0, (float)W, (float)H, code, IM_ICON);
    ic_mode = om; ic_fam = of;
    return &(ov_cache[k] = ov_upload(im));
}
static uint32_t ov_bind(int a, bool pad) {   // first key (keyboard) or button (controller) of a menu action
    const uint32_t* l = binds_of(a, pad);
    uint32_t any = 0;
    for (int k = 0; k < N_BINDS; k++) {
        if (!l[k]) continue;
        uint32_t t = l[k] >> 24;
        if (pad ? t == 5 : t == 1) return l[k];
        if (!pad && t == 2 && !any) any = l[k];
    }
    return any;
}
static int ov_dev = 0, ov_im_prev = -1;   // device the menu shows: 0 keyboard and mouse, 1 controller
static bool ov_pad_prev = true;
static bool ov_pad() { return ov_dev == 1; }
static uint32_t ov_code_tab(int d) { return ov_pad() ? ov_bind(d < 0 ? A_M_PAGEL : A_M_PAGER, true) : C_KEY(d < 0 ? 'Q' : 'E'); }
static uint32_t ov_code_sub(int d) { return ov_pad() ? C_PAD(d < 0 ? P_RSL : P_RSR) : C_KEY(d < 0 ? 'Z' : 'C'); }

static const char* OV_VS =
    "#version 330 core\n"
    "uniform vec4 r;\n"
    "out vec2 uv;\n"
    "void main(){\n"
    "  int i = gl_VertexID;\n"
    "  vec2 c = vec2(i == 1 || i == 2 || i == 4 ? 1.0 : 0.0, i == 2 || i == 4 || i == 5 ? 1.0 : 0.0);\n"
    "  uv = c;\n"
    "  gl_Position = vec4(mix(r.x, r.z, c.x), mix(r.y, r.w, c.y), 0.0, 1.0);\n"
    "}\n";
static const char* OV_FS =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "uniform sampler2D t;\n"
    "uniform vec4 k;\n"
    "out vec4 o;\n"
    "void main(){ o = texture(t, uv) * vec4(k.rgb * k.a, k.a); }\n";

static int ov_cw = 1, ov_ch = 1;
static void ov_quad(GLuint t, float x, float y, float w, float h, uint32_t rgb, float a) {
    float X0 = ov_fx + x * ov_fs, Y0 = ov_fy + y * ov_fs, X1 = X0 + w * ov_fs, Y1 = Y0 + h * ov_fs;
    glUniform4f(ov_u_r, X0 / ov_cw * 2 - 1, 1 - Y0 / ov_ch * 2, X1 / ov_cw * 2 - 1, 1 - Y1 / ov_ch * 2);
    glUniform4f(ov_u_k, ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
    glBindTexture(GL_TEXTURE_2D, t);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
static float ov_ico(uint32_t code, float x, float y, float h, float a = 1.0f) {   // drawn width
    const OvTex* t = ov_icon(code);
    if (!t || !t->h) return 0;
    float w = h * t->w / t->h;
    ov_quad(t->t, x, y, w, h, 0xFFFFFF, a);
    return w;
}
static float ov_ico_w(uint32_t code, float h) { const OvTex* t = ov_icon(code); return t && t->h ? h * t->w / t->h : 0; }
static void ov_spr(int i, float x, float y, int w, int h, float a = 1.0f) { ov_quad(ov_sprite(i, w, h).t, x, y, (float)w, (float)h, 0xFFFFFF, a); }
static int ov_q() { int q = (int)ceilf(ov_fs); return q < 1 ? 1 : q > 4 ? 4 : q; }
static void ov_txt(int style, float size, const std::string& s, float x, float y, int w, int h, int align, uint32_t rgb, float a = 1.0f) {
    const OvTex* t = ov_text(style, size, s, w, h, ov_q(), align);
    if (t) ov_quad(t->t, x, y, (float)w, (float)h, rgb, a);
}

// layout in game pixels (480 x 272)
static const int OV_PX = 40, OV_PY = 8, OV_PW = 400, OV_PH = 256, OV_TY = OV_PY + 26, OV_TH = 13, OV_SY = OV_PY + 43, OV_SH = 12, OV_RH = 15, OV_RS = 17, OV_HS = 13;
static const int OV_BX = OV_PX + OV_PW - 128, OV_BY = OV_PY + OV_PH - 21, OV_BW = 116, OV_BH = 15, OV_DY = OV_BY - 27, OV_DH = 24, OV_SIDE = 26;
static const float OV_TS = 0.62f, OV_DS = 0.55f;   // text size of rows and tabs, of the description (1 = the font's game size)
static void ov_tab_rect(int i, float* x, float* w) { float tw = (OV_PW - 2 * OV_SIDE - 4 * 2) / (float)OV_NTABS; *x = OV_PX + OV_SIDE + i * (tw + 2); *w = tw; }
static int ov_nsubs() { return ov_tab == OV_TAB_GFX ? 6 : ov_tab == OV_TAB_CTL ? 5 : ov_tab == 4 ? 2 : 0; }
static const OvTabName& ov_subname(int i) { return ov_tab == OV_TAB_GFX ? OV_SUBS[i] : ov_tab == 4 ? OV_MSUBS[i] : OV_CSUBS[i]; }
static void ov_sub_rect(int i, float* x, float* w) { float tw = (OV_PW - 2 * OV_SIDE) / (float)std::max(1, ov_nsubs()); *x = OV_PX + OV_SIDE + i * tw; *w = tw; }
static int ov_list_top() { return ov_nsubs() ? OV_SY + OV_SH + 6 : OV_TY + OV_TH + 6; }
static bool ov_mods_page();
static int ov_list_bottom() { return ov_mods_page() ? OV_BY - 20 : OV_DY - 5; }

static std::vector<std::string> ov_names(std::initializer_list<std::pair<const char*, const char*>> l) {
    std::vector<std::string> v;
    for (auto& p : l) v.push_back(ov_s(p.first, p.second));
    return v;
}
int psp_cr_pak();
static std::vector<std::string> ov_onoff() { return ov_names({{"Off", "No"}, {"On", "Sí"}}); }
static OvRow ov_head(const char* en, const char* es) { OvRow r; r.kind = RK_HEAD; r.en = en; r.es = es; return r; }
static OvRow ov_val(const char* en, const char* es, std::function<int()> get, std::function<void(int)> set, std::vector<std::string> names, bool restart = false, bool on = true) {
    OvRow r; r.en = en; r.es = es; r.get = get; r.set = set; r.names = std::move(names); r.restart = restart; r.on = on;
    return r;
}
static OvRow ov_gw(const char* en, const char* es, int* p, std::vector<std::string> names, bool restart = false, bool on = true) {
    return ov_val(en, es, [p] { return *p; }, [p](int v) { *p = v; gw_save(); }, std::move(names), restart, on);
}
// controls page: action names and descriptions (key = the action id)
struct OvActTxt { const char* id; const char* en; const char* es; const char* den; const char* des; };
static const OvActTxt OV_ACTS[] = {
    {"menu_up", "Navigate up", "Navegar arriba", "Moves the cursor up in the menus.", "Mueve el cursor hacia arriba en los menús."},
    {"menu_down", "Navigate down", "Navegar abajo", "Moves the cursor down in the menus.", "Mueve el cursor hacia abajo en los menús."},
    {"menu_left", "Navigate left", "Navegar a la izquierda", "Moves the cursor left or changes a value.", "Mueve el cursor a la izquierda o cambia un valor."},
    {"menu_right", "Navigate right", "Navegar a la derecha", "Moves the cursor right or changes a value.", "Mueve el cursor a la derecha o cambia un valor."},
    {"menu_accept", "Accept", "Aceptar", "Confirms the selected option.", "Confirma la opción seleccionada."},
    {"menu_accept_start", "Accept / Start", "Aceptar / Start", "Accepts and also works as Start (title screen, start a battle).", "Acepta y además funciona como Start (pantalla de título, iniciar un combate)."},
    {"menu_start", "Start", "Start", "Start button of the menus.", "Botón Start de los menús."},
    {"menu_back", "Back", "Volver", "Returns to the previous screen.", "Vuelve a la pantalla anterior."},
    {"menu_random", "Random", "Aleatorio", "Random choice in the select screens. Removes a binding in this page.", "Elección aleatoria en las pantallas de selección. En esta página quita una asignación."},
    {"menu_page_left", "Previous page", "Página anterior", "Previous page or tab.", "Página o pestaña anterior."},
    {"menu_page_right", "Next page", "Página siguiente", "Next page or tab.", "Página o pestaña siguiente."},
    {"menu_switch", "Switch / Details", "Cambiar / Detalles", "Switches the view or shows details. Adds a binding in this page.", "Cambia la vista o muestra detalles. En esta página agrega una asignación."},
    {"battle_up", "Move forward", "Avanzar", "Moves the fighter towards the opponent.", "Mueve al luchador hacia el oponente."},
    {"battle_down", "Move back", "Retroceder", "Moves the fighter away from the opponent.", "Aleja al luchador del oponente."},
    {"battle_left", "Move left", "Moverse a la izquierda", "Moves the fighter around the opponent to the left.", "Mueve al luchador alrededor del oponente hacia la izquierda."},
    {"battle_right", "Move right", "Moverse a la derecha", "Moves the fighter around the opponent to the right.", "Mueve al luchador alrededor del oponente hacia la derecha."},
    {"battle_hit", "Hit", "Golpe", "Melee attacks.", "Ataques cuerpo a cuerpo."},
    {"battle_blast", "Ki blast", "Ráfaga de ki", "Ki blasts. Hold to charge them.", "Ráfagas de ki. Mantén para cargarlas."},
    {"battle_guard", "Guard", "Defensa", "Blocks the attacks.", "Bloquea los ataques."},
    {"battle_dash", "Dash", "Carrera", "Moves or flies faster.", "Vuela o se desplaza más rápido."},
    {"battle_charge", "Charge ki", "Cargar ki", "Hold to charge ki. Also opens the special attacks.", "Mantén para cargar ki. También da acceso a los ataques especiales."},
    {"battle_target", "Change target", "Cambiar objetivo", "Locks on another opponent.", "Fija la mira en otro oponente."},
    {"battle_rise", "Move up", "Subir", "Rises in the air.", "Sube en el aire."},
    {"battle_fall", "Move down", "Bajar", "Descends in the air.", "Baja en el aire."},
    {"battle_rise_fast", "Move up fast", "Subir rápido", "Double tap of Move up.", "Doble pulsación de Subir."},
    {"battle_fall_fast", "Move down fast", "Bajar rápido", "Double tap of Move down.", "Doble pulsación de Bajar."},
    {"battle_grab", "Grab", "Agarre", "Grabs the opponent.", "Agarra al oponente."},
    {"battle_emote", "Emote", "Burla", "Taunt animation.", "Animación de burla."},
    {"battle_transform", "Transform", "Transformación", "Transforms the fighter or opens the transformation wheel.", "Transforma al luchador o abre la rueda de transformación."},
    {"battle_fusion", "Fusion", "Fusión", "Fusion with the partner.", "Fusión con el compañero."},
    {"battle_pause", "Pause", "Pausa", "Pauses the battle.", "Pausa el combate."},
    {"battle_partner_left", "Partner mode left", "Modo del compañero (izquierda)", "Changes how the partner fights.", "Cambia la forma de pelear del compañero."},
    {"battle_partner_right", "Partner mode right", "Modo del compañero (derecha)", "Changes how the partner fights.", "Cambia la forma de pelear del compañero."},
    {"special_skill1", "Skill 1", "Habilidad 1", "First special skill of the fighter.", "Primera habilidad especial del luchador."},
    {"special_skill2", "Skill 2", "Habilidad 2", "Second special skill of the fighter.", "Segunda habilidad especial del luchador."},
    {"special_dexplosion", "Double explosion", "Explosión doble", "Sparking mode for the player and the partner.", "Modo sparking para el jugador y el compañero."},
    {"special_attack1", "Special attack 1", "Ataque especial 1", "First special attack of the fighter.", "Primer ataque especial del luchador."},
    {"special_attack2", "Special attack 2", "Ataque especial 2", "Second special attack of the fighter.", "Segundo ataque especial del luchador."},
    {"special_attack3", "Ultimate attack", "Ataque definitivo", "Ultimate attack of the fighter.", "Ataque definitivo del luchador."},
    {"special_fusion1", "Fusion 1", "Fusión 1", "First fusion with the partner.", "Primera fusión con el compañero."},
    {"special_fusion2", "Fusion 2", "Fusión 2", "Second fusion with the partner.", "Segunda fusión con el compañero."},
    {"special_fusion3", "Fusion 3", "Fusión 3", "Third fusion with the partner.", "Tercera fusión con el compañero."},
    {"special_transform1", "Transformation 1", "Transformación 1", "First transformation of the fighter.", "Primera transformación del luchador."},
    {"special_transform2", "Transformation 2", "Transformación 2", "Second transformation of the fighter.", "Segunda transformación del luchador."},
    {"special_transform3", "Transformation 3", "Transformación 3", "Third transformation of the fighter.", "Tercera transformación del luchador."},
    {"special_detransform", "Revert transformation", "Deshacer transformación", "Returns to the base form.", "Vuelve a la forma base."},
    {"camera_left", "BT3 camera left", "Cámara BT3 a la izquierda", "Turns the BT3 camera to the left.", "Gira la cámara BT3 a la izquierda."},
    {"camera_right", "BT3 camera right", "Cámara BT3 a la derecha", "Turns the BT3 camera to the right.", "Gira la cámara BT3 a la derecha."},
    {"camera_distance", "BT3 camera distance", "Distancia de la cámara BT3", "Changes the distance of the BT3 camera.", "Cambia la distancia de la cámara BT3."},
    {"story_up", "Move forward", "Avanzar", "Moves the character on the map.", "Mueve al personaje en el mapa."},
    {"story_down", "Move back", "Retroceder", "Moves the character on the map.", "Mueve al personaje en el mapa."},
    {"story_left", "Move left", "Moverse a la izquierda", "Moves the character on the map.", "Mueve al personaje en el mapa."},
    {"story_right", "Move right", "Moverse a la derecha", "Moves the character on the map.", "Mueve al personaje en el mapa."},
    {"story_concentration", "Concentration", "Concentración", "Concentration of the story mode.", "Concentración del modo historia."},
    {"story_ability", "Ability", "Habilidad", "Uses the ability of the character on the map.", "Usa la habilidad del personaje en el mapa."},
    {"story_fly", "Fly fast", "Volar rápido", "Flies faster on the map.", "Vuela más rápido en el mapa."},
    {"story_next", "Next dialogue / Accept", "Siguiente diálogo / Aceptar", "Advances the dialogues and accepts.", "Avanza los diálogos y acepta."},
    {"story_skip", "Skip dialogues", "Saltar diálogos", "Skips the dialogues.", "Salta los diálogos."},
    {"story_map", "Map / previous page", "Mapa / página anterior", "Opens the map or goes to the previous page.", "Abre el mapa o va a la página anterior."},
    {"story_back", "Back", "Volver", "Returns to the previous screen.", "Vuelve a la pantalla anterior."},
    {"story_menu_up", "Menu up", "Subir en el menú", "Moves up in the story menus.", "Sube en los menús del modo historia."},
    {"story_menu_down", "Menu down", "Bajar en el menú", "Moves down in the story menus.", "Baja en los menús del modo historia."},
    {"story_partner_left", "Partner mode left", "Modo del compañero (izquierda)", "Changes how the partner fights.", "Cambia la forma de pelear del compañero."},
    {"story_partner_right", "Partner mode right", "Modo del compañero (derecha)", "Changes how the partner fights.", "Cambia la forma de pelear del compañero."},
    {"story_page_right", "Next page", "Página siguiente", "Goes to the next page.", "Va a la página siguiente."},
};
static const OvActTxt* ov_acttxt(int a) {
    for (const OvActTxt& t : OV_ACTS) if (!strcmp(t.id, ACT[a].id)) return &t;
    return nullptr;
}

// what a binding row shows: codes and separators; false = derived from other actions (not edited here)
enum : uint32_t { OV_PLUS = 0xFF000001u, OV_X2, OV_OR, OV_NA, OV_EMPTY };
static uint32_t ov_fb(int a, bool pad) { const uint32_t* l = binds_of(a, pad); for (int k = 0; k < N_BINDS; k++) if (l[k]) return l[k]; return 0; }
static bool ov_seq(int a, bool pad, std::vector<uint32_t>& q) {
    const Action& A = ACT[a];
    q.clear();
    auto one = [&](int b) { uint32_t c = ov_fb(b, pad); q.push_back(c ? c : OV_EMPTY); };
    if (pad && a >= A_S_SK1 && a <= A_S_DETR) {
        int m = a <= A_S_DEX ? A_B_GUARD : a <= A_S_AT3 ? A_B_BLAST : a <= A_S_FU3 ? A_B_FUSION : -1;
        if (m >= 0) {
            one(A_B_CHARGE);
            if (A.dir) { q.push_back(OV_PLUS); q.push_back(C_PAD(P_RSU + A.dir - 1)); }
            q.push_back(OV_PLUS); one(m);
        } else {
            one(A_B_TRANSF);
            if (A.dir) { q.push_back(OV_PLUS); q.push_back(C_PAD(P_LSU + A.dir - 1)); }
        }
        return false;
    }
    if (pad && a == A_B_GRAB) { one(A_B_DASH); q.push_back(OV_X2); return false; }
    int kind = pad ? A.k_pad : A.k_kb, from = pad ? A.from_pad : A.from_kb, mod = pad ? A.mod_pad : A.mod_kb;
    if (kind == K_NONE && from < 0) { q.push_back(OV_NA); return false; }
    if (from >= 0) { one(from); if (kind == K_DOUBLE) q.push_back(OV_X2); return false; }
    if (mod >= 0) { one(mod); q.push_back(OV_PLUS); }
    const uint32_t* l = binds_of(a, pad);
    bool any = false;
    for (int k = 0; k < N_BINDS; k++) {
        if (!l[k]) continue;
        if (any) q.push_back(OV_OR);
        q.push_back(l[k]);
        any = true;
    }
    if (!any) q.push_back(OV_EMPTY);
    return true;
}

// capture of a key, mouse button or controller button for one action of the device in use
static int ov_cap = -1;
static bool ov_cap_pad = false, ov_cap_add = false;
static unsigned ov_cap_until = 0;
static std::vector<uint32_t> ov_cap_cand;
static std::vector<float> ov_cap_base;
static bool ov_editable(int a, bool pad) { return pad ? pad_editable(a) : kb_editable(a); }
static void ov_cfg_done(int a, bool pad) {
    static const int KEEP[6] = {A_M_UP, A_M_DOWN, A_M_LEFT, A_M_RIGHT, A_M_ACCEPT, A_M_BACK};   // the menus can always be navigated
    for (int b : KEEP) {
        uint32_t* l = pad ? cfg.pad[b] : cfg.kb[b];
        bool any = false;
        for (int k = 0; k < N_BINDS; k++) if (l[k]) any = true;
        if (!any) { memcpy(l, pad ? ACT[b].pad_def : ACT[b].kb_def, sizeof(uint32_t) * N_BINDS); printf("ajustes: control %s sin asignar, vuelve al predeterminado\n", ACT[b].id); }
    }
    cfg_save();
    if (a >= 0) {
        const uint32_t* l = pad ? cfg.pad[a] : cfg.kb[a];
        printf("ajustes: control %s (%s):", ACT[a].id, pad ? "mando" : "teclado");
        for (int k = 0; k < N_BINDS; k++) if (l[k]) printf(" %08X", l[k]);
        printf("\n");
    }
}
static OvRow ov_bindrow(int a) {
    OvRow r; r.kind = RK_BIND; r.action = a; r.en = ""; r.es = "";
    for (const OvActTxt& t : OV_ACTS) if (!strcmp(t.id, ACT[a].id)) { r.en = t.en; r.es = t.es; }
    return r;
}
static OvRow ov_exp(OvRow r) { r.tag2 = 1; return r; }
static OvRow ov_norec(OvRow r) { r.tag2 = 2; return r; }
static OvRow ov_destr(OvRow r) { r.tag2 = 3; return r; }
static OvRow ov_act(const char* en, const char* es, std::string tag, std::function<void()> act, bool on = true) {
    OvRow r; r.kind = RK_ACTION; r.en = en; r.es = es; r.tag = std::move(tag); r.act = act; r.on = on;
    return r;
}

static void ov_gw_ready() {
    if (!gw_loaded) { gw_read(); gw_start = gw_v; gw_loaded = true; }
}
static void ov_close();
static void ov_quit() { ov_confirm = 2; ov_yes = 0; }

// mods page: installed mods (order = priority, on/off, image and details, options)
static std::string ov_mroot;
static std::vector<ModInfo> ov_mods;
static bool ov_mods_read = false;
static int ov_mmove = -1, ov_mdrag = -1, ov_msel = 0, ov_opts = -1, ov_opt = 0, ov_rm = -1;
static std::string ov_note;
static unsigned ov_note_t = 0;
static void ov_notice(const std::string& t) { ov_note = t; ov_note_t = GetTickCount() + 5000; }
static bool ov_mods_page() { return ov_tab == 4 && ov_sub == 0; }
static int ov_rh() { return ov_mods_page() ? 10 : OV_RH; }
static int ov_rs() { return ov_mods_page() ? 11 : OV_RS; }
static const int OV_MLW = 196, OV_MDX = OV_PX + 10 + OV_MLW + 10, OV_MDW = OV_PX + OV_PW - 12 - OV_MDX;   // list width, details column
static float ov_list_w() { return ov_mods_page() ? (float)OV_MLW : (float)(OV_PW - 20); }
static void ov_mods_load() {
    if (ov_mods_read) return;
    if (mods_root_global.empty()) { const char* e = getenv("TTT_ASSETS"); mods_root_global = mods_root_of(e && *e && strcmp(e, "0") ? e : "../../assets"); }
    ov_mroot = mods_root_global;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(ov_mroot), ec);
    ov_mods = mods_list(ov_mroot);
    ov_mods_read = true;
    if (ov_msel >= (int)ov_mods.size()) ov_msel = 0;
}
static void ov_mod_toggle(int k) {
    if (k < 0 || k >= (int)ov_mods.size()) return;
    ov_mods[k].enabled = !ov_mods[k].enabled;
    mods_save(ov_mroot, ov_mods);
    ov_mods_changed = true;
    printf("ajustes: mod %s %s\n", ov_mods[k].id.c_str(), ov_mods[k].enabled ? "activado" : "desactivado");
}
static void ov_mod_swap(int i, int j) {
    if (i < 0 || j < 0 || i >= (int)ov_mods.size() || j >= (int)ov_mods.size() || i == j) return;
    std::swap(ov_mods[i], ov_mods[j]);
    mods_save(ov_mroot, ov_mods);
    ov_mods_changed = true;
    printf("ajustes: mod %s pasa a la posicion %d\n", ov_mods[j].id.c_str(), j + 1);
}
static std::string ov_tags(const ModInfo& m) {
    std::string t = mod_tags_en(mw_field(m, "etiquetas"));
    if (!ov_es()) return t;
    static const char* ES[MOD_NTAGS] = {"Texturas", "Audio", "Personajes", "Roster", "Mapas", "Menús", "Ataques", "Modelos", "Modo historia", "Lógica", "Otros"};
    std::string o, w;
    auto flush = [&]() {
        size_t a = w.find_first_not_of(' ');
        std::string x = a == std::string::npos ? "" : w.substr(a);
        for (int i = 0; i < MOD_NTAGS; i++) if (x == mw_u8(MOD_TAGS[i])) x = ES[i];
        if (!x.empty()) o += (o.empty() ? "" : ", ") + x;
        w.clear();
    };
    for (char c : t) { if (c == ',') flush(); else w += c; }
    flush();
    return o;
}
static const OvTex* ov_mod_tex(const ModInfo& m) {
    std::string p = ov_mroot + "/" + m.id + "/imagen.png";
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(std::filesystem::u8path(p), ec);
    std::string k = "m|" + p + "|" + (ec ? std::string("0") : std::to_string((long long)ft.time_since_epoch().count()));
    auto it = ov_cache.find(k);
    if (it != ov_cache.end()) return it->second.t ? &it->second : nullptr;
    OvImg im;
    std::vector<uint8_t> f;
    if (FILE* fp = mods_fopen(p, "rb")) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        f.resize(n > 0 ? (size_t)n : 0);
        if (n <= 0 || fread(f.data(), 1, f.size(), fp) != f.size()) f.clear();
        fclose(fp);
    }
    std::string err;
    if (f.empty() || !png_decode(f, &im.w, &im.h, im.px, &err) || !im.w || !im.h) { ov_cache[k] = OvTex(); return nullptr; }
    return &(ov_cache[k] = ov_upload(im));
}
static void ov_mod_install(const std::string& fpath) {
    std::string id, err, out;
    if (!mod_install(ov_mroot, fpath, &id, &err)) { ov_notice(ov_s("Could not install: ", "No se pudo instalar: ") + err); printf("ajustes: no se pudo instalar %s: %s\n", fpath.c_str(), err.c_str()); return; }
    std::string dir = ov_mroot + "/" + id;
    bool failed = mw_closed(dir) && !pm_run(L"--mod " + pm_quote(dir), out);
    ov_mods = mods_list(ov_mroot);
    ov_mods_changed = true;
    for (size_t k = 0; k < ov_mods.size(); k++) if (ov_mods[k].id == id) { ov_msel = (int)k; ov_row = (int)k; }
    ov_notice(failed ? ov_s("Installed, but its .pak files stayed closed", "Instalado, pero sus .pak quedaron cerrados") : ov_s("Mod installed", "Mod instalado"));
    printf("ajustes: mod %s instalado%s\n", id.c_str(), failed ? " (pak cerrados)" : "");
}
static void ov_mod_pick() {
    wchar_t file_name[4096] = L"";
    OPENFILENAMEW of; memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of); of.hwndOwner = ventana;
    of.lpstrFilter = L"Mods (*.mkpak)\0*.mkpak\0All files\0*.*\0";
    of.lpstrFile = file_name; of.nMaxFile = 4096;
    of.lpstrTitle = ov_es() ? L"Instalar mod" : L"Install mod";
    of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&of)) ov_mod_install(mw_u8(file_name));
}
static void ov_mod_export(int k) {
    if (k < 0 || k >= (int)ov_mods.size()) return;
    std::string id = ov_mods[k].id, err;
    wchar_t file_name[4096];
    wcsncpy(file_name, mw_w(id + ".mkpak").c_str(), 4095); file_name[4095] = 0;
    OPENFILENAMEW of; memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of); of.hwndOwner = ventana;
    of.lpstrFilter = L"Mod (*.mkpak)\0*.mkpak\0";
    of.lpstrFile = file_name; of.nMaxFile = 4096; of.lpstrTitle = ov_es() ? L"Exportar mod" : L"Export mod"; of.lpstrDefExt = L"mkpak";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&of)) return;
    size_t n = 0;
    if (!mw_pack(ov_mroot + "/" + id, mw_u8(file_name), &n, nullptr, &err)) {
        ov_notice(err == "empty" ? ov_s("The mod folder is empty", "La carpeta del mod está vacía") : err == "big" ? ov_s("Too big for a .mkpak (2 GB or 65535 files)", "Demasiado grande para un .mkpak (2 GB o 65535 archivos)") : ov_s("Could not write the file", "No se pudo escribir el archivo"));
        return;
    }
    ov_notice(ov_s("Exported: ", "Exportado: ") + std::to_string(n) + ov_s(" files", " archivos"));
    printf("ajustes: mod %s exportado (%zu archivos)\n", id.c_str(), n);
}
static std::function<void()> ov_pend;   // runs when the menu opens (links, dropped files)
static void ov_select_mod(const std::string& id, bool notice) {
    ov_mods = mods_list(ov_mroot);
    for (size_t k = 0; k < ov_mods.size(); k++) if (ov_mods[k].id == id) { ov_msel = (int)k; ov_row = (int)k; }
    if (notice) ov_notice(ov_s("Mod installed, it's already active", "Mod instalado, ya está activo"));
}
static bool ov_link(const std::string& s) {
    if (!usar_gl) return false;
    if (!s.compare(0, 10, "installed:")) { std::string id = s.substr(10); ov_pend = [id] { ov_select_mod(id, true); }; return true; }
    if (!s.compare(0, 5, "open:")) {
        std::string id = s.substr(5);
        ov_pend = [id] { std::string out; bool ok = pm_run(L"--mod " + pm_quote(ov_mroot + "/" + id), out); ov_mods_changed = true; ov_select_mod(id, false); ov_notice(ok ? ov_s("Mod installed", "Mod instalado") : ov_s("Installed, but its .pak files stayed closed", "Instalado, pero sus .pak quedaron cerrados")); };
        return true;
    }
    if (s.size() > 6 && !_stricmp(s.c_str() + s.size() - 6, ".mkpak")) { ov_pend = [s] { ov_mod_install(s); }; return true; }
    std::string lid = link_lib_id(s);
    if (!lid.empty()) { ov_pend = [lid] { ol_open_id(lid); }; return true; }
    return false;
}
static void ov_library(const std::string& publish) { ol_publish(publish); }
static const char* OV_MOPT[4][2] = {{"Open folder", "Abrir carpeta"}, {"Export .mkpak", "Exportar .mkpak"}, {"Publish", "Publicar"}, {"Remove", "Quitar"}};
static void ov_mopt_do(int i) {
    int k = ov_opts;
    ov_opts = -1;
    if (k < 0 || k >= (int)ov_mods.size()) return;
    if (i == 0) ShellExecuteW(ventana, L"open", mw_w(ov_mroot + "/" + ov_mods[k].id).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (i == 1) ov_mod_export(k);
    else if (i == 2) ov_library(ov_mods[k].id);
    else if (i == 3) { ov_rm = k; ov_confirm = 6; ov_yes = 0; }
}

static void ov_tool(int act, void (*f)()) {   // the modding tools keep their own windows
    printf("ajustes: herramienta %d\n", act);
    ui_activity(act);
    f();
    ui_activity(0);
    ov_mods_read = false;
    if (restart_requested) { ov_open = false; ov_confirm = 0; ov_hold = true; }
}

static std::vector<OvRow> ov_page() {
    ov_gw_ready();
    std::vector<OvRow> r;
    GwValues& g = gw_v;
    auto lv = [](std::initializer_list<std::pair<const char*, const char*>> l) { return ov_names(l); };
    auto OFF = []() { return ov_onoff(); };
    if (ov_tab == 0) {
        r.push_back(ov_act("Resume", "Volver al juego", "", [] { ov_close(); }));
        r.push_back(ov_val("Language", "Idioma", [] { return language == 3 ? 1 : 0; }, [](int v) { language = v ? 3 : 1; settings_save(); printf("settings: idioma cambiado a %d (al reiniciar)\n", language); }, {"English", "Español"}, true));
        r.push_back(ov_gw("Discord", "Discord", &g.discord, OFF()));
        r.push_back(ov_gw("Skip intro credits", "Saltar créditos iniciales", &g.skipcr, OFF()));
        r.push_back(ov_act("Quit game", "Cerrar el juego", "", [] { ov_quit(); }));
    } else if (ov_tab == 2) {
        r.push_back(ov_val("Sound", "Sonido", [] { return snd_3d < 0 ? 0 : snd_3d > 2 ? 2 : snd_3d; }, [](int v) { snd_3d = v; settings_save(); },
                           lv({{"Original stereo", "Estéreo original"}, {"3D stereo", "Estéreo 3D"}, {"Wide 3D stereo", "Estéreo 3D amplio"}})));
        r.push_back(ov_gw("Underwater sound", "Sonido bajo el agua", &g.sndw, OFF()));
        if (psp_cr_pak() >= 0) r.push_back(ov_gw("Voice on character select", "Voz al elegir personaje", &g.selv, OFF()));
    } else if (ov_tab == OV_TAB_CTL && ov_sub == 4) {
        r.push_back(ov_val("Rumble intensity", "Intensidad de la vibración", [] { return std::min(4, (cfg.rumble + 12) / 25); }, [](int v) { cfg.rumble = v * 25; rumble_level = cfg.rumble; cfg_save(); },
                           lv({{"Off", "No"}, {"25 %", "25 %"}, {"50 %", "50 %"}, {"75 %", "75 %"}, {"100 %", "100 %"}})));
        r.push_back(ov_act("Test rumble", "Probar vibración", "", [] { hid_retry_ms = 0; input_mode = 1; rumble_request(1.0f, 0.0f, 250); rumble_request(0.0f, 1.0f, 250); }, cfg.rumble > 0));
    } else if (ov_tab == OV_TAB_CTL) {
        static const int TABS[4] = {TAB_BATTLE, TAB_SPECIAL, TAB_MENU, TAB_STORY};
        int tab = TABS[ov_sub];
        bool pad = ov_pad();
        r.push_back(!pad ? ov_head("Keyboard and mouse", "Teclado y ratón") : pad_family == 2 ? ov_head("Controller (PlayStation)", "Mando (PlayStation)")
                    : pad_family == 3 ? ov_head("Controller (generic)", "Mando (genérico)") : ov_head("Controller (Xbox)", "Mando (Xbox)"));
        std::vector<uint32_t> q;
        for (int a = 0; a < N_ACTIONS; a++) {
            if (ACT[a].tab != tab || a == A_B_RISEF || a == A_B_FALLF || a == A_M_ACCSTART) continue;
            if (!ov_seq(a, pad, q) && q[0] == OV_NA) continue;   // nothing for this device
            r.push_back(ov_bindrow(a));
        }
        r.push_back(ov_act("Restore defaults", "Restaurar predeterminados", "", [tab, pad] {
            for (int a = 0; a < N_ACTIONS; a++) if (ACT[a].tab == tab) memcpy(pad ? cfg.pad[a] : cfg.kb[a], pad ? ACT[a].pad_def : ACT[a].kb_def, sizeof(uint32_t) * N_BINDS);
            printf("ajustes: controles predeterminados (seccion %d, %s)\n", tab, pad ? "mando" : "teclado");
            ov_cfg_done(-1, pad);
        }));
    } else if (ov_tab == 4 && ov_sub == 0) {
        ov_mods_load();
        if (ov_mods.empty()) r.push_back(ov_head("No mods installed", "No hay mods instalados"));
        for (size_t k = 0; k < ov_mods.size(); k++) { OvRow m; m.kind = RK_MOD; m.action = (int)k; m.on = !ov_net; r.push_back(m); }
        r.push_back(ov_act("+ Install .mkpak...", "+ Instalar .mkpak...", "", [] { ov_mod_pick(); }, !ov_net));
    } else if (ov_tab == 4) {
    } else if (ov_tab == 5) {
        r.push_back(ov_head("Mod tools", "Herramientas de modding"));
        r.push_back(ov_act("Create mod...", "Crear mod...", "", [] { ov_tool(3, createmod_window); }, !ov_net));
        r.push_back(ov_destr(ov_act("Characters and maps...", "Personajes y mapas...", "", [] { ov_tool(4, characters_window); }, !ov_net)));
        r.push_back(ov_act("Material editor...", "Editor de materiales...", "", [] { ov_tool(6, tpal_window); }, !ov_net));
        r.push_back(ov_act("Render editor...", "Editor de renders...", "", [] { ov_tool(5, render_editor); }, !ov_net));
        r.push_back(ov_act("Map renders...", "Renders de mapas...", "", [] { ov_tool(8, map_chooser); }, !ov_net));
    } else if (ov_sub == 0) {
        std::vector<std::string> sizes;
        for (int i = 0; i < 8; i++) { std::string t = GW_SIZE_C[i]; size_t x = t.find('x'); sizes.push_back(t.substr(0, x) + " x " + t.substr(x + 1)); }
        if (!gw_size_other.empty()) sizes.push_back(gw_size_other);
        std::vector<std::string> scales = {ov_s("Auto", "Auto")};
        for (int i = 1; i <= 10; i++) scales.push_back("x" + std::to_string(i) + " (" + std::to_string(480 * i) + " x " + std::to_string(272 * i) + ")");
        r.push_back(ov_head("Window", "Ventana"));
        r.push_back(ov_gw("Window size", "Tamaño de ventana", &g.size, sizes, true));
        r.push_back(ov_val("Image fit", "Ajuste de imagen", [] { return modo_vista(); }, [](int v) { vista_var = v; limpiar_fondo = true; settings_save(); },
                           lv({{"Black bars", "Bandas negras"}, {"Stretch", "Estirar"}, {"Widen 3D view", "Ampliar vista 3D"}})));
        r.push_back(ov_val("Fullscreen", "Pantalla completa", [] { return completa ? 1 : 0; }, [](int v) { if ((v != 0) != completa) fullscreen_switch(); }, OFF()));
        r.push_back(ov_gw("Hide cursor", "Ocultar cursor", &g.cursor, lv({{"Off", "No"}, {"1 s", "1 s"}, {"3 s", "3 s"}, {"5 s", "5 s"}, {"10 s", "10 s"}})));
        r.push_back(ov_head("Image", "Imagen"));
        r.push_back(ov_gw("Internal resolution", "Resolución interna", &g.scale, scales, true));
        r.push_back(ov_gw("Antialiasing", "Antialiasing", &g.msaa, lv({{"Off", "No"}, {"FXAA", "FXAA"}, {"MSAA 2x", "MSAA 2x"}, {"MSAA 4x", "MSAA 4x"}, {"MSAA 8x", "MSAA 8x"}, {"MSAA 4x + FXAA", "MSAA 4x + FXAA"}}), true, g.gpu != 0));
        r.push_back(ov_gw("Texture filter", "Filtro de texturas", &g.texf, lv({{"Original", "Original"}, {"xBR 2x", "xBR 2x"}, {"xBR 4x", "xBR 4x"}}), true, g.gpu != 0));
        r.push_back(ov_val("Screen filter", "Filtro de pantalla", [] { return gw_v.filter; }, [](int v) { gw_v.filter = v; filtro_var = v; gw_save(); },
                           lv({{"Sharp pixels", "Píxeles nítidos"}, {"Smooth", "Suave"}, {"High quality", "Alta calidad"}})));
        r.push_back(ov_val("Sharpness", "Nitidez", [] { return gw_v.sharpness; }, [](int v) { gw_v.sharpness = v; nitidez_var = GW_SHARP[v]; gw_save(); },
                           lv({{"Off", "No"}, {"Low", "Baja"}, {"Medium", "Media"}, {"High", "Alta"}, {"Maximum", "Máxima"}})));
    } else if (ov_sub == 1) {
        auto lvl = lv({{"Very low", "Muy baja"}, {"Low", "Baja"}, {"Medium", "Media"}, {"High", "Alta"}, {"Very high", "Muy alta"}});
        r.push_back(ov_head("Light", "Luz"));
        r.push_back(ov_gw("Bloom", "Resplandor", &g.bloom, lv({{"Off", "No"}, {"Classic", "Clásico"}, {"Soft", "Suave"}})));
        r.push_back(ov_gw("Effects light", "Luz de los efectos", &g.vfxmap, lvl, false, g.bloom != 0));
        r.push_back(ov_gw("VFX light on Cel BT3", "Luz de VFX en Cel BT3", &g.bt3fx, OFF()));
        r.push_back(ov_gw("Storm flashes", "Destellos de tormenta", &g.bolts, OFF()));
        r.push_back(ov_head("Map effects", "Efectos de mapa"));
        r.push_back(ov_gw("Storm", "Tormenta", &g.storm, OFF()));
        r.push_back(ov_gw("Bubbles", "Burbujas", &g.bubbles, OFF()));
        r.push_back(ov_gw("Heat haze", "Calima", &g.haze, OFF()));
        r.push_back(ov_head("Scene", "Escena"));
        r.push_back(ov_gw("Atmosphere", "Atmósfera", &g.atm, lv({{"Off", "No"}, {"BT3", "BT3"}, {"Enhanced", "Mejorada"}})));
        r.push_back(ov_gw("Sun glare", "Reflejo del sol", &g.sun, lv({{"Off", "No"}, {"BT3", "BT3"}, {"Full", "Completo"}})));
        r.push_back(ov_gw("Motion blur", "Desenfoque de movimiento", &g.mblur, lv({{"Off", "No"}, {"Low", "Bajo"}, {"Medium", "Medio"}, {"High", "Alto"}})));
        r.push_back(ov_val("Water effects", "Efectos de agua", [] { int w = gw_v.water; return w == 3 ? 3 : w == 2 ? 1 : w == 1 ? 2 : 0; }, [](int v) { gw_v.water = GW_WATER_V[std::min(v, 3)]; gw_save(); },
                           lv({{"Off", "No"}, {"BT3", "BT3"}, {"Full", "Completo"}, {"Enhanced", "Mejorado"}})));
        r.push_back(ov_gw("Rain", "Lluvia", &g.rain, lv({{"BT3", "BT3"}, {"Enhanced", "Mejorada"}})));
        r.push_back(ov_gw("Wet surfaces", "Superficies mojadas", &g.wet, OFF()));
        r.push_back(ov_gw("Impact effects", "Efectos de impacto", &g.impact, lv({{"Off", "No"}, {"Shake", "Temblor"}, {"Lines + shake", "Líneas + temblor"}, {"Flash frames + shake", "Destellos + temblor"}})));
    } else if (ov_sub == 2) {
        r.push_back(ov_head("Shading", "Sombreado"));
        r.push_back(ov_gw("Cel shading", "Cel shading", &g.cel, lv({{"Off", "No"}, {"Cel shading", "Cel shading"}, {"Cel BT3", "Cel BT3"}, {"Mixed", "Mixto"}, {"Mixed BT3", "Mixto BT3"}})));
        r.push_back(ov_gw("Cel smoothing", "Suavizado del cel", &g.smooth, lv({{"Off", "No"}, {"Low", "Bajo"}, {"Medium", "Medio"}, {"High", "Alto"}, {"Maximum", "Máximo"}}), false, g.cel != 0));
        r.push_back(ov_gw("BT3 glow", "Brillo BT3", &g.celglow, OFF()));
        r.push_back(ov_gw("Soft shadows", "Sombras suaves", &g.soft, OFF()));
        r.push_back(ov_gw("Outline", "Contorno", &g.outline, lv({{"No outline", "Sin contorno"}, {"Thin", "Fino"}, {"Normal", "Normal"}, {"Thick", "Grueso"}}), true));
        r.push_back(ov_gw("Outline style", "Estilo del contorno", &g.olmode, lv({{"Original", "Original"}, {"Absolute", "Absoluto"}}), false, g.outline != 0));
        r.push_back(ov_head("Model", "Modelo"));
        r.push_back(ov_gw("Distance quality", "Calidad a distancia", &g.lod, OFF(), true));
        r.push_back(ov_gw("Colors from files", "Colores desde archivos", &g.dyncol, OFF(), true));
        r.push_back(ov_gw("Partner glow", "Brillo del compañero", &g.glow, OFF()));
        r.push_back(ov_head("Faces", "Caras"));
        r.push_back(ov_gw("Lip sync", "Sincronía labial", &g.lip, OFF()));
        r.push_back(ov_gw("Blinking", "Parpadeo", &g.blink, OFF()));
    } else if (ov_sub == 3) {
        std::vector<std::string> bars;
        static const char* BAR_ES[4] = {"Original", "Original (completa)", "BT3", "Sparking Zero"};
        for (int i = 0; i < gw_bars_n; i++) bars.push_back(ov_es() ? BAR_ES[gw_bars_v[i]] : (gw_bars_v[i] == 1 ? "Original (Full)" : BAR_ES[gw_bars_v[i]]));
        r.push_back(ov_head("HUD", "HUD"));
        r.push_back(ov_gw("Battle HUD", "HUD de combate", &g.hud, OFF()));
        r.push_back(ov_val("Life bars", "Barras de vida", [] { return gw_bars_sel(); }, [](int v) { if (v >= 0 && v < gw_bars_n) gw_v.bars = gw_bars_v[v]; gw_save(); }, bars));
        r.push_back(ov_gw("Life bar names", "Nombres en las barras", &g.names, OFF()));
        r.push_back(ov_gw("Bars behind fighters", "Barras detrás de los luchadores", &g.barsback, OFF()));
        r.push_back(ov_gw("Technique list (SZ)", "Lista de técnicas (SZ)", &g.charge, OFF()));
        r.push_back(ov_gw("Transformation wheel (SZ)", "Rueda de transformación (SZ)", &g.trwheel, OFF()));
        r.push_back(ov_head("Camera", "Cámara"));
        r.push_back(ov_exp(ov_gw("BT3 camera", "Cámara BT3", &g.bt3cam, OFF())));
        r.push_back(ov_gw("Original close-cam silhouette", "Silueta original en cámara cercana", &g.camsil, OFF()));
        r.push_back(ov_head("Gameplay", "Jugabilidad"));
        r.push_back(ov_gw("Mid-air floating", "Flotar en el aire", &g.floating, OFF()));
        r.push_back(ov_gw("Transform wait", "Espera de transformación", &g.trwait, lv({{"Off", "No"}, {"Until loaded", "Hasta cargar"}, {"Loaded + 1 s", "Cargado + 1 s"}, {"Loaded + 2 s", "Cargado + 2 s"}, {"Loaded + 3 s", "Cargado + 3 s"}})));
        r.push_back(ov_gw("Quick blast recovery (fix)", "Recuperación rápida de ráfaga (fix)", &g.blastend, OFF()));
        r.push_back(ov_exp(ov_gw("Two-phase charge (SZ)", "Carga en dos fases (SZ)", &g.chg2, OFF())));
    } else if (ov_sub == 4) {
        const char* q0 = ov_s("Regenerate", "Regenerar"), *q1 = ov_s("Queued", "En cola");
        r.push_back(ov_head("Generated renders", "Renders generados"));
        r.push_back(ov_gw("Renders from 3D", "Renders desde el 3D", &g.genr, OFF()));
        r.push_back(ov_gw("Quality", "Calidad", &g.genq, lv({{"Original", "Original"}, {"x2", "x2"}, {"x4", "x4"}}), false, g.genr != 0 || g.genm != 0));
        r.push_back(ov_act("Character renders", "Renders de personajes", ov_queued[0] ? q1 : q0, [] { char nb[16]; snprintf(nb, sizeof(nb), "%d", gw_env("TTT_GEN_RENDERS_GO", 0) + 1); _putenv_s("TTT_GEN_RENDERS_GO", nb); ov_queued[0] = true; }, g.genr != 0));
        r.push_back(ov_gw("Map renders from 3D", "Renders de mapas desde el 3D", &g.genm, OFF()));
        r.push_back(ov_act("Map renders", "Renders de mapas", ov_queued[1] ? q1 : q0, [] { char nb[16]; snprintf(nb, sizeof(nb), "%d", gw_env("TTT_GEN_MAP_RENDERS_GO", 0) + 1); _putenv_s("TTT_GEN_MAP_RENDERS_GO", nb); ov_queued[1] = true; }, g.genm != 0));
        r.push_back(ov_head("Select screens", "Pantallas de selección"));
        r.push_back(ov_gw("Character select style", "Estilo de la selección de personajes", &g.selstyle, lv({{"Original", "Original"}, {"BT3", "BT3"}})));
        r.push_back(ov_gw("3D model in select", "Modelo 3D en la selección", &g.sel3d, OFF(), true));
        if (g.sel3d) r.push_back(ov_gw("3D view entrance", "Entrada en la vista 3D", &g.selentry, lv({{"Fall", "Caída"}, {"Teleport", "Teletransporte"}})));
        r.push_back(ov_gw("3D map in select", "Mapa 3D en la selección", &g.map3d, OFF()));
        r.push_back(ov_gw("Names as text", "Nombres como texto", &g.textn, OFF()));
    } else {
        r.push_back(ov_head("Performance", "Rendimiento"));
        r.push_back(ov_gw("GPU rendering", "Dibujo por GPU", &g.gpu, OFF(), true));
        r.push_back(ov_norec(ov_gw("Vertical sync", "Sincronía vertical", &g.vsync, OFF(), true)));
        r.push_back(ov_gw("Frame rate", "Cuadros por segundo", &g.fps, lv({{"30 (original)", "30 (original)"}, {"60 (experimental)", "60 (experimental)"}}), true));
        r.push_back(ov_gw("Smooth motion", "Movimiento suave", &g.smotion, lv({{"Off", "No"}, {"On", "Sí"}, {"On (low latency)", "Sí (baja latencia)"}})));
        r.push_back(ov_gw("FPS counter", "Contador de FPS", &g.fpsc, lv({{"Off", "No"}, {"Top left", "Arriba izquierda"}, {"Top right", "Arriba derecha"}, {"Bottom left", "Abajo izquierda"}, {"Bottom right", "Abajo derecha"}})));
    }
    return r;
}

// short description of each row (key = the row's English name)
struct OvDesc { const char* key; const char* en; const char* es; };
static const OvDesc OV_DESC[] = {
    {"Resume", "Closes this menu and returns to the game.", "Cierra este menú y vuelve al juego."},
    {"Language", "Language of the game texts and of this menu.", "Idioma de los textos del juego y de este menú."},
    {"Discord", "Shows on your Discord profile what you are playing.", "Muestra en tu perfil de Discord lo que estás jugando."},
    {"Skip intro credits", "Skips the logos and credits shown when the game starts.", "Omite los logos y créditos que aparecen al iniciar el juego."},
    {"Quit game", "Closes the game.", "Cierra el juego."},
    {"Window size", "Size of the game window when it is not in fullscreen.", "Tamaño de la ventana del juego cuando no está en pantalla completa."},
    {"Image fit", "How the image fills the screen: black bars keep the original shape, stretch fills it and widen shows more of the 3D scene.", "Cómo llena la imagen la pantalla: bandas negras conserva la forma original, estirar la llena y ampliar muestra más de la escena 3D."},
    {"Fullscreen", "Switches between window and fullscreen.", "Alterna entre ventana y pantalla completa."},
    {"Hide cursor", "Hides the mouse cursor after it stays still for this time.", "Oculta el cursor del ratón cuando queda quieto durante este tiempo."},
    {"Internal resolution", "Resolution the game is drawn at. Higher is sharper but needs a stronger graphics card.", "Resolución a la que se dibuja el juego. Más alta se ve más nítida pero exige más a la tarjeta gráfica."},
    {"Antialiasing", "Smooths jagged edges. MSAA is cleaner, FXAA is lighter. Needs GPU rendering.", "Suaviza los bordes dentados. MSAA es más limpio, FXAA es más ligero. Requiere dibujo por GPU."},
    {"Texture filter", "Scales the textures with xBR so they look smoother. Needs GPU rendering.", "Escala las texturas con xBR para que se vean más suaves. Requiere dibujo por GPU."},
    {"Screen filter", "How the final image is scaled to the window.", "Cómo se escala la imagen final a la ventana."},
    {"Sharpness", "Adds sharpness to the final image.", "Agrega nitidez a la imagen final."},
    {"Bloom", "Glow around lights and bright effects.", "Resplandor alrededor de las luces y los efectos brillantes."},
    {"Effects light", "How much the effects light up the map and the fighters. Needs Bloom.", "Cuánto iluminan los efectos al mapa y a los luchadores. Requiere Resplandor."},
    {"VFX light on Cel BT3", "The effects also light the fighters when Cel BT3 is in use.", "Los efectos también iluminan a los luchadores cuando se usa Cel BT3."},
    {"Storm flashes", "Lightning flashes light up the scene on stormy maps.", "Los relámpagos iluminan la escena en los mapas con tormenta."},
    {"Storm", "Storm clouds and lightning on the maps that have them.", "Nubes de tormenta y relámpagos en los mapas que los tienen."},
    {"Bubbles", "Bubbles under the water like in BT3.", "Burbujas bajo el agua como en BT3."},
    {"Heat haze", "Heat distortion on hot maps.", "Distorsión por calor en los mapas calurosos."},
    {"Atmosphere", "Distance fog and sky colour like in BT3. Enhanced adds more depth.", "Niebla a distancia y color del cielo como en BT3. Mejorada agrega más profundidad."},
    {"Sun glare", "Glare when the camera looks at the sun.", "Reflejo cuando la cámara mira hacia el sol."},
    {"Motion blur", "Blur during fast camera movements.", "Desenfoque durante los movimientos rápidos de la cámara."},
    {"Water effects", "Reflections and waves on the water.", "Reflejos y olas en el agua."},
    {"Rain", "Style of the rain on rainy maps.", "Estilo de la lluvia en los mapas lluviosos."},
    {"Wet surfaces", "Floors and models look wet when it rains.", "El suelo y los modelos se ven mojados cuando llueve."},
    {"Impact effects", "Screen shake, lines or flashes on strong hits.", "Temblor de pantalla, líneas o destellos en los golpes fuertes."},
    {"Cel shading", "Anime style shading on the fighters. Mixed combines it with the original lighting.", "Sombreado estilo anime en los luchadores. Mixto lo combina con la iluminación original."},
    {"Cel smoothing", "Softens the edge between light and shadow of the cel shading.", "Suaviza el borde entre luz y sombra del cel shading."},
    {"BT3 glow", "Glow of Cel BT3 on the bright parts of the fighters.", "Brillo de Cel BT3 en las partes luminosas de los luchadores."},
    {"Soft shadows", "Shadows with soft edges instead of hard ones.", "Sombras con bordes suaves en lugar de duros."},
    {"Outline", "Black outline around the fighters and its thickness.", "Contorno negro alrededor de los luchadores y su grosor."},
    {"Outline style", "Original changes with the distance, absolute keeps the same thickness.", "Original cambia con la distancia, absoluto mantiene el mismo grosor."},
    {"Distance quality", "Keeps the full detail models when the fighters are far away.", "Mantiene los modelos con todo su detalle cuando los luchadores están lejos."},
    {"Colors from files", "The number of colors in the menu comes straight from the colors in the files, mods included.", "La cantidad de colores en el menú se define por la cantidad de colores en los archivos, mods incluidos."},
    {"Partner glow", "Original pulsing glow on the partner of your team.", "Brillo pulsante original sobre el compañero de tu equipo."},
    {"Lip sync", "Moves the mouth of the fighters with their voice.", "Mueve la boca de los luchadores con su voz."},
    {"Blinking", "The fighters blink from time to time.", "Los luchadores parpadean de vez en cuando."},
    {"Battle HUD", "Shows the life bars and the rest of the battle interface.", "Muestra las barras de vida y el resto de la interfaz de combate."},
    {"Life bars", "Style of the life bars.", "Estilo de las barras de vida."},
    {"Life bar names", "Shows the name of each fighter or player on the bars.", "Muestra el nombre de cada luchador o jugador en las barras."},
    {"Bars behind fighters", "Draws the life bars behind the fighters.", "Dibuja las barras de vida detrás de los luchadores."},
    {"Technique list (SZ)", "List of the techniques, based on Sparking Zero.", "Lista de técnicas, basada en Sparking Zero."},
    {"Transformation wheel (SZ)", "Wheel to choose transformations and fusions, based on Sparking Zero.", "Rueda para elegir transformaciones y fusiones, basada en Sparking Zero."},
    {"BT3 camera", "Battle camera that behaves like the one of BT3.", "Cámara de combate que se comporta como la de BT3."},
    {"Original close-cam silhouette", "Original flat silhouette when the camera gets inside a fighter.", "Silueta plana original cuando la cámara entra en un luchador."},
    {"Mid-air floating", "The fighters float softly while they stay still in the air.", "Los luchadores flotan suavemente mientras están quietos en el aire."},
    {"Transform wait", "Wait time of the transformation loop.", "Tiempo de espera del loop de transformación."},
    {"Quick blast recovery (fix)", "Recover quickly after a blast attack. Fixes an error of the original game.", "Recuperarse rápido tras un ataque blast. Corrige un error del juego original."},
    {"Two-phase charge (SZ)", "Ki charge in two phases with its own effects, based on Sparking Zero.", "Carga de ki en dos fases con sus propios efectos, basada en Sparking Zero."},
    {"Renders from 3D", "Creates the character pictures of the menus from their 3D models.", "Crea las imágenes de los personajes en los menús a partir de sus modelos 3D."},
    {"Quality", "Size of the generated pictures.", "Tamaño de las imágenes generadas."},
    {"Character renders", "Creates again every character picture the next time you enter the character select.", "Vuelve a crear todas las imágenes de personajes la próxima vez que entres a la selección de personajes."},
    {"Map renders from 3D", "Creates the map pictures of the menus from the 3D maps.", "Crea las imágenes de los mapas en los menús a partir de los mapas 3D."},
    {"Map renders", "Creates again every map picture the next time you enter the map select.", "Vuelve a crear todas las imágenes de mapas la próxima vez que entres a la selección de mapa."},
    {"3D model in select", "Shows the 3D model of the character in the character select.", "Muestra el modelo 3D del personaje en la selección de personajes."},
    {"3D map in select", "Shows the 3D map in the map select.", "Muestra el mapa 3D en la selección de mapa."},
    {"Character select style", "Layout of the character select. BT3: both sides on one screen.", "Diseño de la selección de personajes. BT3: ambos lados en una sola pantalla."},
    {"Names as text", "Draws the names in the select screens as text instead of pictures.", "Dibuja los nombres en las pantallas de selección como texto en lugar de imágenes."},
    {"GPU rendering", "Draws the game with the graphics card. Needed by most effects.", "Dibuja el juego con la tarjeta gráfica. La necesitan la mayoría de los efectos."},
    {"Vertical sync", "Syncs the image with the monitor to avoid tearing. Not recommended: it still shows problems.", "Sincroniza la imagen con el monitor para evitar cortes. No recomendado: aún presenta problemas."},
    {"FPS counter", "Shows the frames per second in a corner of the screen.", "Muestra los cuadros por segundo en una esquina de la pantalla."},
    {"Frame rate", "60 makes the game smoother. Experimental: some actions may still behave differently.", "60 hace el juego más fluido. Experimental: algunas acciones aún pueden comportarse distinto."},
    {"Smooth motion", "Interpolates the movement between frames. Low latency answers faster.", "Interpola el movimiento entre cuadros. Baja latencia responde más rápido."},
    {"Sound", "Original stereo or 3D stereo that places the sounds around you.", "Estéreo original o estéreo 3D que ubica los sonidos a tu alrededor."},
    {"Underwater sound", "Muffles the sound when the camera is under the water.", "Amortigua el sonido cuando la cámara está bajo el agua."},
    {"Voice on character select", "The character says one of its lines when you choose it (Character Reference mod).", "El personaje dice una de sus frases al elegirlo (mod Character Reference)."},
    {"+ Install .mkpak...", "Installs a mod from a .mkpak file.", "Instala un mod desde un archivo .mkpak."},
    {"Rumble intensity", "Strength of the controller rumble.", "Fuerza de la vibración del mando."},
    {"Test rumble", "Makes the controller rumble for a moment.", "Hace vibrar el mando un momento."},
    {"Restore defaults", "Restores the default bindings of this section for the device in use.", "Restaura las asignaciones predeterminadas de esta sección para el dispositivo en uso."},
    {"Mods", "Install, enable and order the mods, and open the library.", "Instala, activa y ordena los mods, y abre la biblioteca."},
    {"Create mod...", "Builds a mod with characters, maps and files, and installs it.", "Arma un mod con personajes, mapas y archivos, y lo instala."},
    {"Characters and maps...", "Destructive: edits the roster, character slots and maps directly in the game assets. It cannot be undone.", "Destructivo: edita el roster, los espacios de personajes y los mapas directamente en los assets del juego. No se puede deshacer."},
    {"Material editor...", "Edits the colours and materials of the characters.", "Edita los colores y materiales de los personajes."},
    {"Render editor...", "Edits the character renders of the menus.", "Edita los renders de personajes de los menús."},
    {"Map renders...", "Edits the map images of the stage select.", "Edita las imágenes de los mapas en la selección de escenario."},
};
static const OvDesc* ov_desc(const char* en) {
    for (const OvDesc& d : OV_DESC) if (!strcmp(d.key, en)) return &d;
    return nullptr;
}

// vertical layout of the page: y of each row (relative to the list top) and the total height
static float ov_layout(const std::vector<OvRow>& rows, std::vector<float>& ys) {
    ys.resize(rows.size());
    float y = 0;
    for (size_t i = 0; i < rows.size(); i++) { if (rows[i].kind == RK_HEAD && i) y += 3; ys[i] = y; y += rows[i].kind == RK_HEAD ? OV_HS : ov_rs(); }
    return y;
}
static std::vector<int> ov_order(const std::vector<OvRow>& rows) {   // focus order: sub-tab strip, rows, restart button
    std::vector<int> o;
    for (size_t i = 0; i < rows.size(); i++) if (rows[i].kind != RK_HEAD) o.push_back((int)i);
    o.push_back((int)rows.size());
    return o;
}
static int ov_first(const std::vector<OvRow>& rows) { return ov_order(rows)[0]; }

static void ov_close() {
    if (ov_need_restart()) { ov_confirm = 4; ov_yes = 1; return; }
    ov_open = false; ov_confirm = 0; ov_hold = true;
    printf("ajustes: menu cerrado\n");
}
static void ov_restart() { restart_requested = true; ov_open = false; ov_confirm = 0; ov_hold = true; printf("ajustes: reinicio desde el menu%s\n", ov_need_restart() ? " (requerido)" : ""); }
static void ov_change(const OvRow& r, int d) {
    if (!r.on || r.kind != RK_VALUE || r.names.empty()) return;
    int n = (int)r.names.size(), v = ((r.get() + d) % n + n) % n;
    r.set(v);
    printf("ajustes: %s -> %s\n", r.en, r.names[v].c_str());
}
static void ov_press(const OvRow& r) {
    if (!r.on) return;
    if (r.kind == RK_ACTION && r.act) r.act();
    else if (r.kind == RK_VALUE) ov_change(r, 1);
}
static void ov_confirm_do(bool yes) {
    int c = ov_confirm;
    ov_confirm = 0;
    if (!yes) return;
    if (c == 2) { ov_open = false; ov_hold = true; abierta = false; }
    else if (c == 4) ov_restart();
    else if (c == 6 && ov_rm >= 0 && ov_rm < (int)ov_mods.size()) {
        printf("ajustes: mod %s quitado\n", ov_mods[ov_rm].id.c_str());
        mod_uninstall(ov_mroot, ov_mods, (size_t)ov_rm);
        ov_mods_changed = true;
        ov_notice(ov_s("Mod removed", "Mod quitado"));
    }
    else if (c >= 7 && c <= 9) ol_confirm(c);
}
static void ov_tab_set(int t) { ol_leave(); ov_mmove = -1; ov_subs_t[ov_tab] = ov_sub; ov_tab = t; ov_sub = ov_subs_t[t]; ov_scroll = 0; ov_row = ov_first(ov_page()); }
static void ov_sub_set(int i) { ol_leave(); ov_mmove = -1; int n = std::max(1, ov_nsubs()); ov_sub = (i + n) % n; ov_scroll = 0; ov_row = ov_first(ov_page()); }
static int ov_hsub = -1;
static float ov_rstick() {
    float v = padv_main[P_RSR] - padv_main[P_RSL];
    for (int d = DEV_XI0; d < N_DEVS; d++)
        if (dev_ok[d]) { float x = dev_padv[d][P_RSR] - dev_padv[d][P_RSL]; if (fabsf(x) > fabsf(v)) v = x; }
    return v;
}
static uint32_t ov_keys() {
    if (!enfocada()) return 0;
    uint32_t k = 0;
    const int K[4] = {'Q', 'E', 'Z', 'C'};
    for (int i = 0; i < 4; i++) if (GetAsyncKeyState(K[i]) & 0x8000) k |= 1u << i;
    return k;
}

static bool ov_combo() {
    for (int d = DEV_XI0; d < N_DEVS; d++)
        if (dev_ok[d] && dev_padv[d][P_START] >= 0.5f && dev_padv[d][P_SELECT] >= 0.5f) return true;
    return padv_main[P_START] >= 0.5f && padv_main[P_SELECT] >= 0.5f;
}
static uint32_t ov_buttons() {
    EnterCriticalSection(&input_cs);
    pad_update(enfocada());
    Engine* ke = eng; float* kp = padv;
    eng = &eng_st[0]; padv = padv_main;
    uint32_t b = 0; float x, y;
    engine(CTX_MENU, enfocada(), false, &b, &x, &y);
    eng = ke; padv = kp;
    LeaveCriticalSection(&input_cs);
    return b;
}

static void ov_cap_start(int a, bool add) {
    bool pad = ov_pad();
    if (!ov_editable(a, pad)) return;
    ov_cap_cand.clear();
    if (!pad) {
        for (int v = 0x08; v < 0xFF; v++) {
            if (v == VK_SHIFT || v == VK_CONTROL || v == VK_MENU || v == VK_ESCAPE || v == VK_DELETE || (v >= VK_F1 && v <= VK_F24)) continue;
            if (v == VK_LBUTTON || v == VK_RBUTTON || v == VK_MBUTTON || v == VK_XBUTTON1 || v == VK_XBUTTON2 || v == VK_LWIN || v == VK_RWIN) continue;
            ov_cap_cand.push_back(C_KEY(v));
        }
        for (int v = 1; v <= 7; v++) ov_cap_cand.push_back(C_MOUSE(v));
    } else for (int v = 0; v < N_PADCODES; v++) ov_cap_cand.push_back(C_PAD(v));
    pad_update_locked(true);
    ov_cap_base.resize(ov_cap_cand.size());
    for (size_t i = 0; i < ov_cap_cand.size(); i++) ov_cap_base[i] = code_strength(ov_cap_cand[i], true, true);
    ov_cap = a; ov_cap_pad = pad; ov_cap_add = add;
    ov_cap_until = GetTickCount() + 5000;
}
static void ov_cap_end() {
    ov_cap = -1;
    ov_prev = ov_buttons(); ov_kprev = 15; ov_rs_prev = 2; ov_lb_prev = ov_rb_prev = true; ov_f1_prev = ov_combo_prev = true;
    InterlockedExchange(&wheel_acc, 0);
}
static void ov_cap_step() {
    InterlockedExchange(&wheel_acc, 0);
    if ((enfocada() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) || GetTickCount() > ov_cap_until) { ov_cap_end(); return; }
    pad_update_locked(true);
    for (size_t i = 0; i < ov_cap_cand.size(); i++) {
        float v = code_strength(ov_cap_cand[i], enfocada(), true);
        if (v >= 0.6f && ov_cap_base[i] < 0.3f) {
            uint32_t c = ov_cap_cand[i], *l = ov_cap_pad ? cfg.pad[ov_cap] : cfg.kb[ov_cap];
            if (!ov_cap_add) memset(l, 0, sizeof(uint32_t) * N_BINDS);
            bool dup = false;
            for (int k = 0; k < N_BINDS; k++) if (l[k] == c) dup = true;
            if (!dup) {
                int k = 0;
                while (k < N_BINDS && l[k]) k++;
                if (k == N_BINDS) { memmove(l, l + 1, sizeof(uint32_t) * (N_BINDS - 1)); k = N_BINDS - 1; }
                l[k] = c;
            }
            int a = ov_cap;
            ov_cfg_done(a, ov_cap_pad);
            ov_cap_end();
            return;
        }
        if (v < ov_cap_base[i]) ov_cap_base[i] = v;
    }
}
static void ov_unbind(int a) {
    bool pad = ov_pad();
    if (!ov_editable(a, pad)) return;
    uint32_t* l = pad ? cfg.pad[a] : cfg.kb[a];
    for (int k = N_BINDS - 1; k >= 0; k--) if (l[k]) { l[k] = 0; break; }
    ov_cfg_done(a, pad);
}

static void ov_begin(bool net) {
    ol_leave();
    if (!ov_lang_boot) ov_lang_boot = language;
    ov_gw_ready();
    gw_bars_list();
    ov_open = true; ov_net = net; ov_confirm = 0; ov_sub = 0; ov_cap = -1; memset(ov_subs_t, 0, sizeof(ov_subs_t)); ov_mods_read = false; ov_mmove = ov_mdrag = ov_opts = -1; ov_queued[0] = ov_queued[1] = false;
    if (ov_mem && ov_mem_tab >= 0 && ov_mem_tab < OV_NTABS) {
        memcpy(ov_subs_t, ov_mem_subs, sizeof(ov_subs_t));
        ov_tab = ov_mem_tab; ov_sub = ov_mem_sub;
        auto rows = ov_page();
        ov_row = std::max(0, std::min(ov_mem_row, (int)rows.size())); ov_scroll = ov_mem_scroll;
    } else ov_tab_set(0);
    if (ov_pend) { auto f = ov_pend; ov_pend = nullptr; ov_tab_set(4); ov_sub_set(0); ov_mods_load(); f(); }
    ov_prev = ov_buttons(); ov_dev = ov_im_prev = input_mode; ov_pad_prev = true; ov_kprev = 15; ov_rs_prev = 2; ov_combo_prev = true; ov_f1_prev = true; ov_lb_prev = ov_rb_prev = true; ov_t0 = GetTickCount(); ov_hover = -2;
    InterlockedExchange(&wheel_acc, 0);
    printf("ajustes: menu abierto%s\n", net ? " (sesion de red: el juego sigue)" : ", juego congelado");
}

// row under the mouse: rows.size() = restart button; -2 nothing
static int ov_hit(const std::vector<OvRow>& rows, const std::vector<float>& ys, float mx, float my) {
    if (mx >= OV_BX && mx < OV_BX + OV_BW && my >= OV_BY && my < OV_BY + OV_BH) return (int)rows.size();
    float top = (float)ov_list_top(), bot = (float)ov_list_bottom();
    if (my < top || my >= bot || mx < OV_PX + 10 || mx >= OV_PX + 10 + ov_list_w()) return -2;
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].kind == RK_HEAD) continue;
        float y = top + ys[i] - ov_scroll;
        if (my >= y && my < y + ov_rh()) return (int)i;
    }
    return -2;
}

static std::vector<std::string> ov_wrap(const std::string& s0, float size, float maxw, size_t maxl = 2);
static float ov_prompt(uint32_t code, const char* label, float x, float y);
#include "ui/overlay_lib.h"

// one step of the menu: input from the menu bindings, the mouse, F1 and Start+Select
static void ov_step() {
    if (ov_cap >= 0) { ov_cap_step(); return; }
    if (!ov_net) lb_poll();
    unsigned now = GetTickCount();
    bool wheel_on = now < wheel_until[0] || now < wheel_until[1];
    uint32_t b = ov_buttons();
    if (wheel_on) b &= ~(uint32_t)(PB_L | PB_R);   // the wheel scrolls the list, never changes tabs
    const uint32_t DIRS = PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT;
    uint32_t pr = b & ~ov_prev;
    if (b & DIRS) { if (pr & DIRS) ov_rep_t = now + 380; else if (now >= ov_rep_t) { pr |= b & DIRS; ov_rep_t = now + 90; } }
    ov_prev = b;
    bool foco = enfocada();
    bool f1 = foco && (GetAsyncKeyState(VK_F1) & 0x8000), combo = ov_combo();
    bool shut = (f1 && !ov_f1_prev) || (combo && !ov_combo_prev);
    ov_f1_prev = f1; ov_combo_prev = combo;
    if (shut) { ov_close(); return; }
    POINT p; GetCursorPos(&p); ScreenToClient(ventana, &p);
    bool lb = foco && (GetAsyncKeyState(VK_LBUTTON) & 0x8000), rb = foco && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    bool click = lb && !ov_lb_prev, rclick = rb && !ov_rb_prev, moved = p.x != ov_mouse_last.x || p.y != ov_mouse_last.y;
    ov_lb_prev = lb; ov_rb_prev = rb; ov_mouse_last = p;
    float mx = (p.x - ov_fx) / ov_fs, my = (p.y - ov_fy) / ov_fs;
    int wheel = (int)InterlockedExchange(&wheel_acc, 0);
    if (moved || click || rclick || wheel) ov_mouse = true;
    {   // device in use: any controller input (right stick and triggers included), or the mouse and keys
        bool pa = false;
        for (int i = 0; i < N_PADCODES && !pa; i++) if (padv_main[i] >= 0.5f) pa = true;
        for (int d = DEV_XI0; d < N_DEVS && !pa; d++) if (dev_ok[d]) for (int i = 0; i < N_PADCODES; i++) if (dev_padv[d][i] >= 0.5f) { pa = true; break; }
        if (input_mode != ov_im_prev) { ov_im_prev = input_mode; ov_dev = input_mode; }
        if (pa && !ov_pad_prev) ov_dev = 1;
        if (moved || click || rclick || wheel || ov_keys()) ov_dev = 0;
        ov_pad_prev = pa;
        if (input_mode != ov_dev) { input_mode = ov_dev; ov_im_prev = ov_dev; }
    }
    if (pr) ov_mouse = false;
    if (ov_confirm) {
        bool in0 = my >= 152 && my < 167 && mx >= 156 && mx < 236, in1 = my >= 152 && my < 167 && mx >= 244 && mx < 324;
        if (moved && (in0 || in1)) ov_yes = in0 ? 1 : 0;
        if (pr & (PB_LEFT | PB_RIGHT)) ov_yes = !ov_yes;
        if ((pr & PB_CRO) || (click && (in0 || in1))) ov_confirm_do(ov_yes != 0);
        else if ((pr & PB_CIR) || rclick) ov_confirm = 0;
        return;
    }
    OvIn in = {pr, click, rclick, moved, lb, mx, my, wheel};
    if (ov_lib_page() && ol_modal_step(in)) return;
    std::vector<OvRow> rows = ov_page();
    std::vector<float> ys;
    float content = ov_layout(rows, ys), area = (float)(ov_list_bottom() - ov_list_top());
    std::vector<int> ord = ov_order(rows);
    int n = (int)rows.size();
    if (ov_opts >= 0) {   // mod options popup
        const float px0 = 165, py0 = 88;
        int hov = -1;
        for (int i = 0; i < 4; i++) if (mx >= px0 + 10 && mx < px0 + 140 && my >= py0 + 24 + i * 17 && my < py0 + 39 + i * 17) hov = i;
        if (moved && hov >= 0) ov_opt = hov;
        if (pr & PB_UP) ov_opt = (ov_opt + 3) % 4;
        if (pr & PB_DOWN) ov_opt = (ov_opt + 1) % 4;
        if ((pr & PB_CRO) || (click && hov >= 0)) ov_mopt_do(ov_opt);
        else if ((pr & (PB_CIR | PB_TRI)) || rclick || click) ov_opts = -1;
        return;
    }
    if (ov_mods_page() && ov_mmove >= 0) {   // moving a mod: up/down change its place
        if ((pr & PB_UP) && ov_mmove > 0) { ov_mod_swap(ov_mmove, ov_mmove - 1); ov_mmove--; }
        else if ((pr & PB_DOWN) && ov_mmove + 1 < (int)ov_mods.size()) { ov_mod_swap(ov_mmove, ov_mmove + 1); ov_mmove++; }
        ov_row = ov_msel = ov_mmove;
        if (ov_mmove < n) { float y = ys[ov_mmove]; if (y < ov_scroll) ov_scroll = y; if (y + ov_rs() > ov_scroll + area) ov_scroll = y + ov_rs() - area; }
        if ((pr & (PB_CRO | PB_SQU | PB_CIR)) || click || rclick) ov_mmove = -1;
        return;
    }
    if (ov_mods_page() && ov_mdrag >= 0) {   // mouse drag of a mod
        int h = ov_hit(rows, ys, mx, my);
        if (!lb) ov_mdrag = -1;
        else if (h >= 0 && h < n && rows[h].kind == RK_MOD && rows[h].action != ov_mdrag) { ov_mod_swap(ov_mdrag, rows[h].action); ov_mdrag = rows[h].action; ov_row = ov_msel = ov_mdrag; }
        return;
    }
    uint32_t kk = ov_keys(), kp = kk & ~ov_kprev;
    ov_kprev = kk;
    float rs = ov_rstick();
    int rd = rs > 0.6f ? 1 : rs < -0.6f ? -1 : fabsf(rs) > 0.3f && ov_rs_prev != 2 ? ov_rs_prev : 0, sd = 0;
    if (ov_rs_prev == 2) rd = rd ? 2 : 0;   // a stick held when the menu opened waits for release
    else if (rd && rd != ov_rs_prev) { sd = rd; ov_rs_t = now + 380; }
    else if (rd && now >= ov_rs_t) { sd = rd; ov_rs_t = now + 200; }
    ov_rs_prev = rd;
    if (kp & 4) sd = -1;
    if (kp & 8) sd = 1;
    if (kp || sd) ov_mouse = false;
    if ((pr & PB_L) || (kp & 1)) { ov_tab_set((ov_tab + OV_NTABS - 1) % OV_NTABS); return; }
    if ((pr & PB_R) || (kp & 2)) { ov_tab_set((ov_tab + 1) % OV_NTABS); return; }
    if (sd && ov_nsubs()) { ov_sub_set(ov_sub + sd); return; }
    if (ov_lib_page()) {
        ov_hsub = -1;
        if (my >= OV_SY && my < OV_SY + OV_SH) for (int i = 0; i < ov_nsubs(); i++) { float x, w; ov_sub_rect(i, &x, &w); if (mx >= x && mx < x + w) ov_hsub = i; }
        if (click) {
            for (int i = 0; i < OV_NTABS; i++) { float x, w; ov_tab_rect(i, &x, &w); if (mx >= x && mx < x + w && my >= OV_TY && my < OV_TY + OV_TH) { ov_tab_set(i); return; } }
            if (ov_hsub >= 0) { ov_sub_set(ov_hsub); return; }
        }
        ol_step(in);
        return;
    }
    int at = 0;
    while (at < (int)ord.size() && ord[at] != ov_row) at++;
    if (at >= (int)ord.size()) at = 0;
    if (pr & PB_UP) ov_row = ord[(at + (int)ord.size() - 1) % ord.size()];
    if (pr & PB_DOWN) ov_row = ord[(at + 1) % ord.size()];
    if (wheel) {   // the wheel scrolls the list
        float maxs = std::max(0.0f, content - area);
        ov_scroll = std::max(0.0f, std::min(maxs, ov_scroll - wheel * (float)ov_rs() * 2));
    }
    ov_hover = ov_hit(rows, ys, mx, my);
    ov_hsub = -1;
    if (ov_nsubs() && my >= OV_SY && my < OV_SY + OV_SH)
        for (int i = 0; i < ov_nsubs(); i++) { float x, w; ov_sub_rect(i, &x, &w); if (mx >= x && mx < x + w) ov_hsub = i; }
    if (moved && ov_hover >= 0) ov_row = ov_hover;
    { int f = ov_mouse ? ov_hover : ov_row; if (f >= 0 && f < n && rows[f].kind == RK_MOD) ov_msel = rows[f].action; }
    if (rclick && ov_hover >= 0 && ov_hover < n && rows[ov_hover].kind == RK_MOD) { if (rows[ov_hover].on) { ov_opts = ov_msel = rows[ov_hover].action; ov_opt = 0; } return; }
    if (rclick && ov_hover >= 0 && ov_hover < n && rows[ov_hover].kind == RK_BIND) { ov_row = ov_hover; ov_cap_start(rows[ov_hover].action, true); return; }
    if ((pr & PB_CIR) || rclick) { ov_close(); return; }
    if (ov_row == n) {
        if (pr & PB_CRO) { ov_restart(); return; }
    } else if (ov_row >= 0 && ov_row < n) {
        if (pr & PB_LEFT) ov_change(rows[ov_row], -1);
        if (pr & PB_RIGHT) ov_change(rows[ov_row], 1);
        if (rows[ov_row].kind == RK_MOD) {
            int k = rows[ov_row].action;
            if (rows[ov_row].on) {
                if (pr & PB_CRO) ov_mod_toggle(k);
                if ((pr & PB_SQU) && ov_pad()) { ov_mmove = ov_msel = k; }
                if ((pr & PB_TRI) && ov_pad()) { ov_opts = ov_msel = k; ov_opt = 0; }
            }
        } else if (rows[ov_row].kind == RK_BIND) {
            int a = rows[ov_row].action;
            if (pr & PB_CRO) { ov_cap_start(a, false); return; }
            if (pr & PB_SQU) { ov_cap_start(a, true); return; }
            if (pr & PB_TRI) ov_unbind(a);
        } else if (pr & PB_CRO) ov_press(rows[ov_row]);
    }
    if (!ov_mouse && ov_row >= 0 && ov_row < n) {   // keep the focused row visible
        float y = ys[ov_row];
        if (y < ov_scroll) ov_scroll = y > OV_HS ? y - OV_HS : 0;
        if (y + ov_rs() > ov_scroll + area) ov_scroll = y + ov_rs() - area;
    }
    if (!click) return;
    for (int i = 0; i < OV_NTABS; i++) {
        float x, w; ov_tab_rect(i, &x, &w);
        if (mx >= x && mx < x + w && my >= OV_TY && my < OV_TY + OV_TH) { ov_tab_set(i); return; }
    }
    if (ov_hsub >= 0) { ov_sub_set(ov_hsub); return; }
    if (ov_hover == n) { ov_restart(); return; }
    if (ov_mods_page() && !ov_net && !ov_mods.empty() && mx >= OV_MDX && mx < OV_MDX + OV_MDW && my >= ov_list_bottom() - 12 && my < ov_list_bottom()) { ov_opts = ov_msel; ov_opt = 0; return; }
    if (ov_hover >= 0) {
        const OvRow& r = rows[ov_hover];
        float right = OV_PX + OV_PW - 10 - (content > area ? 10 : 0);
        if (r.kind == RK_MOD) {
            if (!r.on) return;
            ov_msel = r.action;
            if (mx >= OV_PX + 10 + ov_list_w() - (content > area ? 10 : 0) - 34) ov_mod_toggle(r.action);
            else ov_mdrag = r.action;
        } else if (r.kind == RK_BIND) ov_cap_start(r.action, false);
        else if (r.kind == RK_VALUE) ov_change(r, mx < right - 64 ? -1 : 1);
        else ov_press(r);
    }
}

static std::vector<std::string> ov_wrap(const std::string& s0, float size, float maxw, size_t maxl) {
    static std::map<std::string, std::vector<std::string>> done;
    std::string s = s0;
    for (char& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    std::string key = std::to_string((int)maxw) + "|" + std::to_string(maxl) + "|" + std::to_string((int)(size * 100)) + "|" + s;
    auto it = done.find(key);
    if (it != done.end()) return it->second;
    std::vector<std::string> out;
    std::string line;
    for (size_t i = 0; i <= s.size();) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        std::string w = s.substr(i, j - i), t = line.empty() ? w : line + " " + w;
        if (!line.empty() && psp_ui_text_w(0, size, t) > maxw) { out.push_back(line); line = w; }
        else line = t;
        i = j + 1;
    }
    if (!line.empty()) out.push_back(line);
    if (maxl == 2) while (out.size() > 2) { out[out.size() - 2] += " " + out.back(); out.pop_back(); }
    else if (out.size() > maxl) { out.resize(maxl); out.back() += "..."; }
    return done[key] = out;
}

// prompt "icon + text" drawn leftwards from x; returns the new x
static float ov_prompt(uint32_t code, const char* label, float x, float y) {
    float tw = ceilf(psp_ui_text_w(0, OV_DS, label)) + 2;
    x -= tw;
    ov_txt(0, OV_DS, label, x, y, (int)tw + 2, 12, 0, 0xFFFFFF);
    float iw = ov_ico_w(code, 11);
    if (iw > 0) { x -= iw + 2; ov_ico(code, x, y + 0.5f, 11); }
    return x - 10;
}

static bool ov_gl_begin(int cw, int ch) {
    if (!ov_prog) {
        ov_prog = gl_program(OV_VS, OV_FS);
        if (!ov_prog) return false;
        ov_u_r = glGetUniformLocation(ov_prog, "r"); ov_u_k = glGetUniformLocation(ov_prog, "k");
        OvImg w; w.w = w.h = 1; w.px.assign(1, 0xFFFFFFFFu);
        ov_white = ov_upload(w).t;
    }
    ov_cw = cw; ov_ch = ch;
    glViewport(0, 0, cw, ch);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ov_prog);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(ov_prog, "t"), 0);
    glBindVertexArray(vao_gl);
    return true;
}

// FPS counter (Graphics > Performance): frames shown in the main window per second, refreshed every 0.5 s, in the chosen corner
static int fps_shown = 0, fps_n = 0, fps_corner = -1;
static LARGE_INTEGER fps_t0 = {};
static void fps_draw(int cw, int ch) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    if (!fps_t0.QuadPart) fps_t0 = t;
    fps_n++;
    double s = (double)(t.QuadPart - fps_t0.QuadPart) / f.QuadPart;
    if (s >= 0.5) {
        fps_shown = (int)(fps_n / s + 0.5); fps_n = 0; fps_t0 = t;
    }
    if (fps_corner < 0) fps_corner = std::max(0, std::min(4, gw_env("TTT_FPS_COUNTER", 0)));
    int c = gw_loaded ? gw_v.fpsc : fps_corner;
    if (c <= 0 || !fps_shown || !ov_gl_begin(cw, ch)) return;
    float fx = ov_fx, fy = ov_fy, fs = ov_fs;
    ov_fs = std::min(cw / 480.0f, ch / 272.0f); ov_fx = 0; ov_fy = 0;
    const float W = 40, H = 12, M = 3, VW = cw / ov_fs, VH = ch / ov_fs;
    float x = c == 1 || c == 3 ? M : VW - W - M, y = c <= 2 ? M : VH - H - M;
    ov_txt(0, 0.52f, std::to_string(fps_shown) + " FPS", x, y, (int)W, (int)H, 1, 0xFFFFFF);
    ov_fx = fx; ov_fy = fy; ov_fs = fs;
    glDisable(GL_BLEND);
}

static void ov_draw(int cw, int ch) {
    if (!ov_open) return;
    if (!ov_confirm) { ov_mem = true; ov_mem_tab = ov_tab; ov_mem_sub = ov_sub; ov_mem_row = ov_row; ov_mem_scroll = ov_scroll; memcpy(ov_mem_subs, ov_subs_t, sizeof(ov_mem_subs)); ov_mem_subs[ov_tab] = ov_sub; }
    if (!ov_gl_begin(cw, ch)) return;
    ov_fs = std::min(cw / 480.0f, ch / 272.0f);
    ov_fx = (cw - 480 * ov_fs) * 0.5f; ov_fy = (ch - 272 * ov_fs) * 0.5f;
    { float fx = ov_fx, fy = ov_fy, fs = ov_fs; ov_fx = 0; ov_fy = 0; ov_fs = 1; ov_quad(ov_white, 0, 0, (float)cw, (float)ch, 0x002757, 0.65f); ov_fx = fx; ov_fy = fy; ov_fs = fs; }
    ov_spr(OP_PANEL, OV_PX, OV_PY, OV_PW, OV_PH);
    ov_spr(OP_TITLE, OV_PX + 6, OV_PY + 5, 180, 16);
    ov_txt(1, 0.72f, ov_s("SETTINGS", "AJUSTES"), OV_PX + 14, OV_PY + 5, 150, 16, 0, 0xFFFFFF);
    bool pad = ov_pad();
    {
        float x = OV_PX + OV_PW - 12;
        x = ov_prompt(ov_bind(A_M_BACK, pad), ov_s("Back", "Volver"), x, OV_PY + 7);
        ov_prompt(ov_bind(A_M_ACCEPT, pad), ov_s("Accept", "Aceptar"), x, OV_PY + 7);
    }
    float pulse = 0.55f + 0.45f * sinf((GetTickCount() - ov_t0) * 0.006f);
    for (int i = 0; i < OV_NTABS; i++) {
        float x, w; ov_tab_rect(i, &x, &w);
        bool on = i == ov_tab;
        ov_spr(on ? OP_TAB_ON : OP_TAB_OFF, x, OV_TY, (int)w, OV_TH);
        const char* tl = ov_s(OV_TABS[i].en, OV_TABS[i].es);
        float tsz = std::min(OV_TS * 0.92f, OV_TS * 0.92f * (w - 10) / std::max(1.0f, psp_ui_text_w(0, OV_TS * 0.92f, tl)));
        ov_txt(0, tsz, tl, x, OV_TY, (int)w, OV_TH, 1, on ? 0xFFFFFF : 0xC8D8F0);
    }
    if (pad) {
        uint32_t l = ov_code_tab(-1), r = ov_code_tab(1);
        ov_ico(l, OV_PX + OV_SIDE - 3 - ov_ico_w(l, 12), OV_TY + 0.5f, 12);
        ov_ico(r, OV_PX + OV_PW - OV_SIDE + 3, OV_TY + 0.5f, 12);
    }
    std::vector<OvRow> rows = ov_page();
    std::vector<float> ys;
    float content = ov_layout(rows, ys);
    int top = ov_list_top(), bot = ov_list_bottom(), n = (int)rows.size();
    float area = (float)(bot - top);
    bool scroll = content > area;
    ov_scroll = std::max(0.0f, std::min(ov_scroll, std::max(0.0f, content - area)));
    auto focus = [&](int r) { return (ov_mouse ? r == ov_hover : r == ov_row) && !ov_confirm; };
    if (ov_nsubs()) {   // sub-tabs: text only, the selected one underlined
        const float ss = OV_TS * 0.82f;
        ov_quad(ov_white, OV_PX + OV_SIDE, OV_SY + OV_SH, OV_PW - 2 * OV_SIDE, 1, 0x95F1FF, 0.22f);
        for (int i = 0; i < ov_nsubs(); i++) {
            float x, w; ov_sub_rect(i, &x, &w);
            bool on = i == ov_sub, hov = ov_mouse && i == ov_hsub && !ov_confirm;
            const char* t = ov_s(ov_subname(i).en, ov_subname(i).es);
            ov_txt(0, ss, t, x, OV_SY, (int)w, OV_SH - 1, 1, on ? 0xFFF379 : hov ? 0xFFFFFF : 0x9DB4D6);
            if (on) { float uw = std::min(w - 4, psp_ui_text_w(0, ss, t) + 8); ov_quad(ov_white, x + (w - uw) * 0.5f, OV_SY + OV_SH - 1, uw, 2, 0xFFA300, 1.0f); }
        }
        uint32_t l = ov_code_sub(-1), r = ov_code_sub(1);
        if (pad) ov_ico(l, OV_PX + OV_SIDE - 3 - ov_ico_w(l, 11), OV_SY + 0.5f, 11);
        if (pad) ov_ico(r, OV_PX + OV_PW - OV_SIDE + 3, OV_SY + 0.5f, 11);
    }
    float lx = OV_PX + 10, lw = ov_list_w() - (scroll ? 10 : 0);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)(ov_fx + lx * ov_fs), (int)(ch - (ov_fy + bot * ov_fs)), (int)(lw * ov_fs + 1), (int)(area * ov_fs + 1));
    for (int r = 0; r < n; r++) {
        float y = top + ys[r] - ov_scroll;
        if (y + ov_rs() < top || y > bot) continue;
        const OvRow& it = rows[r];
        if (it.kind == RK_HEAD) {
            ov_txt(1, 0.55f, ov_s(it.en, it.es), lx + 4, y, 200, OV_HS, 0, 0xFFFFFF);
            continue;
        }
        bool on = focus(r);
        float a = it.on ? 1.0f : 0.45f, vx = lx + lw - 124;
        if (it.kind == RK_MOD) {
            int k = it.action;
            const ModInfo& m = ov_mods[k];
            bool mv = k == ov_mmove || k == ov_mdrag;
            ov_spr(on || mv ? OP_ROW_ON : OP_ROW_OFF, lx, y, (int)lw, ov_rh(), a);
            if (on || mv) ov_spr(OP_CURSOR, lx, y + 1, (int)lw, ov_rh() - 2, mv ? pulse : pulse * 0.6f);
            float ta = a * (m.enabled ? 1.0f : 0.55f);
            ov_txt(0, 0.4f, std::to_string(k + 1), lx + 4, y, 12, ov_rh(), 1, 0x95F1FF, ta);
            float px = lx + lw - 22;
            ov_txt(0, 0.4f, ol_fit(mw_name(m), 0.4f, px - lx - 22), lx + 18, y, (int)(px - lx - 20), ov_rh(), 0, mv ? 0xFFA300 : on ? 0xFFF379 : 0xFFFFFF, ta);
            ov_quad(ov_white, px, y + 2, 18, 6, m.enabled ? 0x2E9E5A : 0x5A5E66, a);
            ov_txt(0, 0.36f, m.enabled ? ov_s("On", "Sí") : ov_s("Off", "No"), px, y + 2, 18, 6, 1, 0xFFFFFF, a);
            continue;
        }
        ov_spr(on ? OP_ROW_ON : OP_ROW_OFF, lx, y, (int)lw, ov_rh(), a);
        if (on) ov_spr(OP_CURSOR, lx, y + 1, (int)lw, ov_rh() - 2, pulse * 0.6f);
        const char* tg = it.tag2 == 1 ? "EXPERIMENTAL" : it.tag2 == 2 ? ov_s("NOT RECOMMENDED", "NO RECOMENDADO") : it.tag2 == 3 ? ov_s("DESTRUCTIVE", "DESTRUCTIVO") : nullptr;
        uint32_t tc = it.tag2 >= 2 ? 0xFF5A3C : 0xFFA300;
        float tw = tg ? ceilf(psp_ui_text_w(0, 0.45f, tg)) + 8 : 0, lend = tg ? vx - tw - 10 : it.kind == RK_BIND ? lx + lw * 0.55f : it.kind == RK_ACTION && it.tag.empty() ? lx + lw - 6 : vx - 6;
        ov_txt(0, OV_TS, ov_s(it.en, it.es), lx + 12, y, (int)(lend - lx - 12), ov_rh(), 0, on ? 0xFFF379 : 0xFFFFFF, a);
        if (tg) {
            ov_quad(ov_white, vx - tw - 6, y + 3, tw, 9, tc, 0.22f * a);
            ov_txt(0, 0.45f, tg, vx - tw - 6, y + 3, (int)tw, 9, 1, tc, a);
        }
        if (it.kind == RK_BIND) {
            float right = lx + lw - 8;
            if (it.action == ov_cap) {
                int left = std::max(0, (int)((ov_cap_until - GetTickCount() + 999) / 1000));
                std::string t = std::string(ov_cap_pad ? ov_s("Press a button...", "Pulsa un botón...") : ov_s("Press a key...", "Pulsa una tecla...")) + " " + std::to_string(left) + " s";
                ov_txt(0, OV_TS, t, right - 160, y, 160, ov_rh(), 2, 0xEF3300, pulse);
                continue;
            }
            std::vector<uint32_t> q;
            bool ed = ov_seq(it.action, pad, q);
            float ia = ed ? 1.0f : 0.6f;
            auto tok = [&](uint32_t c) -> const char* {
                return c == OV_PLUS ? "+" : c == OV_X2 ? "x2" : c == OV_OR ? "/" : c == OV_NA ? "-" : c == OV_EMPTY ? ov_s("Unassigned", "Sin asignar") : nullptr;
            };
            float qw = 0;
            for (uint32_t c : q) { const char* t = tok(c); qw += (t ? ceilf(psp_ui_text_w(0, OV_TS, t)) + 2 : ov_ico_w(c, 11)) + 3; }
            float x = right - qw;
            if (!ed && q[0] != OV_NA) {
                const char* au = ov_s("auto", "automático");
                float w = ceilf(psp_ui_text_w(0, OV_TS * 0.8f, au)) + 2;
                ov_txt(0, OV_TS * 0.8f, au, x - w - 6, y, (int)w + 2, ov_rh(), 0, 0x9DB4D6, a);
            }
            for (uint32_t c : q) {
                const char* t = tok(c);
                if (t) { float w = ceilf(psp_ui_text_w(0, OV_TS, t)) + 2; ov_txt(0, OV_TS, t, x, y, (int)w + 2, ov_rh(), 0, c == OV_EMPTY ? 0xFFA300 : 0x95F1FF, ia); x += w + 3; }
                else x += ov_ico(c, x, y + 2, 11, ia) + 3;
            }
            continue;
        }
        if (it.kind == RK_ACTION) {
            if (!it.tag.empty()) ov_txt(0, OV_TS, it.tag, vx, y, 110, ov_rh(), 1, 0x95F1FF, a);
            continue;
        }
        if (it.on) { ov_spr(OP_ARROW_L, vx, y + 2, 11, 11); ov_spr(OP_ARROW_R, lx + lw - 26, y + 2, 11, 11); }
        int v = it.get();
        ov_txt(0, OV_TS, v >= 0 && v < (int)it.names.size() ? it.names[v] : "", vx + 12, y, 86, ov_rh(), 1, 0x95F1FF, a);
    }
    glDisable(GL_SCISSOR_TEST);
    if (scroll) {   // knob at its native size, only its speed changes with the length of the list
        float sx = lx + lw + 4, kh = 24;
        ov_spr(OP_SB_TRACK, sx + 1, (float)top, 4, (int)area, 0.8f);
        float ky = top + (area - kh) * (ov_scroll / (content - area));
        ov_spr(OP_SB_KNOB, sx - 1, ky, 8, (int)kh);
    }
    if (ov_mods_page() && !ov_mods.empty()) {   // details of the selected mod
        const ModInfo& m = ov_mods[std::min(ov_msel, (int)ov_mods.size() - 1)];
        float dx = OV_MDX, dy = (float)top, iw = OV_MDW, ih = std::min(84.0f, floorf(iw * 9 / 16));
        ov_quad(ov_white, dx, dy, iw, ih, 0x16365F, 0.9f);
        ov_quad(ov_white, dx, dy, iw, 1, 0x3D6A9A, 1); ov_quad(ov_white, dx, dy + ih - 1, iw, 1, 0x3D6A9A, 1);
        ov_quad(ov_white, dx, dy, 1, ih, 0x3D6A9A, 1); ov_quad(ov_white, dx + iw - 1, dy, 1, ih, 0x3D6A9A, 1);
        if (const OvTex* t = ov_mod_tex(m)) {
            float k = std::min((iw - 2) / t->w, (ih - 2) / t->h), w = t->w * k, h = t->h * k;
            ov_quad(t->t, dx + (iw - w) * 0.5f, dy + (ih - h) * 0.5f, w, h, 0xFFFFFF, 1);
        } else ov_txt(0, OV_DS, ov_s("No image", "Sin imagen"), dx, dy, (int)iw, (int)ih, 1, 0x9DB4D6);
        float ty = dy + ih + 3;
        ov_txt(0, 0.5f, ol_fit(mw_name(m), 0.5f, iw - 2), dx, ty, (int)iw, 11, 0, 0xFFF379);
        ty += 11;
        std::string v = mw_field(m, "version"), au = mw_field(m, "autor"), tg = ov_tags(m), meta;
        if (!v.empty()) meta = "v" + v;
        if (!au.empty()) meta += (meta.empty() ? "" : ", ") + au;
        if (!meta.empty()) { ov_txt(0, 0.42f, ol_fit(meta, 0.42f, iw - 2), dx, ty, (int)iw, 9, 0, 0x95F1FF); ty += 9; }
        if (!tg.empty()) for (const std::string& l : ov_wrap(tg, 0.42f, iw - 2, 3)) { ov_txt(0, 0.42f, l, dx, ty, (int)iw, 9, 0, 0x95F1FF); ty += 9; }
        ty += 2;
        float bb = bot - 15;
        int nl = std::max(0, (int)((bb - ty) / 9));
        std::string de = mw_field(m, "descripcion");
        if (!de.empty() && nl > 0) {
            std::vector<std::string> ls = ov_wrap(de, 0.42f, iw - 2, (size_t)std::max(3, nl));
            for (size_t i = 0; i < ls.size() && (int)i < nl; i++) ov_txt(0, 0.42f, ls[i], dx, ty + i * 9, (int)iw, 9, 0, 0xD8E4F4);
        }
        if (!ov_net) {
            uint32_t oc = pad ? ov_bind(A_M_RANDOM, true) : C_MOUSE(2);
            ov_spr(OP_BUTTON, dx, bot - 12, (int)iw, 12, ov_opts >= 0 ? 1.0f : 0.8f);
            float tw2 = ceilf(psp_ui_text_w(0, 0.48f, ov_s("Options", "Opciones"))) + 2, iw2 = ov_ico_w(oc, 9), x0 = dx + (iw - tw2 - iw2 - 3) * 0.5f;
            ov_ico(oc, x0, bot - 10.5f, 9);
            ov_txt(0, 0.48f, ov_s("Options", "Opciones"), x0 + iw2 + 3, bot - 12, (int)tw2 + 2, 12, 0, 0xFFFFFF);
        }
    }
    int fr = ov_confirm ? -2 : ov_mouse ? ov_hover : ov_row;
    const char* d = nullptr;
    if (fr == n) d = ov_s("Restarts the game to apply the pending changes.", "Reinicia el juego para aplicar los cambios pendientes.");
    else if (ov_cap >= 0) d = ov_cap_pad ? ov_s("Press the controller button to assign. Wait 5 s to cancel.", "Pulsa el botón del mando que quieres asignar. Espera 5 s para cancelar.")
                                     : ov_s("Press the key or mouse button to assign. Esc cancels.", "Pulsa la tecla o el botón del ratón que quieres asignar. Esc cancela.");
    else if (ov_mmove >= 0 || ov_mdrag >= 0) d = ov_s("Up and down change its place. The first one wins when two mods change the same thing.", "Arriba y abajo cambian su lugar. El primero gana cuando dos mods cambian lo mismo.");
    else if (fr >= 0 && fr < n && rows[fr].kind == RK_MOD) d = nullptr;
    else if (fr >= 0 && fr < n && rows[fr].kind == RK_BIND) { const OvActTxt* t = ov_acttxt(rows[fr].action); if (t) d = ov_s(t->den, t->des); }
    else if (fr >= 0 && fr < n && rows[fr].kind != RK_HEAD) { const OvDesc* od = ov_desc(rows[fr].en); if (od) d = ov_s(od->en, od->es); }
    if (ov_mods_page() || ov_lib_page()) d = nullptr;
    else ov_quad(ov_white, OV_PX + 12, OV_DY - 3, OV_PW - 24, 1, 0x95F1FF, 0.22f);
    if (d) {
        std::vector<std::string> ls = ov_wrap(d, OV_DS, (float)(OV_PW - 30));
        for (size_t i = 0; i < ls.size(); i++) ov_txt(0, OV_DS, ls[i], OV_PX + 14, OV_DY + (float)i * 12, OV_PW - 28, 12, 0, 0xD8E4F4);
    }
    if (ov_lib_page()) ol_draw(pad);
    bool need = ov_need_restart(), bon = focus(n);
    const char* hint = need ? ov_s("Restart required", "Reinicio requerido")
                            : fr >= 0 && fr < n && rows[fr].restart ? ov_s("Applies after restarting", "Se aplica al reiniciar") : nullptr;
    bool note = GetTickCount() < ov_note_t;
    if (note && !ov_lib_page()) ov_txt(0, OV_TS * 0.9f, ov_note, OV_PX + 12, OV_BY, 200, OV_BH, 0, 0x95F1FF);
    else if (hint && !(ov_lib_page() && GetTickCount() < ov_note_t)) ov_txt(0, OV_TS * 0.9f, hint, OV_PX + 12, OV_BY, 150, OV_BH, 0, 0xFFA300, need ? pulse : 1.0f);
    if (ov_mods_page() && !ov_confirm && ov_opts < 0) {
        float x = OV_PX + OV_PW - 12, py = OV_BY - 16;
        if (ov_mmove >= 0) ov_prompt(ov_bind(A_M_ACCEPT, pad), ov_s("Done", "Listo"), x, py);
        else if (fr >= 0 && fr < n && rows[fr].kind == RK_MOD && rows[fr].on) {
            x = ov_prompt(pad ? ov_bind(A_M_RANDOM, true) : C_MOUSE(2), ov_s("Options", "Opciones"), x, py);
            if (pad) x = ov_prompt(ov_bind(A_M_SWITCH, true), ov_s("Move", "Mover"), x, py);
            ov_prompt(ov_bind(A_M_ACCEPT, pad), ov_s("On / Off", "Activar"), x, py);
        }
    }
    else if (!note && ov_cap < 0 && fr >= 0 && fr < n && rows[fr].kind == RK_BIND && ov_editable(rows[fr].action, pad)) {
        float x = OV_BX - 10;
        x = ov_prompt(ov_bind(A_M_RANDOM, pad), ov_s("Remove", "Quitar"), x, OV_BY + 1.5f);
        x = ov_prompt(ov_bind(A_M_SWITCH, pad), ov_s("Add", "Añadir"), x, OV_BY + 1.5f);
        ov_prompt(ov_bind(A_M_ACCEPT, pad), ov_s("Change", "Cambiar"), x, OV_BY + 1.5f);
    }
    ov_spr(OP_BUTTON, OV_BX, OV_BY, OV_BW, OV_BH, bon || need ? 1.0f : 0.45f);
    ov_txt(0, OV_TS, ov_s("Restart", "Reiniciar"), OV_BX, OV_BY, OV_BW, OV_BH, 1, bon ? 0xEF3300 : need ? 0xFFA300 : 0xFFFFFF);
    if (ov_lib_page()) ol_draw_modal();
    if (ov_confirm) {
        ov_spr(OP_DIALOG, 140, 92, 200, 80);
        std::string q = ov_confirm == 2 ? ov_s("Quit the game?", "¿Cerrar el juego?") : ov_confirm == 6 ? std::string(ov_s("Remove \"", "¿Quitar \"")) + (ov_rm >= 0 && ov_rm < (int)ov_mods.size() ? mw_name(ov_mods[ov_rm]) : "") + ov_s("\"?", "\"?")
                                                                                     : ov_s("Restart required to apply the changes", "Se requiere reiniciar para aplicar los cambios");
        std::string sub, yes = ov_confirm == 4 ? ov_s("Accept", "Aceptar") : ov_confirm == 6 ? ov_s("Remove", "Quitar") : ov_s("Yes", "Sí");
        if (ov_confirm == 6) sub = ov_s("Its folder in mods is deleted. Original assets are not touched.", "Se borra su carpeta de mods. Los assets originales no se tocan.");
        if (ov_confirm >= 7) ol_confirm_text(ov_confirm, q, sub, yes);
        float qy = 99;
        for (const std::string& l : ov_wrap(q, OV_TS, 180, 3)) { ov_txt(0, OV_TS, l, 150, qy, 180, 12, 1, 0xFFFFFF); qy += 12; }
        if (!sub.empty()) for (const std::string& l : ov_wrap(sub, 0.5f, 180, 3)) { ov_txt(0, 0.5f, l, 150, qy + 1, 180, 10, 1, 0x9DB4D6); qy += 10; }
        const char* no = ov_confirm == 4 ? ov_s("Cancel", "Cancelar") : ov_s("No", "No");
        for (int k = 0; k < 2; k++) {
            bool on = (k == 0) == (ov_yes != 0);
            float x = k == 0 ? 156.0f : 244.0f;
            ov_spr(OP_BUTTON, x, 152, 80, 15, on ? 1.0f : 0.35f);
            ov_txt(0, OV_TS, k == 0 ? yes : no, x, 152, 80, 15, 1, on ? 0xEF3300 : 0xFFFFFF);
        }
    }
    if (ov_opts >= 0 && ov_opts < (int)ov_mods.size()) {
        const float px0 = 165, py0 = 88;
        ov_spr(OP_DIALOG, px0, py0, 150, 26 + 4 * 17 + 6);
        ov_txt(0, OV_TS, mw_name(ov_mods[ov_opts]), px0 + 10, py0 + 6, 130, 15, 1, 0xFFF379);
        for (int i = 0; i < 4; i++) {
            bool on = i == ov_opt;
            float y = py0 + 24 + i * 17;
            ov_spr(OP_BUTTON, px0 + 10, y, 130, 15, on ? 1.0f : 0.35f);
            ov_txt(0, OV_TS, ov_s(OV_MOPT[i][0], OV_MOPT[i][1]), px0 + 10, y, 130, 15, 1, on ? 0xEF3300 : i == 3 ? 0xFF8A70 : 0xFFFFFF);
        }
    }
    glDisable(GL_BLEND);
}


// held buttons of the menu never reach the game: input is masked until everything is released (or 1.5 s)
static bool ov_mask_on() {
    if (ov_open) return true;
    if (!ov_hold) return false;
    static unsigned t = 0;
    if (!t) t = GetTickCount();
    bool held = ov_buttons() || ov_combo() || (GetAsyncKeyState(VK_F1) & 0x8000) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    if (!held || GetTickCount() - t > 1500) { ov_hold = false; t = 0; }
    return ov_hold;
}
static void ov_mask_local() { for (int k = 0; k < 4; k++) { loc_b[k] = 0; loc_x[k] = loc_y[k] = 128; } }

static void ov_drop(const std::string& f) {
    if (f.size() <= 6 || _stricmp(f.c_str() + f.size() - 6, ".mkpak")) return;
    if (ov_open && !ov_net) { ov_mods_load(); ov_tab_set(4); ov_sub_set(0); ov_mod_install(f); }
    else if (!ov_open) psp_link_queue(f.c_str());
}
static void pintar_gl();
static void ov_run() {
    if (!usar_gl) return;
    if (psp_net_live()) { ov_begin(true); return; }
    ov_begin(false);
    pause_begin();
    while (ov_open && abierta) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { abierta = false; break; }
            if (msg.message == WM_CHAR && ol_wants_chars()) { ol_char((wchar_t)msg.wParam); continue; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!abierta) break;
        ov_step();
        rumble_update();
        cursor_update();
        pintar_gl();
        Sleep(12);
    }
    if (lb_dl.st == 1) lb_dl_cancel = true;
    ol_leave();
    unsigned t = GetTickCount();
    while (abierta && ov_mask_on() && GetTickCount() - t < 1600) Sleep(10);
    pause_end();
}
