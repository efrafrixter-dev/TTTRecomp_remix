void psp_fn_ring_report();
static void report() {
    psp_coverage_save();
    psp_step_census_dump();
    printf("\nllamadas a importaciones: %ld  (ultima: %s)\n", total_hle,
           last_hle >= 0 ? psp_import_name(last_hle) : "ninguna");
    printf("llamadas/saltos no resueltos: %u\n", psp_unresolved);
    printf("\norden de primera aparicion:\n");
    for (long i = 0; i < order_n; i++)
        printf("  %2ld  %-40s x%llu\n", i + 1, psp_import_name(order[i]),
               (unsigned long long)psp_hle_count[order[i]]);
    printf("\nhilos: %d\n", psp_threads_n());
    for (int i = 0; i < psp_threads_n(); i++)
        printf("  hilo %d entrada 0x%08X prioridad %-3d estado %s\n", i, psp_thread_entry(i), psp_thread_prio(i),
               psp_thread_state(i) == 3 ? "terminado" : psp_thread_state(i) == 2 ? "bloqueado" :
               psp_thread_state(i) == 1 ? "listo" : "sin arrancar");
    printf("\nlas mas llamadas:\n");
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < psp_imports_n() && i < 4096; i++)
            if (psp_hle_count[i] && (best < 0 || psp_hle_count[i] > psp_hle_count[best])) best = i;
        if (best < 0) break;
        printf("  %-40s x%llu\n", psp_import_name(best), (unsigned long long)psp_hle_count[best]);
        psp_hle_count[best] = 0;
    }
    psp_ge_report();
    psp_gpu_report();
    snd_report();
    printf("\n");
    psp_census_report();
    psp_fn_ring_report();
    perf_report("final");
    printf("\nMOTIVO DEL FINAL: %s\n", end_reason);
    printf("\nfuncion actual por hilo:\n");
    for (int i = 0; i < psp_threads_n(); i++) {
        Ctx* x = psp_thread_ctx(i);
        if (x) printf("  hilo %d: en f_%08X tras %llu llamadas, pc de retorno 0x%08X\n", i, x->fn,
                      (unsigned long long)x->llamadas, x->r[31]);
    }
}

// games/ttt/recomp.txt [legacy_env]: old environment variables and cfg keys, emitted into generated game_data.cpp
extern const char* const GAME_LEGACY_ENV[][2]; extern const int GAME_LEGACY_ENV_N;
static const char* legacy_env(const char* k) {
    for (int i = 0; i < GAME_LEGACY_ENV_N; i++) if (!strcmp(k, GAME_LEGACY_ENV[i][0])) return GAME_LEGACY_ENV[i][1];
    return k;
}
static void env_set(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
#endif
}
static void env_migrate() {
    for (int i = 0; i < GAME_LEGACY_ENV_N; i++) { auto& r = GAME_LEGACY_ENV[i]; const char* v = getenv(r[0]); if (v && !getenv(r[1])) env_set(r[1], v); }
}
// graphics.cfg (Graphics window): KEY=value overriding the environment; TTT_GRAPHICS=0 ignores it
// defaults of a new install; keys missing from an existing file are added with these values
static const char* GRAPHICS_DEF =
    "TTT_SCALE=1\nTTT_WINDOW=1280x720\nTTT_FILTER=2\nTTT_SHARPNESS=0.00\nTTT_MSAA=0\nTTT_FXAA=0\nTTT_TEXFILTER=0\nTTT_HIDE_CURSOR=3\n"
    "TTT_DISCORD=1\nTTT_SKIP_CREDITS=1\n"
    "TTT_FX_BLOOM=0\nTTT_FX_VFXMAP=0.60\nTTT_CEL_BT3FX=1\nTTT_BOLTS=0\nTTT_BT3_STORM=1\nTTT_BT3_BUBBLES=1\nTTT_BT3_HAZE=1\nTTT_FX_ATM=0\nTTT_FX_SUN=0\n"
    "TTT_FX_MBLUR=2\nTTT_WATER=2\nTTT_RAIN=0\nTTT_WET=0\nTTT_FX_IMPACT=1\nTTT_FX_SHAKE=1\n"
    "TTT_CEL=0\nTTT_CEL_SMOOTH=0.00\nTTT_CEL_GLOW=1\nTTT_SOFT_SHADOWS=0\nTTT_OUTLINE=2\nTTT_OUTLINE_MODE=1\nTTT_LOD=4\nTTT_DYN_COLORS=1\nTTT_TEAM_GLOW=0\nTTT_LIPSYNC=1\nTTT_BLINK=1\n"
    "TTT_BATTLE_HUD=1\nTTT_HUD_BT3=0\nTTT_BAR_NAMES=1\nTTT_BARS_BEHIND=0\nTTT_CHARGE_LIST=1\nTTT_TR_WHEEL=1\nTTT_BT3_CAM=0\nTTT_CAM_SILHOUETTE=0\nTTT_FLOAT=1\nTTT_TRANSFORM_WAIT=2\nTTT_BLAST_END=1\nTTT_CHARGE2=0\n"
    "TTT_GEN_RENDERS=0\nTTT_GEN_RENDERS_Q=2\nTTT_GEN_MAP_RENDERS=0\nTTT_SELECT_3D=0\nTTT_SEL3D_ENTRY=0\nTTT_MAP_3D=0\nTTT_TEXT_NAMES=1\n"
    "TTT_GLRAST=1\nTTT_VSYNC=0\nTTT_BATTLE_FPS=30\nTTT_SMOOTH=0\nTTT_FPS_COUNTER=0\nTTT_SND_WATER=1\n";
// the file the first alpha wrote on its first run (480x272): replaced by the defaults above
static const char* GRAPHICS_OLD = "TTT_SCALE=1\nTTT_WINDOW=480x272\nTTT_FILTER=2\nTTT_SHARPNESS=0.25\nTTT_OUTLINE=2\nTTT_LOD=4\nTTT_GLRAST=1\nTTT_VSYNC=0\nTTT_BATTLE_FPS=30\n";
static void graphics_load() {
    const char* e = getenv("TTT_GRAPHICS");
    if (e && !atoi(e)) return;
    FILE* f = fopen("graphics.cfg", "r");
    bool migrated = false, reset = false;
    if (!f && (f = fopen("graficos.cfg", "r"))) migrated = true;   // old name: read once, rewritten and removed
    std::string text;
    if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) text.append(b, n); fclose(f); }
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (!f || text == GRAPHICS_OLD) { reset = f != nullptr; text = GRAPHICS_DEF; migrated = true; }
    std::string out;
    std::set<std::string> have;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        std::string l = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? text.size() : j + 1;
        size_t ig = l.find('=');
        if (ig == std::string::npos || l.compare(0, 4, "TTT_")) continue;
        std::string k = legacy_env(l.substr(0, ig).c_str()), v = l.substr(ig + 1);
        env_set(k.c_str(), v.c_str());
        have.insert(k);
        out += k + "=" + v + "\n";
    }
    int added = 0;
    for (std::string d = GRAPHICS_DEF; !d.empty();) {
        size_t j = d.find('\n');
        std::string l = d.substr(0, j);
        d.erase(0, j + 1);
        size_t ig = l.find('=');
        std::string k = l.substr(0, ig);
        if (have.count(k)) continue;
        env_set(k.c_str(), l.substr(ig + 1).c_str());
        out += l + "\n";
        added++;
    }
    if (migrated || added) {
        FILE* w = fopen("graphics.cfg", "w");
        if (w) { fputs(out.c_str(), w); fclose(w); remove("graficos.cfg"); }
    }
    printf("graphics: ajustes de graphics.cfg%s%s, %d valores predeterminados agregados\n", !f ? " (nuevo)" : "", reset ? " (el de la primera alpha se reemplazo)" : "", added);
}

void psp_disc_warm();
void psp_window_wait(volatile bool* done);
static const std::chrono::steady_clock::time_point boot_t0 = std::chrono::steady_clock::now();
// startup timing: ms since the process was created (and since the runtime's own start)
void psp_boot_mark(const char* what) {
    static int n = 0;
    static double created = -1;
    if (n++ > 80) return;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - boot_t0).count();
#ifdef _WIN32
    if (created < 0) {
        FILETIME c, e, k, u, now;
        GetSystemTimeAsFileTime(&now);
        created = GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? (double)(((uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime) - ((uint64_t)c.dwHighDateTime << 32 | c.dwLowDateTime)) / 10000.0 - ms : 0;
    }
#else
    created = 0;
#endif
    printf("arranque: %7.0f ms (%6.0f desde el inicio del proceso) %s\n", ms, ms + created, what);
}

void psp_link_queue(const char* s);
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    psp_boot_mark("main");
    env_migrate();
    graphics_load();
    patches_load();
    psp_boot_mark("ajustes y parches");
    net_account_boot();
    psp_boot_mark("cuenta y Discord");
    const char* eboot = argc > 1 ? argv[1] : "../../game/EBOOT.OLD";
    int seconds_n = argc > 2 ? atoi(argv[2]) : 20;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--window") || !strcmp(argv[i], "--ventana")) with_window = true;
        if ((!strcmp(argv[i], "--name") || !strcmp(argv[i], "--nombre")) && i + 1 < argc) { strncpy(player_name, argv[i + 1], 31); player_name[31] = 0; }
    }
#ifdef _WIN32
    {   // .mkpak opened with the port (double click) or a tttrecomp:// link; a .mkpak is installed before the mods are read
        int wn = 0;
        LPWSTR* wa = CommandLineToArgvW(GetCommandLineW(), &wn);
        for (int i = 2; wa && i < wn; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wa[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(n > 0 ? n - 1 : 0, 0);
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wa[i], -1, &a[0], n, nullptr, nullptr);
            std::string low = a;
            for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
            if (!low.compare(0, 12, "tttrecomp://")) { psp_link_queue(a.c_str()); continue; }
            if (low.size() < 7 || low.compare(low.size() - 6, 6, ".mkpak")) continue;
            const char* e = getenv("TTT_ASSETS");
            std::string root = mods_root_of(e && *e && strcmp(e, "0") ? e : "../../assets"), id, err;
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(root), ec);
            if (mod_install(root, a, &id, &err)) {
                mods_list(root);
                bool closed = false;
                for (auto it = std::filesystem::recursive_directory_iterator(std::filesystem::u8path(root + "/" + id + "/Packfile"), ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                    std::string x = it->path().extension().u8string();
                    for (auto& ch : x) ch = (char)tolower((unsigned char)ch);
                    if (x == ".pak" || x == ".txt1" || x == ".vagp" || x == ".atex" || x == ".rhg" || x == ".unk") { closed = true; break; }
                }
                printf("enlaces: %s instalado antes de arrancar como %s%s\n", a.c_str(), id.c_str(), closed ? " (con paks cerrados)" : "");
                psp_link_queue(((closed ? "open:" : "installed:") + id).c_str());
            } else psp_link_queue(("fail:Could not install:\n" + a + "\n\n" + err).c_str());
        }
        if (wa) LocalFree(wa);
    }
#endif
    if (*net_account_name()) { snprintf(player_name, sizeof(player_name), "%s", net_account_name()); printf("perfil: nombre del jugador %s\n", player_name); }
    if (with_window) {   // the window first; the virtual disc is prepared meanwhile (TTT_DISC_FIRST=1: the old order)
        const char* df = getenv("TTT_DISC_FIRST");
        bool first = df && atoi(df);
        if (first) { disc_init(); psp_boot_mark("disco virtual listo"); }
        if (!psp_window_init(psp_scale())) { fprintf(stderr, "no puedo abrir la ventana\n"); return 1; }
        psp_boot_mark("ventana abierta");
        if (!first) {
            static volatile bool disc_done = false;
            std::thread dt([] { disc_init(); disc_done = true; });
            psp_window_wait(&disc_done);
            dt.join();
            psp_boot_mark("disco virtual listo");
        }
        psp_disc_warm();
        seconds_n = 100000;
        psp_scheduler_hook(psp_window_paint);
        printf("ventana abierta. F1 menu, Ctrl+F1 censo de escena, Esc salir, F12 informe, F11 pantalla completa, F10 encaje, F9 captura\n");
    }
    if (!with_window) { disc_init(); psp_boot_mark("disco virtual listo"); }
    psp_mem_init(0x10000000u);
    if (!load_elf(eboot, &GP)) { fprintf(stderr, "no puedo cargar %s\n", eboot); return 1; }
    printf("gp = 0x%08X\n", GP);
    psp_boot_mark("EBOOT cargado");
    {   // game heap: the word at 0x08A785D0 is in KB (20350 in the original)
        const char* e = getenv("TTT_HEAP_KB");
        uint32_t kb = e ? (uint32_t)atoi(e) : 40960u;
        if (psp_l32(0x08A785D0u) == 0x4F7Eu) { psp_s32(0x08A785D0u, kb); printf("game heap: %u KB\n", kb); }
        else printf("game heap: palabra 0x08A785D0 distinta de la esperada, sin cambiar\n");
    }

    Ctx c;
    memset(&c, 0, sizeof(c));
    psp_vfpu_init(c);
    c.r[28] = GP;
    c.r[29] = STACK_SIZE - 0x400;
    c.r[31] = 0;
    c.r[4] = 0;
    c.r[5] = 0;

    Fn entry_pc = psp_resolve(0x08804040u);
    if (!entry_pc) { fprintf(stderr, "no encuentro module_start\n"); return 1; }

    t_begin = clock();
    limit_s = seconds_n;
    psp_gp(GP);
    psp_time_cap((double)seconds_n);
    std::thread(watcher_external, seconds_n + 3).detach();   // must come after reading --window
    signal(SIGSEGV, fatal_error);
    signal(SIGABRT, fatal_error);
#ifdef _WIN32
    crash_prev = SetUnhandledExceptionFilter(crash_filter);
#endif
    int reason = setjmp(exit_jmp);
    if (reason == 0) {
        psp_boot_mark("arranca el juego");
        entry_pc(c);
        printf("module_start retorno, $v0 = 0x%08X; arranca el planificador\n", c.r[2]);
        const char* end_pos = psp_schedule();
        end_reason = end_pos;
        printf("planificador terminado: %s\n", end_pos);
    } else if (reason == 1) {
        end_reason = "corte por tiempo";
        printf("cortado por tiempo (%d s)\n", seconds_n);
    } else {
        end_reason = "el juego pidio salir";
        printf("el juego pidio salir\n");
    }
    psp_stop_all();
    printf("cuadros esperados: %ld, cambios de contexto: %ld, tiempo virtual: %.2f s\n",
           cuadros, psp_switches(), psp_time_us() / 1000000.0);

    if (!report_done.exchange(true)) report();
    fflush(stdout);
    _Exit(0);
}
