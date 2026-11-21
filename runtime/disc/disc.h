#pragma once
// Virtual disc: serves the PACKFILE and the disc files from assets/ instead of the ISO.
// The manifest gives the index; when a file changes size everything is relocated and the index rebuilt.
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <chrono>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <functional>
#include "patches/load_smooth.h"
#include "patches/char_textures.h"
#include "patches/bt3_chars.h"
#include "patches/bt3_maps.h"
#include "disc/bt3.h"
#include "disc/bt3_stage.h"
#include <map>
#include <set>
#include "gpu/image.h"
#include "disc/mods.h"
#include "disc/roster_def.h"
#include "disc/stages_def.h"
#include "disc/fonttext.h"
void psp_boot_mark(const char* what);
static bool hc_get(char k, const std::string& real, std::vector<int64_t>& v);
static void hc_put(char k, const std::string& real, const std::vector<int64_t>& v);
static std::string hc_real(const std::string& fpath, bool base_only);
#include "disc/ags.h"
#include <array>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define DISC_SECTOR 2048u
#define KEY_SECTOR 0x4B636150u
#define KEY_ALT 0xAAAAAAAAu

struct DiscSpan { uint32_t lbn, sectors, sz; int type; int idx; };   // type 0 file, 1 index, 2 entry
struct Part { std::string fpath; uint32_t sz = 0, off = 0, freq = 0; int type = 0, id = 0, idx = 0; };   // type 0 file, 1 pak, 2 txt1, 3 vagp, 4 texture
struct Container { int type = 0; uint32_t sz = 0; int64_t date = 0; std::vector<Part> parts; };   // type 1 pak, 2 txt1, 3 vagp
struct PkEntry { uint32_t sector, sz, alt, cola, sector0, size0, real; std::string fpath, key; bool folder; };
#define ALIN_PAK 64u          // body start
#define ALIGN_ENTRY 64u      // entry stride, same as the game and tools/open_paks.py
#define TXT1_HDR 0x48u        // fixed txt1 header
struct DiscFile { std::string fpath, local; uint32_t lbn, sz; };

static std::string disc_dir;
static std::vector<PkEntry> disc_pk;
static std::vector<DiscFile> disc_files;
static std::vector<DiscSpan> disc_spans;
static std::vector<uint8_t> disc_index;
static uint32_t disc_pk_lbn = 0, disc_pk_sectors = 0, disc_pk_data = 112, disc_pk_size = 0;
static bool disc_ready = false, disc_relocated = false;
static int disc_verify_mode = 0;
static FILE* disc_cache[8];
static int disc_cache_idx[8];
static int disc_cache_n = 0;
static int rl_gen = 0;   // bumped by every Ctrl+R reload (disc/reload.h): function-local listing caches start over
static std::set<int> rl_watch_idx;   // reloaded entries: their next read by the game is logged
static std::mutex rl_watch_mx;
static std::set<int> rl_fresh;   // reloaded entries: built without the disk cache (it holds the texture ids of an older run)

static uint32_t sectors_of(uint32_t bytes) { return (bytes + DISC_SECTOR - 1) / DISC_SECTOR; }

// natural order, same as tools/open_paks.py: by component, numbers compared as numbers
static int natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == '/' || b[j] == '/') {
            if (a[i] != b[j]) return a[i] == '/' ? -1 : 1;
            i++; j++; continue;
        }
        bool da = isdigit((unsigned char)a[i]), db = isdigit((unsigned char)b[j]);
        if (da && db) {
            size_t ia = i, jb = j;
            while (ia < a.size() && isdigit((unsigned char)a[ia])) ia++;
            while (jb < b.size() && isdigit((unsigned char)b[jb])) jb++;
            size_t za = i, zb = j;
            while (za < ia - 1 && a[za] == '0') za++;
            while (zb < jb - 1 && b[zb] == '0') zb++;
            if (ia - za != jb - zb) return ia - za < jb - zb ? -1 : 1;
            int c = a.compare(za, ia - za, b, zb, jb - zb);
            if (c) return c < 0 ? -1 : 1;
            if (ia - i != jb - j) return ia - i < jb - j ? -1 : 1;
            i = ia; j = jb; continue;
        }
        if (da != db) return da ? -1 : 1;
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[j]);
        if (ca != cb) return ca < cb ? -1 : 1;
        i++; j++;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

static bool ignore_file(const std::string& name) {
    std::string n = name;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return n.empty() || n[0] == '.' || n == "thumbs.db" || n == "desktop.ini" || n == "_pak.txt" || n == "_cola.bin" || n == "_txt1.bin" || n == "texto.txt" || n == "_vagp.txt" || n == "_tex.txt" || n == "config.set" || n == "preview_txts.txt" ||
           n == "043_move_list_en.txt" || n == "color_name.txt" || n == "1_p_dmg" || n == "pre_cman" || n.compare(0, 17, "011_textures_dmg.") == 0 || n == "043_move_list_es.txt" || (n.size() > 4 && n.compare(n.size() - 4, 4, ".vfx") == 0);
}

static bool ends_with(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t k = 0; k < n; k++) if (tolower((unsigned char)s[s.size() - n + k]) != suffix[k]) return false;
    return true;
}
// French is not part of the port (tools/open_paks.py es_frances): only a _fr name part counts
static bool lang_dropped(const std::string& path) {
    std::string s = path;
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    for (size_t i = s.find("_fr"); i != std::string::npos; i = s.find("_fr", i + 1)) { char c = i + 3 < s.size() ? s[i + 3] : 0; if (!c || c == '.' || c == '/' || c == '_') return true; }
    return false;
}
static int container_type(const std::string& name) { return ends_with(name, ".pak") ? 1 : ends_with(name, ".txt1") ? 2 : ends_with(name, ".vagp") ? 3 : (ends_with(name, ".atex") || ends_with(name, ".rhg")) ? 4 : 0; }

// ---------------- mod layer ----------------
// every path under assets/Packfile is looked up first in the enabled mods, by priority; lowercase keys
static std::string mod_pk;
static std::map<std::string, std::string> mod_files;
static bool mod_enabled = false;
static std::vector<std::pair<std::string, std::string>> mod_active;   // enabled at start, priority order: id, version

static std::string lower(std::string s) { for (auto& c : s) { c = (char)tolower((unsigned char)c); if (c == '\\') c = '/'; } return s; }

static bool mod_rel(const std::string& fpath, std::string& rel) {
    if (!mod_enabled || fpath.size() < mod_pk.size() || lower(fpath.substr(0, mod_pk.size())) != mod_pk) return false;
    rel = lower(fpath.substr(mod_pk.size()));
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    return true;
}

static std::string mod_path(const std::string& fpath) {
    std::string rel;
    if (!mod_rel(fpath, rel)) return fpath;
    auto it = mod_files.find(rel);
    return it == mod_files.end() ? fpath : it->second;
}

static std::string link_real(const std::string& p);

static std::atomic<uint64_t> open_n(0), open_us(0);
// startup profile (TTT boot): time and calls per disc function, printed once after the scan
struct DProf { std::atomic<uint64_t> us{0}, n{0}; };
static DProf dprof[36];
static const char* const DPROF_N[36] = {"tex_base_size", "list_tex: cabeceras PNG", "fecha (last_write_time)", "listado de carpeta", "fechas de enlaces", "list_tree (recorrido)", "at3_register (wav)", "png_cabecera", "list_tex", "list_vagp", "list_txt1", "bt3_scan", "bt3_anm_scan", "bt3_eff_scan", "bt3_file_scan", "list_pak (con hijos)", "pak_folder (con hijos)", "at3_entry",
                                        "list_tex: bloque BT3", "bt3_data: carga de P", "mod_type: listado", "list_pak: list_tree sin cache", "dmg: is_dir", "dmg: list_container", "dmg: pak cerrado", "list_pak: contenedor tocado por mod", "list_tex: cabecera sin cache", "list_tex: claves BT3 en cache",
                                        "mods: lista de mods", "mods: recorrido Packfile", "mods: recorrido Shared", "mods: revision de archivos", "mods: voces", "dir_sig (firma de carpeta)", "listado desde memo", "listado adelantado (hilos)"};
struct DScope { int k; std::chrono::steady_clock::time_point t; DScope(int k_) : k(k_), t(std::chrono::steady_clock::now()) {} ~DScope() { dprof[k].us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count(); dprof[k].n++; } };
static void dprof_print(const char* phase) {
    for (int k = 0; k < 36; k++) if (dprof[k].n) printf("arranque: perfil %s: %s %llu veces, %llu ms\n", phase, DPROF_N[k], (unsigned long long)dprof[k].n, (unsigned long long)(dprof[k].us / 1000));
    printf("arranque: perfil %s: aperturas de archivo %llu, %llu ms\n", phase, (unsigned long long)open_n, (unsigned long long)(open_us / 1000));
    for (auto& d : dprof) { d.us = 0; d.n = 0; }
}
static FILE* open_file(const std::string& fpath, const char* modo, bool base_only = false) {
    auto t0 = std::chrono::steady_clock::now();
    std::string r = base_only ? fpath : mod_path(fpath);
    if (r == fpath) r = link_real(fpath);
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, r.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, 0), m(modo, modo + strlen(modo));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, r.c_str(), -1, &w[0], n);
    FILE* f = _wfopen(w.c_str(), m.c_str());
#else
    FILE* f = fopen(r.c_str(), modo);
#endif
    open_n++;
    open_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    return f;
}

// listing cache: assets/paks.cache stores each container's date and its parts with size and type
static std::map<std::string, Container> disc_cache_dir;
static bool disc_cache_dirty = false, disc_rescan = false;

static int64_t date_of(const std::filesystem::path& p) {
    DScope ds_(2);
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    return ec ? 0 : (int64_t)t.time_since_epoch().count();
}

static void disc_cache_read() {
    FILE* f = fopen((disc_dir + "/paks.cache").c_str(), "r");
    if (!f) return;
    char line[1200];
    Container* current = nullptr;
    bool v2 = false;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (!strcmp(line, "# v2")) { v2 = true; continue; }
        if (!v2) break;
        if (line[0] == '#') {
            char fpath[1024]; long long date = 0; int type = 1;
            if (sscanf(line, "# %1023[^\t]\t%lld\t%d", fpath, &date, &type) >= 2) { current = &disc_cache_dir[fpath]; current->date = date; current->type = type; current->parts.clear(); }
            continue;
        }
        char* tab = strchr(line, '\t');
        if (current && tab) {
            *tab = 0;
            char* tab2 = strchr(tab + 1, '\t');
            Part q; q.fpath = line; q.sz = (uint32_t)strtoul(tab + 1, nullptr, 10); q.type = tab2 ? atoi(tab2 + 1) : 0;
            current->parts.push_back(q);
        }
    }
    fclose(f);
}

static void disc_cache_write() {
    if (!disc_cache_dirty) return;
    FILE* f = fopen((disc_dir + "/paks.cache").c_str(), "w");
    if (!f) return;
    fprintf(f, "# v2\n");
    for (auto& kv : disc_cache_dir) {
        fprintf(f, "# %s\t%lld\t%d\n", kv.first.c_str(), (long long)kv.second.date, kv.second.type);
        for (auto& q : kv.second.parts) fprintf(f, "%s\t%u\t%d\n", q.fpath.c_str(), q.sz, q.type);
    }
    fclose(f);
    disc_cache_dirty = false;
}

// N-HEX.txt text to zero-terminated UTF-16 LE, same as tools/open_paks.py
static void utf8_to_utf16(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < n) {
        uint32_t c = p[i], extra = 0;
        if (c >= 0xF0) { c &= 7; extra = 3; } else if (c >= 0xE0) { c &= 15; extra = 2; } else if (c >= 0xC0) { c &= 31; extra = 1; }
        if (i + extra >= n + (extra ? 0 : 1)) { out.clear(); return; }
        for (uint32_t k = 1; k <= extra; k++) { if ((p[i + k] & 0xC0) != 0x80) { out.clear(); return; } c = (c << 6) | (p[i + k] & 0x3F); }
        i += extra + 1;
        if (c >= 0x10000) { c -= 0x10000; uint16_t a = 0xD800 | (c >> 10), b = 0xDC00 | (c & 0x3FF); out.push_back(a & 255); out.push_back(a >> 8); out.push_back(b & 255); out.push_back(b >> 8); }
        else { out.push_back(c & 255); out.push_back(c >> 8); }
    }
}

static void txt_decode(const std::vector<uint8_t>& d, std::vector<uint8_t>& out) {
    out.clear();
    size_t n = d.size();
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) out.assign(d.begin() + 2, d.end());
    else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { for (size_t k = 2; k + 1 < n; k += 2) { out.push_back(d[k + 1]); out.push_back(d[k]); } }
    else {
        size_t start = (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) ? 3 : 0;
        utf8_to_utf16(d.data() + start, n - start, out);
        if (out.empty() && n > start) for (size_t k = start; k < n; k++) { out.push_back(d[k]); out.push_back(0); }
    }
    size_t w = 0;
    for (size_t k = 0; k + 1 < out.size(); k += 2) if (!(out[k] == '\r' && out[k + 1] == 0)) { out[w++] = out[k]; out[w++] = out[k + 1]; }
    out.resize(w);
    if (w < 2 || out[w - 2] || out[w - 1]) { out.push_back(0); out.push_back(0); }
}

static bool read_file(const std::string& fpath, std::vector<uint8_t>& d, bool base_only = false) {
    FILE* f = open_file(fpath, "rb", base_only);
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d.resize(n > 0 ? n : 0);
    bool ok = n <= 0 || fread(d.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

// texto.txt: one UTF-8 file per txt1, each string follows a "[N]" line (N = position, from 1)
static void utf16_to_utf8(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    for (size_t k = 0; k + 1 < n; k += 2) {
        uint32_t c = p[k] | (p[k + 1] << 8);
        if (c >= 0xD800 && c < 0xDC00 && k + 3 < n) { uint32_t l = p[k + 2] | (p[k + 3] << 8); c = 0x10000 + ((c - 0xD800) << 10) + (l - 0xDC00); k += 2; }
        if (c < 0x80) out.push_back((uint8_t)c);
        else if (c < 0x800) { out.push_back((uint8_t)(0xC0 | (c >> 6))); out.push_back((uint8_t)(0x80 | (c & 63))); }
        else if (c < 0x10000) { out.push_back((uint8_t)(0xE0 | (c >> 12))); out.push_back((uint8_t)(0x80 | ((c >> 6) & 63))); out.push_back((uint8_t)(0x80 | (c & 63))); }
        else { out.push_back((uint8_t)(0xF0 | (c >> 18))); out.push_back((uint8_t)(0x80 | ((c >> 12) & 63))); out.push_back((uint8_t)(0x80 | ((c >> 6) & 63))); out.push_back((uint8_t)(0x80 | (c & 63))); }
    }
}

static bool txt1_split(const std::vector<uint8_t>& file, std::vector<std::vector<uint8_t>>& texts, std::string* error) {
    std::vector<uint8_t> t;
    size_t n = file.size();
    if (n >= 2 && file[0] == 0xFF && file[1] == 0xFE) utf16_to_utf8(file.data() + 2, n - 2, t);
    else t.assign(file.begin() + ((n >= 3 && file[0] == 0xEF && file[1] == 0xBB && file[2] == 0xBF) ? 3 : 0), file.end());
    size_t w = 0;
    for (size_t k = 0; k < t.size(); k++) if (t[k] != '\r') t[w++] = t[k];
    t.resize(w);
    texts.clear();
    std::string s(t.begin(), t.end()), marker = "[1]\n";
    if (s.compare(0, marker.size(), marker)) { if (error) *error = "no empieza por [1]"; return false; }
    size_t pos = marker.size();
    for (int id = 1;; id++) {
        marker = "\n[" + std::to_string(id + 1) + "]\n";
        size_t end_pos = s.find(marker, pos - 1);
        if (end_pos == std::string::npos) {
            size_t f = s.size();
            if (f > pos && s[f - 1] == '\n') f--;
            if (f < pos) f = pos;
            texts.emplace_back(s.begin() + pos, s.begin() + f);
            return true;
        }
        texts.emplace_back(s.begin() + pos, s.begin() + (end_pos < pos ? pos : end_pos));
        pos = end_pos + marker.size();
    }
}

static bool list_container(Container& c, const std::string& key, const std::string& folder, int type);
static void rl_start();

static std::map<std::string, Container> disc_containers;

// folder listing with size and date in one pass. On Windows std::filesystem makes two more
// queries per file for that; FindFirstFile already returns them, which shows with 200 000 files
struct DirEntry { std::string name; uint64_t sz; int64_t date; bool dir; };

#ifdef _WIN32
static std::wstring to_utf16(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
static void list_dir_raw(const std::string& folder, std::vector<DirEntry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((to_utf16(folder) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        DirEntry e;
        e.name = to_utf8(fd.cFileName);
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.date = ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#else
static void list_dir_raw(const std::string& folder, std::vector<DirEntry>& out) {
    std::error_code ec;
    for (auto& p : std::filesystem::directory_iterator(std::filesystem::u8path(folder), ec)) {
        DirEntry e;
        e.name = p.path().filename().u8string();
        e.dir = p.is_directory(ec);
        e.sz = e.dir ? 0 : (uint64_t)p.file_size(ec);
        e.date = (int64_t)p.last_write_time(ec).time_since_epoch().count();
        out.push_back(std::move(e));
    }
}
#endif
// startup scan: folder listings kept by lower-case path, the folders mods touch listed ahead on worker threads
static std::atomic<bool> ls_memo_on{false}, ls_stop{false};
static std::mutex ls_mx;
static std::unordered_map<std::string, std::vector<DirEntry>> ls_memo;
static std::vector<std::thread> ls_threads;
static void list_dir_os(const std::string& folder, std::vector<DirEntry>& out) {
    if (!ls_memo_on.load(std::memory_order_acquire)) { DScope ds_(3); list_dir_raw(folder, out); return; }
    std::string k = lower(folder);
    {
        std::lock_guard<std::mutex> g(ls_mx);
        auto it = ls_memo.find(k);
        if (it != ls_memo.end()) { out.insert(out.end(), it->second.begin(), it->second.end()); dprof[34].n++; return; }
    }
    std::vector<DirEntry> v;
    { DScope ds_(3); list_dir_raw(folder, v); }
    out.insert(out.end(), v.begin(), v.end());
    std::lock_guard<std::mutex> g(ls_mx);
    ls_memo.emplace(k, std::move(v));
}
static void ls_prefetch(std::vector<std::string> dirs) {
    const char* e = getenv("TTT_LS_PREFETCH");
    if (e && *e == '0') return;
    ls_stop = false;
    ls_memo_on = true;
    auto list = std::make_shared<std::vector<std::string>>(std::move(dirs));
    auto next = std::make_shared<std::atomic<size_t>>(0);
    unsigned hw = std::thread::hardware_concurrency(), nt = hw > 3 ? std::min(6u, hw - 2) : 1u;
    for (unsigned t = 0; t < nt; t++)
        ls_threads.emplace_back([list, next] {
            for (size_t i; !ls_stop && (i = (*next)++) < list->size();) {
                std::string k = lower((*list)[i]);
                { std::lock_guard<std::mutex> g(ls_mx); if (ls_memo.count(k)) continue; }
                std::vector<DirEntry> v;
                list_dir_raw((*list)[i], v);
                dprof[35].n++;
                std::lock_guard<std::mutex> g(ls_mx);
                ls_memo.emplace(k, std::move(v));
            }
        });
}
static void ls_end() {
    ls_stop = true;
    for (auto& t : ls_threads) t.join();
    ls_threads.clear();
    ls_memo_on = false;
    std::lock_guard<std::mutex> g(ls_mx);
    ls_memo.clear();
}

// shared assets: assets/shared.txt maps a Packfile path (or a path inside another shared copy) to one copy
// under assets/Shared. Whatever is present at the path wins; its absence means the shared copy
static std::unordered_map<std::string, std::string> link_to;
static std::vector<std::pair<std::string, std::string>> link_list;
static std::unordered_map<std::string, std::vector<DirEntry>> link_dirs;
static size_t link_root = 0;

static std::string link_key(const std::string& p) {
    std::string n = lower(p);
    while (!n.empty() && n.back() == '/') n.pop_back();
    return n;
}

static std::string link_real(const std::string& p) {
    if (link_to.empty()) return p;
    std::string r = p;
    for (int depth = 0; depth < 8; depth++) {
        std::string n = link_key(r);
        bool hit = false;
        for (size_t e = n.size(); e > link_root && e != std::string::npos; e = n.rfind('/', e - 1)) {
            auto it = link_to.find(n.substr(0, e));
            if (it != link_to.end()) { r = it->second + r.substr(e); hit = true; break; }
        }
        if (!hit) break;
    }
    return r;
}

static void link_add(std::vector<DirEntry>& out, const std::string& key) {
    auto it = link_dirs.find(key);
    if (it == link_dirs.end()) return;
    for (auto& v : it->second) {
        bool present = false;
        for (auto& e : out) if (lower(e.name) == lower(v.name)) { present = true; break; }
        if (!present) out.push_back(v);
    }
}

static void list_dir_fs(const std::string& folder, std::vector<DirEntry>& out) {
    std::string real = link_real(folder);
    list_dir_os(real, out);
    if (link_dirs.empty()) return;
    std::string k = link_key(folder), kr = link_key(real);
    link_add(out, k);
    if (kr != k) link_add(out, kr);
}

// date key of an opened pak: its own links plus those of every subfolder (an edited Shared file inside a section folder
// must invalidate the cached listing, or its old size is served). TTT_LINK_TREE=0 restores the direct links only
static size_t link_gen = 0;
static int64_t link_dates(const std::string& folder) {
    DScope ds_(4);
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_LINK_TREE"); on = !(e && *e == '0'); }
    static std::mutex mx;
    static std::unordered_map<std::string, int64_t> tree;
    static size_t built = (size_t)-1;
    std::lock_guard<std::mutex> g(mx);
    if (on && built != link_gen) {
        tree.clear();
        for (auto& kv : link_dirs) {
            int64_t d = 0;
            for (auto& v : kv.second) d += v.date + (int64_t)v.sz * 7919;
            std::string p = kv.first;
            for (;;) {
                tree[p] += d;
                size_t c = p.find_last_of('/');
                if (c == std::string::npos || c <= link_root) break;
                p.resize(c);
            }
        }
        built = link_gen;
    }
    int64_t d = 0;
    std::string k = link_key(folder), kr = link_key(link_real(folder));
    for (const std::string* key : {&k, &kr}) {
        if (on) { auto it = tree.find(*key); if (it != tree.end()) d += it->second; }
        else { auto it = link_dirs.find(*key); if (it != link_dirs.end()) for (auto& v : it->second) d += v.date + (int64_t)v.sz * 7919; }
        if (kr == k) break;
    }
    return d;
}

// one query for existence, kind, size and date (std::filesystem makes one per question)
static bool stat_one(const std::string& path, DirEntry& e) {
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(to_utf16(path).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    e.date = ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
    return true;
#else
    std::error_code ec;
    auto p = std::filesystem::u8path(path);
    if (!std::filesystem::exists(p, ec)) return false;
    e.dir = std::filesystem::is_directory(p, ec);
    e.sz = e.dir ? 0 : (uint64_t)std::filesystem::file_size(p, ec);
    e.date = date_of(p);
    return true;
#endif
}

static void links_load() {
    std::string pk = disc_dir + "/Packfile", sh = disc_dir + "/Shared";
    link_root = disc_dir.size();
    FILE* f = fopen((disc_dir + "/shared.txt").c_str(), "r");
    if (!f) return;
    char line[2048];
    unsigned n = 0, own = 0, missing = 0;
    while (fgets(line, sizeof(line), f)) {
        char* tab = strchr(line, '\t');
        if (line[0] == '#' || !tab) continue;
        *tab = 0;
        std::string rel = line, tgt = tab + 1;
        while (!tgt.empty() && (tgt.back() == '\n' || tgt.back() == '\r')) tgt.pop_back();
        std::string full = rel.compare(0, 7, "Shared/") == 0 ? disc_dir + "/" + rel : pk + "/" + rel, real = sh + "/" + tgt;
        DirEntry own_e, e;
        if (stat_one(full, own_e)) { own++; continue; }
        if (!stat_one(real, e)) { if (missing++ < 10) printf("shared: falta %s\n", tgt.c_str()); continue; }
        link_to[link_key(full)] = real;
        link_list.push_back({rel, tgt});
        size_t c = rel.find_last_of('/');
        e.name = c == std::string::npos ? rel : rel.substr(c + 1);
        if (e.dir) e.sz = 0;
        link_dirs[link_key(full.substr(0, full.find_last_of('/')))].push_back(e); link_gen++;
        n++;
    }
    fclose(f);
    printf("shared: %u enlaces, %u con copia propia, %u sin destino\n", n, own, missing);
}

// the port names Shared groups by the old layout (Menus/<category>/<name>); unify_assets now puts them under the menu's
// path or Common: the old name finds the group by its category and name
static std::unordered_map<std::string, std::string> sh_legacy;
std::string psp_shared_rel(const std::string& legacy) {
    DirEntry e;
    if (link_list.empty() || stat_one(disc_dir + "/Shared/" + legacy, e)) return legacy;
    if (sh_legacy.empty())
        for (auto& l : link_list) {
            std::vector<std::string> p;
            for (size_t a = 0;;) { size_t b = l.second.find('/', a); p.push_back(l.second.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
            if (p.size() < 3) continue;
            sh_legacy.emplace(lower(p[0] + "/" + p[p.size() - 2] + "/" + p.back()), l.second);
            sh_legacy.emplace(lower(p[0] + "/" + p[p.size() - 2]), l.second.substr(0, l.second.find_last_of('/')));
        }
    std::vector<size_t> cut;
    for (size_t i = 0; i < legacy.size(); i++) if (legacy[i] == '/') cut.push_back(i);
    cut.push_back(legacy.size());
    for (size_t n : {(size_t)3, (size_t)2}) {
        if (cut.size() < n) continue;
        auto it = sh_legacy.find(lower(legacy.substr(0, cut[n - 1])));
        if (it != sh_legacy.end()) return it->second + legacy.substr(cut[n - 1]);
    }
    return legacy;
}

// txt1 headers: assets/txt1.txt gives each txt1 its first id and the language slots it holds, in order
// (2 en, 3 fr, 4 es); the header is a table of count and running total per slot. "raw" keeps a header in hex
struct Txt1Info { uint16_t base = 0; std::vector<int> langs; std::vector<uint8_t> raw; };
static std::unordered_map<std::string, Txt1Info> txt1_info;

static void txt1_info_load() {
    FILE* f = fopen((disc_dir + "/txt1.txt").c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char* a = strchr(line, '\t');
        char* b = a ? strchr(a + 1, '\t') : nullptr;
        if (!b) continue;
        *a = 0; *b = 0;
        std::string rest = b + 1;
        while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
        Txt1Info t;
        if (!strcmp(a + 1, "raw")) {
            for (size_t k = 0; k + 1 < rest.size() && t.raw.size() < TXT1_HDR; k += 2) t.raw.push_back((uint8_t)strtoul(rest.substr(k, 2).c_str(), nullptr, 16));
            t.raw.resize(TXT1_HDR, 0);
        } else {
            t.base = (uint16_t)atoi(a + 1);
            size_t p = 0;
            while (p < rest.size()) {
                size_t q = rest.find(' ', p);
                std::string w = rest.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (w == "en") t.langs.push_back(2); else if (w == "fr") t.langs.push_back(3); else if (w == "es") t.langs.push_back(4);
                else if (!w.empty()) t.langs.push_back(atoi(w.c_str()));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        txt1_info[lower(line)] = t;
    }
    fclose(f);
}

static bool txt1_header_make(const std::string& key, uint32_t total, uint8_t* d) {
    auto it = txt1_info.find(lower(key));
    if (it == txt1_info.end()) return false;
    const Txt1Info& t = it->second;
    if (!t.raw.empty()) { memcpy(d, t.raw.data(), TXT1_HDR); return false; }
    uint16_t h[TXT1_HDR / 2] = {0};
    uint16_t n = (uint16_t)(total / (t.langs.empty() ? 1 : t.langs.size())), sum = 0;
    h[0] = 0x64; h[1] = t.base; h[2] = (uint16_t)(t.base + n - 1);
    for (int i = 0; i < 17; i++) {
        uint16_t c = std::find(t.langs.begin(), t.langs.end(), i) != t.langs.end() ? n : 0;
        sum = (uint16_t)(sum + c);
        if (3 + 2 * i < (int)(TXT1_HDR / 2)) h[3 + 2 * i] = c;
        if (4 + 2 * i < (int)(TXT1_HDR / 2)) h[4 + 2 * i] = sum;
    }
    for (unsigned k = 0; k < TXT1_HDR / 2; k++) { d[2 * k] = h[k] & 255; d[2 * k + 1] = h[k] >> 8; }
    return true;
}

static std::string tex_loose_name(const std::string& name, int* esc = nullptr);

// a mod shadows by name or, inside a container, by leading number (01.png/01@2x.png, 04_x.rhg/04_x.rhg.png)
static std::map<std::string, std::vector<DirEntry>> mod_dirs;
static std::unordered_set<std::string> mod_entries;
static std::mutex mod_mutex;

static bool mod_numbered(const std::string& rel) {
    size_t p = 0;
    for (;;) {
        size_t s = rel.find('/', p);
        std::string pre = rel.substr(0, s), comp = rel.substr(p, s == std::string::npos ? std::string::npos : s - p);
        if (!pre.empty() && (mod_entries.count(pre) || container_type(comp))) return true;
        if (s == std::string::npos) return false;
        p = s + 1;
    }
}

static std::string mod_key(const std::string& name, bool dir, bool num, bool txt1) {
    int n = 0; char hex[32];
    if ((!dir || lower(name).compare(0, 17, "011_textures_dmg.") == 0) && ignore_file(name)) return lower(name);   // port files (043_move_list_<lang>.txt) never take a part's number
    if (num && isdigit((unsigned char)name[0])) {
        if (txt1) { if (!dir && ends_with(name, ".txt") && sscanf(name.c_str(), "%d-%31[0-9A-Fa-f].", &n, hex) == 2) return "#" + std::to_string(n); }
        else if (!dir || container_type(name)) return "#" + std::to_string(atoi(name.c_str()));
    }
    if (!dir) { std::string t = tex_loose_name(name); if (!t.empty()) return lower(t); }
    return lower(name);
}

static void list_dir(const std::string& folder, std::vector<DirEntry>& out) {
    list_dir_fs(folder, out);
    std::string rel;
    if (!mod_rel(folder, rel)) return;
    auto it = mod_dirs.find(rel);
    if (it == mod_dirs.end() || it->second.empty()) return;
    bool num = mod_numbered(rel), t1 = ends_with(rel, ".txt1");
    std::set<std::string> covering;
    for (auto& e : it->second) covering.insert(mod_key(e.name, e.dir, num, t1));
    out.erase(std::remove_if(out.begin(), out.end(), [&](const DirEntry& e) { return covering.count(mod_key(e.name, e.dir, num, t1)) > 0; }), out.end());
    out.insert(out.end(), it->second.begin(), it->second.end());
}

// during the startup scan the listings (disc and mod) are kept per parent folder
static bool scan_memo = false;
static std::unordered_map<std::string, std::unordered_map<std::string, DirEntry>> memo_dirs, mod_memo;
static void memo_clear() { memo_dirs.clear(); mod_memo.clear(); }

// -1: no mod touches the folder, ask the disc; 0 missing; 1 file; 2 folder
static int mod_type(const std::string& fpath, DirEntry* outp = nullptr) {
    std::string rel;
    if (!mod_rel(fpath, rel)) return -1;
    size_t s = rel.find_last_of('/');
    if (!mod_dirs.count(s == std::string::npos ? std::string() : rel.substr(0, s))) return -1;
    size_t c = fpath.find_last_of("/\\");
    std::string parent = fpath.substr(0, c), name = lower(fpath.substr(c + 1));
    std::lock_guard<std::mutex> lock_(mod_mutex);
    if (scan_memo) {
        auto it = mod_memo.find(parent);
        if (it == mod_memo.end()) {
            DScope ds_(20);
            std::vector<DirEntry> inp;
            list_dir(parent, inp);
            auto& m = mod_memo[parent];
            for (auto& e : inp) m.emplace(lower(e.name), e);
            it = mod_memo.find(parent);
        }
        auto e = it->second.find(name);
        if (e == it->second.end()) return 0;
        if (outp) *outp = e->second;
        return e->second.dir ? 2 : 1;
    }
    static std::string last;
    static std::vector<DirEntry> inp;
    static int gen = 0;
    if (parent != last || gen != rl_gen) { DScope ds_(20); last = parent; gen = rl_gen; inp.clear(); list_dir(parent, inp); }
    for (auto& e : inp) if (lower(e.name) == name) { if (outp) *outp = e; return e.dir ? 2 : 1; }
    return 0;
}

// during the startup scan: one listing per parent folder instead of one query per path
static int memo_type(const std::string& fpath, DirEntry* outp) {
    size_t c = fpath.find_last_of("/\\");
    if (c == std::string::npos) return -1;
    std::string parent = fpath.substr(0, c);
    auto it = memo_dirs.find(parent);
    if (it == memo_dirs.end()) {
        std::vector<DirEntry> inp;
        list_dir_fs(parent, inp);
        auto& m = memo_dirs[parent];
        for (auto& e : inp) m[lower(e.name)] = e;
        it = memo_dirs.find(parent);
    }
    auto e = it->second.find(lower(fpath.substr(c + 1)));
    if (e == it->second.end()) return 0;
    if (outp) *outp = e->second;
    return e->second.dir ? 2 : 1;
}

static bool is_dir(const std::string& fpath) {
    int t = mod_type(fpath);
    if (t >= 0) return t == 2;
    if (scan_memo) { t = memo_type(fpath, nullptr); if (t >= 0) return t == 2; }
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::u8path(link_real(fpath)), ec);
}

static bool is_file(const std::string& fpath, uint64_t* sz = nullptr) {
    DirEntry e;
    int t = mod_type(fpath, &e);
    if (t < 0 && scan_memo) t = memo_type(fpath, &e);
    if (t >= 0) { if (t == 1 && sz) *sz = e.sz; return t == 1; }
    std::error_code ec;
    uintmax_t n = std::filesystem::file_size(std::filesystem::u8path(link_real(fpath)), ec);
    if (ec) return false;
    if (sz) *sz = n;
    return true;
}

// container with mod content inside: neither reads nor writes the asset caches
// damaged colour pak (item 30): the colour's container key + "#dmg", 1_p entries 2..16 from its 1_p_dmg folder
static bool dmg_key(const std::string& key) { return key.size() > 4 && key.compare(key.size() - 4, 4, "#dmg") == 0; }
static std::string dmg_strip(const std::string& key) { size_t p = key.find("#dmg"); return p == std::string::npos ? key : key.substr(0, p) + key.substr(p + 4); }
// character or map mod: any file under Maps/, or a non-texture file (or a damaged texture) in a character colour folder
static bool mod_is_game(const std::string& pk) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(std::filesystem::u8path(pk), ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string k = lower(std::filesystem::relative(it->path(), std::filesystem::u8path(pk), ec).generic_u8string());
        bool map = !k.compare(0, 5, "maps/");
        if (!map && (k.compare(0, 11, "characters/") || k.find("/color_") == std::string::npos)) continue;
        size_t d = k.find_last_of('.');
        std::string ext = d == std::string::npos ? "" : k.substr(d + 1);
        bool tex = ext == "png" || ext == "atex" || ext == "ctex" || ext == "dbt" || ext == "rhg" || ext == "dds" || ext == "tga" || ext == "bmp";
        if (map || !tex || k.find("_dmg") != std::string::npos) return true;
    }
    return false;
}

static bool mod_touches(const std::string& key) { return mod_enabled && mod_dirs.count(lower(dmg_strip(key))) > 0; }

// mods made before the AT3 rename: x_m_.wav stands for x.unk when assets already use the new name
static std::string mod_legacy_at3(const std::string& rel, const std::string& name) {
    std::string b = lower(name);
    if (b.size() < 8 || (!ends_with(b, "_m_.wav") && !ends_with(b, "_m_.unk"))) return name;
    std::string stem = name.substr(0, name.size() - 7), ext = name.substr(name.size() - 4);
    std::string entry = (rel.empty() ? "" : rel + "/") + lower(stem) + ".unk";
    return mod_entries.count(entry) ? stem + ext : name;
}

static void mod_walk(const std::string& real, const std::string& rel, const std::string& orig, int& n, int& covered, std::vector<std::string>& added) {
    std::vector<DirEntry> inp;
    list_dir_fs(real, inp);
    auto& list = mod_dirs[rel];
    bool num = mod_numbered(rel), t1 = ends_with(rel, ".txt1");
    std::unordered_map<std::string, size_t> keys;   // the folder's keys once, not per file
    for (size_t i = 0; i < list.size(); i++) keys.emplace(mod_key(list[i].name, list[i].dir, num, t1), i);
    for (auto& e : inp) {
        std::string on_disk = e.name;
        if (!e.dir) e.name = mod_legacy_at3(rel, e.name);
        std::string b = lower(e.name);
        if (b[0] == '.' || b == "thumbs.db" || b == "desktop.ini") continue;
        std::string k = mod_key(e.name, e.dir, num, t1);
        auto ki = keys.find(k);
        DirEntry prev;
        const DirEntry* already = nullptr;
        if (ki != keys.end()) { prev = list[ki->second]; already = &prev; }
        else keys.emplace(k, list.size());
        std::string r = rel.empty() ? b : rel + "/" + b, o = orig.empty() ? e.name : orig + "/" + e.name;
        if (e.dir) {
            if (already && !already->dir) { covered++; continue; }
            if (!already) list.push_back(e);
            mod_walk(real + "/" + e.name, r, o, n, covered, added);
            continue;
        }
        if (already) { covered++; continue; }
        list.push_back(e);
        mod_files[r] = real + "/" + on_disk;
        added.push_back(o);
        n++;
    }
}

// a mod's Shared/ file reaches every Packfile path whose shared link resolves to it (nested links included)
static void mod_shared_targets(const std::string& srel, int depth, std::vector<std::string>& out) {
    std::string ls = lower(srel);
    for (auto& l : link_list) {
        std::string lt = lower(l.second);
        if (ls != lt && ls.compare(0, lt.size() + 1, lt + "/") != 0) continue;
        std::string v = l.first + srel.substr(lt.size());
        if (v.compare(0, 7, "Shared/") == 0) { if (depth < 8) mod_shared_targets(v.substr(7), depth + 1, out); }
        else out.push_back(v);
    }
}
static void mod_add_virtual(const std::string& vpath, const DirEntry& f, const std::string& real, int& n, int& covered, std::vector<std::string>& added) {
    std::string rel;
    size_t a = 0;
    for (size_t b; (b = vpath.find('/', a)) != std::string::npos; a = b + 1) {
        std::string seg = vpath.substr(a, b - a);
        auto& list = mod_dirs[rel];
        bool num = mod_numbered(rel), t1 = ends_with(rel, ".txt1");
        std::string k = mod_key(seg, true, num, t1);
        const DirEntry* already = nullptr;
        for (auto& o : list) if (mod_key(o.name, o.dir, num, t1) == k) { already = &o; break; }
        if (already && !already->dir) { covered++; return; }
        if (!already) list.push_back({seg, 0, f.date, true});
        rel = rel.empty() ? lower(seg) : rel + "/" + lower(seg);
    }
    auto& list = mod_dirs[rel];
    bool num = mod_numbered(rel), t1 = ends_with(rel, ".txt1");
    DirEntry e = f;
    e.name = mod_legacy_at3(rel, vpath.substr(a));
    std::string k = mod_key(e.name, false, num, t1);
    for (auto& o : list) if (mod_key(o.name, o.dir, num, t1) == k) { covered++; return; }
    list.push_back(e);
    mod_files[rel.empty() ? lower(e.name) : rel + "/" + lower(e.name)] = real;
    added.push_back(vpath);
    n++;
}
static void mod_walk_shared(const std::string& real, const std::string& srel, int& n, int& covered, int& unused, std::vector<std::string>& added) {
    std::vector<DirEntry> inp;
    list_dir_fs(real, inp);
    for (auto& e : inp) {
        std::string b = lower(e.name);
        if (b[0] == '.' || b == "thumbs.db" || b == "desktop.ini") continue;
        std::string r = srel.empty() ? e.name : srel + "/" + e.name;
        if (e.dir) { mod_walk_shared(real + "/" + e.name, r, n, covered, unused, added); continue; }
        std::vector<std::string> vs;
        mod_shared_targets(r, 0, vs);
        if (vs.empty() && unused++ < 20) printf("mods: Shared/%s no lo usa ningun menu (sin enlace en shared.txt)\n", r.c_str());
        for (auto& v : vs) mod_add_virtual(v, e, real + "/" + e.name, n, covered, added);
    }
}

// mod files the game would never read: outside every entry, or in a folder assets does not have
static void mod_check(const std::string& id, const std::vector<std::string>& added) {
    DScope ds_(31);
    int warnings = 0;
    std::unordered_map<std::string, bool> dir_of;
    for (auto& o : added) {
        std::string r = lower(o), unk = r;
        size_t pu = unk.find_last_of("./");
        if (pu != std::string::npos && unk[pu] == '.') unk = unk.substr(0, pu) + ".unk";
        bool reaches = mod_entries.count(r) || mod_entries.count(unk);
        for (size_t p = r.find('/'); !reaches && p != std::string::npos; p = r.find('/', p + 1)) reaches = mod_entries.count(r.substr(0, p)) > 0;
        const char* reason = nullptr;
        if (!reaches) reason = "no esta dentro de ninguna entrada";
        else {
            size_t c = o.find_last_of('/');
            std::string parent = c == std::string::npos ? "" : o.substr(0, c), last_one = parent.substr(parent.find_last_of('/') + 1);
            if (!parent.empty() && !mod_entries.count(lower(parent)) && !container_type(last_one)) {
                auto d = dir_of.find(parent);
                if (d == dir_of.end()) { std::error_code ec; d = dir_of.emplace(parent, std::filesystem::is_directory(std::filesystem::u8path(link_real(disc_dir + "/Packfile/" + parent)), ec)).first; }
                if (!d->second) reason = "su carpeta no existe en assets";
            }
        }
        if (reason) for (auto& c : roster::defs().chars) if (!c.folder.empty() && r.compare(0, c.folder.size() + 1, lower(c.folder) + "/") == 0) reason = nullptr;   // a new character's folder
        if (reason) { size_t q = r.find("/color_"); int j = q == std::string::npos ? 0 : atoi(r.c_str() + q + 7); if (j >= 5 && j <= 10) reason = nullptr; }   // colours 5..10
        if (reason) for (auto& m : stages::defs().maps) for (const std::string& f : {"maps/" + lower(m.id) + "/", "maps/sfx/" + lower(m.id) + "/"}) if (r.compare(0, f.size(), f) == 0) reason = nullptr;
        if (reason && r.compare(0, 7, "planet/") == 0) reason = nullptr;   // planet destruction transition packs (item 71)
        if (reason && r.compare(0, 15, "menus/bg_chars/") == 0) reason = nullptr;   // main menu flying characters (item 72)
        if (reason && warnings++ < 20) printf("mods: %s: %s %s\n", id.c_str(), o.c_str(), reason);
    }
    if (warnings > 20) printf("mods: %s: %d avisos mas\n", id.c_str(), warnings - 20);
}

void psp_mods_info(std::string& root, std::vector<std::string>& ids) {
    root = mods_root_global;
    ids.clear();
    for (auto& m : mod_active) ids.push_back(m.first);
}

// select screen images of new characters (item 29.3): mods/<id>/Roster/<BT3 id>/small.png, mid.png, name.png
// (name_en/es/fr.png), big.png. The PNGs join the shared RHGs as new textures, the character select AGS get a
// state per new number (a copy of number 0 on the new texture) and the big render gets pak entries of its own
static bool png_cabecera(const std::string& fpath, int* w, int* h, int* colors);
static bool roster_png(const std::string& path, int w, int h, const std::vector<uint32_t>& px);
static bool roster_blank_png(const std::string& path, int w, int h);
static std::map<int, std::array<uint32_t, 3>> ro_big;   // menu number -> select AGS, room AGS, RHG (1-based pak entries)

static void ro_force(const std::string& srel, const std::string& src, int& n) {   // a port file over Shared srel, above any mod
    std::vector<std::string> vs;
    mod_shared_targets(srel, 0, vs);
    std::string real = link_real(src);   // a Shared file can be a link: mod files are read without resolving links
    std::error_code ec;
    uint64_t sz = std::filesystem::file_size(std::filesystem::u8path(real), ec);
    if (ec) { printf("roster: no se encuentra %s\n", real.c_str()); return; }
    int64_t date = date_of(std::filesystem::u8path(real));
    for (auto& v : vs) {
        std::string rel = lower(v);
        auto it = mod_files.find(rel);
        if (it == mod_files.end()) {
            int covered = 0; std::vector<std::string> added;
            mod_add_virtual(v, DirEntry{v.substr(v.find_last_of('/') + 1), sz, date, false}, real, n, covered, added);
            continue;
        }
        it->second = real;
        size_t c = rel.find_last_of('/');
        for (auto& e : mod_dirs[rel.substr(0, c)]) if (lower(e.name) == rel.substr(c + 1)) { e.sz = sz; e.date = date; }
    }
}

static int ro_count_png(const std::string& dir) {
    std::vector<DirEntry> inp;
    list_dir_fs(dir, inp);
    int n = 0;
    for (auto& e : inp) if (!e.dir && isdigit((unsigned char)e.name[0]) && ends_with(lower(e.name), ".png")) n = std::max(n, atoi(e.name.c_str()));
    return n;
}

// select labels (names 025, form labels 029/031) per language: own PNG (name/form_right/form_left[_lang].png in
// Roster/<ID or N>/), another character's label (name_image, form_label...), empty, or for a transformation without
// any the name of its base; resolved to the state each number copies and the textures it adds
struct RoLabel { int src = -1; std::string img; };   // src: state to copy, -1 default, -3 empty
static int ro_menu(int s) { return roster::vanilla(s) ? roster::rank_vanilla(s) : roster::menu_of(s); }
static std::string ro_label_img(const std::string& root_d, int s, int kind, int l) {
    static const char* B[3] = {"name", "form_right", "form_left"}, *L[2] = {"en", "es"};
    std::vector<std::string> dirs;
    if (const roster::Char* c = roster::find(s)) { if (!c->id.empty()) dirs.push_back(root_d + "/" + c->mod + "/Roster/" + c->id + "/"); dirs.push_back(root_d + "/" + c->mod + "/Roster/" + std::to_string(s) + "/"); }
    else if (const roster::Rename* r = roster::renamed(s)) dirs.push_back(root_d + "/" + r->mod + "/Roster/" + std::to_string(s) + "/");
    for (const std::string& n : {std::string(B[kind]) + "_" + L[l == 1] + ".png", std::string(B[kind]) + ".png"})
        for (auto& dd : dirs) if (is_file(dd + n)) return dd + n;
    return "";
}
// a label written with the app's font (names as text): kind 0 name (256x22 at 4x), 1 form label of RHG 030, 2 of RHG 032 (at 1x,
// rewritten at 4x when served, disc/text_names.h); one file per text, so equal texts share one texture or cell
static std::map<std::string, std::pair<int, std::string>> tn_textpng;   // file -> kind, text
struct TnRect { int kind, l; float r[4]; std::string text; };
static std::vector<TnRect> tn_rects;   // form labels placed in the atlases' free space
static const float FORM_SIZE = 0.85f;   // transformation texts: the font's size made to the original labels'
// the transformation row (forms.txt) each original number shows: its state in the original AGS 029 (rows of 14 of RHG 030)
int psp_form_row(int menu) {
    static std::vector<int> rows;
    static bool ok = false;
    if (!ok) {
        ok = true;
        std::vector<uint8_t> d; std::vector<ags::State> st;
        std::string f = link_real(disc_dir + "/Shared/" + psp_shared_rel("Menus/char_select/char_select_en.pak") + "/1_interface/029_char_forms_right.ags");
        if (read_file(f, d, true) && ags::parse(d, st)) {
            int off = ags::off_state(d, st);
            for (int i = 0; i < roster::K_OLD && i < (int)st.size(); i++) {
                int t, tw, th; float r[4];
                rows.push_back(i != off && ags::rect(d, st[i], &t, &tw, &th, r) && th > 0 ? (int)std::lround(r[1] * 256.0f / th / 14.0f) : -1);
            }
        }
        printf("roster: %d filas de transformacion de los originales leidas\n", (int)rows.size());
    }
    return menu >= 0 && menu < (int)rows.size() ? rows[menu] : -1;
}
int psp_form_menu(int row) { for (int m = 0; m < roster::K_OLD; m++) if (psp_form_row(m) == row) return m; return -1; }
static std::string ro_text_png(int kind, int l, const std::string& text) {
    if (text.empty()) return "";
    static const char* ST[3] = {"char_name", "char_form/left", "char_form/right"};
    static ft::Font f[3] = {ft::load(std::string("../../games/ttt/fonts/") + ST[0]), ft::load(std::string("../../games/ttt/fonts/") + ST[1]), ft::load(std::string("../../games/ttt/fonts/") + ST[2])};
    f[1].size = f[2].size = FORM_SIZE;
    if (!f[kind].ok) return "";
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(kind * 7 + l);
    for (unsigned char ch : text) { h ^= ch; h *= 1099511628211ull; }
    char nm[64]; snprintf(nm, sizeof nm, "/cache/roster/text/%d_%016llx.png", kind, (unsigned long long)h);
    std::string out = disc_dir + nm;
    if (!is_file(out)) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache/roster/text"), ec);
        ft::Img t = kind == 0 ? ft::compose(f[0], text, 256, 22, 4, -1, 5) : ft::compose(f[kind], text, kind == 1 ? 256 : 200, 14, 1, -1, kind == 1 ? 2.0f : 3.0f);
        if (!t.w || !roster_png(out, t.w, t.h, t.px)) return "";
    }
    tn_textpng[out] = {kind, text};
    return out;
}
static RoLabel ro_label(const std::string& root_d, int s, int kind, int l, int depth = 0) {
    RoLabel r;
    if (depth > 8 || s < 0) return r;
    const roster::Char* c = roster::find(s);
    const roster::Rename* rn = c ? nullptr : roster::renamed(s);
    if (kind > 0 && l < 2) {   // a transformation given as text (names as text): an original row is that row's state, a new text its own label
        int fid = c ? c->form : rn ? rn->form : -2;
        if (fid == -1) { r.src = -3; return r; }
        if (fid >= 0 && fid < roster::FORM_NEW) { int m = psp_form_menu(fid); if (m >= 0) { r.src = m; return r; } }
        if (fid >= roster::FORM_NEW) { r.img = ro_text_png(kind, l, roster::form_text(fid, l)); if (!r.img.empty()) return r; }
    }
    r.img = ro_label_img(root_d, s, kind, l);
    if (!r.img.empty()) return r;
    const roster::Labels* lb = c ? &c->lab : rn ? &rn->lab : nullptr;
    int ref = lb ? (lb->ref[kind][l] != -2 ? lb->ref[kind][l] : lb->ref[kind][3]) : -2;
    if (kind == 0 && c && ref == -2 && l < 2 && !c->name[l].empty()) { r.img = ro_text_png(0, l, c->name[l]); if (!r.img.empty()) return r; }   // a new character's name as text, not a copy of another's
    if (ref == -1) { r.src = -3; return r; }
    int from = ref >= 0 ? ref : kind == 0 && c && c->parent >= 0 ? c->parent : -1;
    if (from < 0) return r;
    RoLabel t = ro_label(root_d, from, kind, l, depth + 1);
    if (t.src == -1 && t.img.empty() && roster::vanilla(from)) t.src = ro_menu(from);
    return t;
}

// one descriptor (kind 0 names, 1 form right, 2 form left) of one language pak: states copied and textures added
struct RoAtlas { int w = 0, h = 0; std::vector<uint32_t> px; std::vector<uint8_t> used; std::string out; bool dirty = false; };
static std::map<std::string, RoAtlas> ro_atlases;   // loose label RHGs: written once with every label placed
static void ro_atlas_flush() { for (auto& e : ro_atlases) if (e.second.dirty) { e.second.dirty = false; roster_png(e.second.out, e.second.w, e.second.h, e.second.px); } }
static void ro_labels(const std::string& root_d, const std::string& prel, const std::string& lang, int kind, const std::vector<uint8_t>& in, int count,
                      std::map<int, int>& srcm, std::map<int, std::vector<int>>& tex, int& files) {
    static std::map<std::string, int> next;   // textures already added per RHG (form RHGs can be shared by the languages)
    static std::map<std::string, std::array<int, 3>> made;   // RHG + PNG -> texture, size
    auto& d = roster::defs();
    std::vector<ags::State> S;
    if (!ags::parse(in, S)) return;
    int n = (int)S.size(), on = ags::on_state(in, S), off = ags::off_state(in, S), l = lang == "en" ? 0 : lang == "es" ? 1 : 2;
    std::string sh = disc_dir + "/Shared/", rhg;
    if (kind == 0) rhg = psp_shared_rel("Menus/char_select/char_names_" + lang + ".rhg");
    else {
        std::vector<DirEntry> inp;
        list_dir_fs(sh + prel + "/1_interface", inp);
        int num = kind == 1 ? 30 : 32;
        for (auto& e : inp) if (isdigit((unsigned char)e.name[0]) && atoi(e.name.c_str()) == num && (ends_with(lower(e.name), ".rhg") || ends_with(lower(e.name), ".rhg.png"))) rhg = prel + "/1_interface/" + e.name;
        std::string real = link_real(sh + rhg);
        if (!rhg.empty() && real.compare(0, sh.size(), sh) == 0) rhg = real.substr(sh.size());
    }
    // a one-texture RHG (loose .rhg.png, the form labels): new labels are drawn into its free space, in the cell size
    auto& atlases = ro_atlases;
    static std::map<std::string, std::array<int, 4>> placed;   // RHG + PNG -> rectangle in texture units
    bool loose = ends_with(lower(rhg), ".rhg.png");
    int tt = 0, tw = 0, th = 0; float cr[4] = {0, 0, 0, 0};
    if (loose && (on < 0 || !ags::rect(in, S[on], &tt, &tw, &th, cr))) loose = false, rhg.clear();
    auto place = [&](const std::string& img) -> const std::array<int, 4>* {
        auto key = rhg + "|" + img;
        auto pit = placed.find(key);
        if (pit != placed.end()) return &pit->second;
        RoAtlas& a = atlases[rhg];
        if (a.px.empty()) {
            std::vector<uint8_t> f; std::string err;
            if (!read_file(link_real(sh + rhg), f, true) || !png_decode(f, &a.w, &a.h, a.px, &err)) { printf("roster: no se pudo leer %s\n", rhg.c_str()); return nullptr; }
            a.used.assign((size_t)a.w * a.h, 0);
            for (int y = 0; y < a.h; y++) for (int x = 0; x < a.w; x++) if (a.px[(size_t)y * a.w + x] >> 24)
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int X = x + dx, Y = y + dy; if (X >= 0 && Y >= 0 && X < a.w && Y < a.h) a.used[(size_t)Y * a.w + X] = 1; }
            a.out = disc_dir + "/cache/roster/" + lang + "_" + rhg.substr(rhg.find_last_of('/') + 1);
        }
        double sc = (double)a.w / tw;
        int sw = std::max(1, (int)((cr[2] - cr[0]) * sc + 0.5)), shh = std::max(1, (int)((cr[3] - cr[1]) * sc + 0.5));
        int ix = -1, iy = -1;
        auto freebox = [&](int x0, int y0) { for (int y = y0; y < y0 + shh; y++) for (int x = x0; x < x0 + sw; x++) if (a.used[(size_t)y * a.w + x]) return false; return true; };
        for (int y = 0; y + shh <= a.h && ix < 0; y++) for (int x0 : {0, a.w - sw}) if (x0 >= 0 && freebox(x0, y)) { ix = x0; iy = y; break; }
        if (ix < 0) { printf("roster: %s no tiene espacio libre para otra etiqueta\n", rhg.c_str()); return nullptr; }
        std::vector<uint8_t> f; std::vector<uint32_t> ip; int iw = 0, ih = 0; std::string err;
        if (!read_file(img, f, true) || !png_decode(f, &iw, &ih, ip, &err)) { printf("roster: %s no es un png valido\n", img.c_str()); return nullptr; }
        double k = std::min((double)sw / iw, (double)shh / ih);
        int dw = std::max(1, (int)(iw * k)), dh = std::max(1, (int)(ih * k));
        for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) a.px[(size_t)(iy + y) * a.w + ix + x] = ip[(size_t)(y * ih / dh) * iw + x * iw / dw];
        for (int y = iy; y < iy + shh; y++) for (int x = ix; x < ix + sw; x++) a.used[(size_t)y * a.w + x] = 1;
        if (!is_file(a.out)) roster_png(a.out, a.w, a.h, a.px);
        a.dirty = true;
        ro_force(rhg, a.out, files);
        std::array<int, 4> rc = {(int)(ix / sc + 0.5), (int)(iy / sc + 0.5), (int)((ix + sw) / sc + 0.5), (int)((iy + shh) / sc + 0.5)};
        auto tp = tn_textpng.find(img);
        if (tp != tn_textpng.end()) tn_rects.push_back({tp->second.first, l, {(float)rc[0], (float)rc[1], (float)rc[2], (float)rc[3]}, tp->second.second});
        return &placed.emplace(key, rc).first->second;
    };
    std::vector<int> subj;
    for (auto& c : d.chars) subj.push_back(c.sysid);
    for (auto& r : d.renames) subj.push_back(r.sysid);
    int shared = 0;
    for (int s : subj) {
        int m = ro_menu(s);
        if (m < 0 || m >= std::max(count, n)) continue;
        RoLabel c = ro_label(root_d, s, kind, l);
        if (!c.img.empty()) {
            if (rhg.empty() || on < 0) { printf("roster: sin textura para la etiqueta %d de %s\n", kind, prel.c_str()); continue; }
            if (loose) {
                if (const std::array<int, 4>* rc = place(c.img)) { tex[m] = {tt, tw, th, (*rc)[0], (*rc)[1], (*rc)[2], (*rc)[3]}; srcm[m] = on; }
                continue;
            }
            auto key = rhg + "|" + c.img;
            auto it = made.find(key);
            if (it == made.end()) {
                int w = 0, h = 0, cols = 0;
                if (!png_cabecera(c.img, &w, &h, &cols)) { printf("roster: %s no es un png valido\n", c.img.c_str()); continue; }
                if (!next.count(rhg)) next[rhg] = ro_count_png(sh + rhg);
                int t = next[rhg]++, x = kind == 0 && w % 256 == 0 && h % 22 == 0 && w / 256 == h / 22 && w / 256 > 1 ? w / 256 : 1;
                char nm[32];
                if (x > 1) { snprintf(nm, sizeof(nm), "/%02d@%dx.png", t + 1, x); w /= x; h /= x; }
                else snprintf(nm, sizeof(nm), "/%02d.png", t + 1);
                ro_force(rhg + nm, c.img, files);
                it = made.emplace(key, std::array<int, 3>{t, w, h}).first;
            } else shared++;
            tex[m] = {it->second[0], it->second[1], it->second[2]};
            srcm[m] = on;
        } else if (c.src == -3) { if (off >= 0) srcm[m] = off; }
        else if (c.src >= 0 && c.src < n && c.src != m) srcm[m] = c.src;
    }
}

// the character select layout for the 3D view (item 33, TTT_SELECT_3D at startup): his AGS of the interface entries
// (games/ttt/select3d/layout/NNN.ags by entry number, NNN.rhg/<texture>.png extra or changed textures) over the en/es char_select paks
static bool sel3d_layout = false;
bool psp_sel_layout3d() { return sel3d_layout; }
static bool sel_wanted() {
    const char* e = getenv("TTT_SELECT_3D");
    const char* o = getenv("TTT_SEL3D_LAYOUT");
    return e && atoi(e) > 0 && !(o && !atoi(o));
}
static void sel_layout() {
    if (!sel_wanted()) return;
    std::string src = "../../games/ttt/select3d/layout/", sh = disc_dir + "/Shared/";
    int files = 0, n = 0;
    for (const char* pk : {"char_select_en.pak", "char_select_es.pak"}) {
        std::string prel = psp_shared_rel(std::string("Menus/char_select/") + pk) + "/1_interface";
        std::vector<DirEntry> in;
        list_dir(sh + prel, in);
        int seen = 0, have = 0;
        for (auto& it : in) {
            const std::string& nm = it.name;
            if (it.dir && nm.size() > 4 && isdigit((unsigned char)nm[0]) && ends_with(lower(nm), ".rhg")) {   // texture folder of an RHG
                char v[16]; snprintf(v, sizeof v, "%03d.rhg", atoi(nm.c_str()));
                std::vector<DirEntry> tx;
                list_dir(src + v, tx);
                for (auto& t : tx) if (!t.dir && ends_with(lower(t.name), ".png")) { ro_force(prel + "/" + nm + "/" + t.name, src + v + "/" + t.name, files); have++; }
                continue;
            }
            if (it.dir || nm.size() < 4 || !isdigit((unsigned char)nm[0]) || !ends_with(lower(nm), ".ags")) continue;
            seen++;
            char v[16]; snprintf(v, sizeof v, "%03d.ags", atoi(nm.c_str()));
            if (!is_file(src + v)) continue;
            ro_force(prel + "/" + nm, src + v, files);
            have++;
        }
        printf("seleccion 3D: %s: %d entradas en la carpeta, %d AGS de la interfaz, %d del diseno puestos\n", prel.c_str(), (int)in.size(), seen, have);
        n += have;
    }
    sel3d_layout = n > 0;
    printf("seleccion 3D: diseno de la vista 3D, %d archivos sobre los paks en/es%s\n", n, is_dir(src) ? "" : " (no se encuentra games/ttt/select3d/layout)"); fflush(stdout);
}

// a Shared group by its stem: unify_assets adds a language suffix when only one language menu holds it (char_renders_small_dr.rhg)
static std::string sh_name(const std::string& sh, const std::string& rel) {
    if (is_dir(sh + rel) || is_file(sh + rel)) return rel;
    size_t sl = rel.find_last_of('/'), dot = rel.find_last_of('.');
    if (sl == std::string::npos || dot == std::string::npos || dot < sl) return rel;
    std::string dir = rel.substr(0, sl + 1), stem = lower(rel.substr(sl + 1, dot - sl - 1)) + "_", ext = lower(rel.substr(dot));
    std::vector<DirEntry> inp;
    list_dir_fs(sh + dir, inp);
    for (auto& e : inp) { std::string n = lower(e.name); if (n.compare(0, stem.size(), stem) == 0 && ends_with(n, ext.c_str())) { printf("roster: %s esta como %s\n", rel.c_str(), e.name.c_str()); return dir + e.name; } }
    return rel;
}
static void roster_assets(const std::string& root_d) {
    auto& d = roster::defs();
    if (d.chars.empty() && d.renames.empty()) return;
    std::vector<const roster::Char*> cs;
    for (auto& c : d.chars) cs.push_back(&c);
    std::sort(cs.begin(), cs.end(), [](const roster::Char* a, const roster::Char* b) { return a->sysid < b->sysid; });
    auto img = [&](const roster::Char* c, const std::string& name) {
        for (const std::string& sub : {c->id, std::to_string(c->sysid)}) {
            if (sub.empty()) continue;
            std::string f = root_d + "/" + c->mod + "/Roster/" + sub + "/" + name;
            if (is_file(f)) return f;
        }
        return std::string();
    };
    std::string sh = disc_dir + "/Shared/";
    std::string cache = disc_dir + "/cache/roster";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(cache), ec);
    int files = 0;
    // the five per-number descriptors (mid, small, name, both form labels) keep the same state count (his rule: a
    // shorter one crashes the game); images retarget the new states of the first three, the rest copy number 0
    struct Kind { const char* png; const char* ags; };
    static const Kind K[5] = {{"small", "017_char_renders_small.ags"}, {"mid", "015_char_renders_mid.ags"}, {"name", "025_char_names.ags"},
                              {nullptr, "029_char_forms_right.ags"}, {nullptr, "031_char_forms_left.ags"}};
    static const char* PAKS[2][2] = {{"char_select_en.pak", "en"}, {"char_select_es.pak", "es"}};
    int count = 0;
    for (auto* c : cs) count = std::max(count, roster::menu_of(c->sysid) + 1);
    for (auto& pk : PAKS) {
        std::string prel = psp_shared_rel(std::string("Menus/char_select/") + pk[0]);
        if (!is_dir(sh + prel)) continue;
        for (int k = 0; k < 5; k++) {
            std::map<int, std::vector<int>> tex;
            std::map<int, int> srcm;
            if (k < 2 && K[k].png) {
                std::string rhg = sh_name(sh, psp_shared_rel(k == 0 ? "Menus/char_select/char_renders_small.rhg" : k == 1 ? "Menus/char_renders_mid/char_renders_mid.rhg" : std::string("Menus/char_select/char_names_") + pk[1] + ".rhg"));
                int base = ro_count_png(sh + rhg), j = 0;
                if (!base) printf("roster: falta %s\n", rhg.c_str());
                for (auto* c : cs) {
                    if (!base) break;
                    std::string f = k == 2 ? img(c, std::string("name_") + pk[1] + ".png") : std::string();
                    if (f.empty()) f = img(c, std::string(K[k].png) + ".png");
                    if (f.empty()) continue;
                    int w = 0, h = 0, cols = 0;
                    if (!png_cabecera(f, &w, &h, &cols)) { printf("roster: %s no es un png valido\n", f.c_str()); continue; }
                    static const int SW[3] = {46, 68, 256}, SH[3] = {30, 48, 22};
                    int x = w % SW[k] == 0 && h % SH[k] == 0 && w / SW[k] == h / SH[k] && w / SW[k] > 1 ? w / SW[k] : 1;
                    char nm[32];
                    if (x > 1) { snprintf(nm, sizeof(nm), "/%02d@%dx.png", base + 1 + j, x); w /= x; h /= x; }
                    else snprintf(nm, sizeof(nm), "/%02d.png", base + 1 + j);
                    ro_force(rhg + nm, f, files);
                    tex[roster::menu_of(c->sysid)] = {base + j, w, h};
                    j++;
                }
            }
            std::string arel = prel + "/1_interface/" + K[k].ags, src;
            std::vector<std::string> vs;
            mod_shared_targets(arel, 0, vs);
            if (!vs.empty()) { auto it = mod_files.find(lower(vs[0])); if (it != mod_files.end()) src = it->second; }
            if (src.empty()) src = sh + arel;
            std::vector<uint8_t> in, out;
            if (!read_file(src, in, true)) { printf("roster: no se pudo leer %s\n", arel.c_str()); continue; }
            if (k >= 2) { ro_labels(root_d, prel, pk[1], k - 2, in, count, srcm, tex, files); ro_atlas_flush(); }
            if (!ags::remap(in, count, srcm, tex, out)) { printf("roster: no se pudo ampliar %s\n", arel.c_str()); continue; }
            std::string dst = cache + "/" + pk[1] + "_" + K[k].ags;
            std::vector<uint8_t> old;
            if (!read_file(dst, old, true) || old != out) { FILE* f = fopen(dst.c_str(), "wb"); if (f) { fwrite(out.data(), 1, out.size(), f); fclose(f); } }
            ro_force(arel, dst, files);
            std::vector<ags::State> st;
            ags::parse(in, st);
            printf("roster: %s: %s de %d a %d estados, %d imagenes nuevas, %d estados copiados de otro\n", pk[0], K[k].ags, (int)st.size(), std::max(count, (int)st.size()), (int)tex.size(), (int)std::count_if(srcm.begin(), srcm.end(), [&](const std::pair<const int, int>& e) { return !tex.count(e.first); }));
        }
    }
    // big render: select AGS and room AGS copied from Goku's (041, 112) and the render as a loose RHG, after the last entry
    std::string rr = psp_shared_rel("Menus/char_select/renders"), ra = psp_shared_rel("Menus/char_select/renders_ags"), sel0, room0;
    int top = 0;
    for (const std::string& dd : {sh + rr, sh + ra, sh + psp_shared_rel("Menus/char_select/char_select_es.pak") + "/1_interface"}) {
        std::vector<DirEntry> inp;
        list_dir_fs(dd, inp);
        for (auto& e : inp) {
            if (!isdigit((unsigned char)e.name[0])) continue;
            int v = atoi(e.name.c_str());
            top = std::max(top, v);
            if (dd == sh + ra && v == 41) sel0 = dd + "/" + e.name;
            if (dd == sh + ra && v == 112) room0 = dd + "/" + e.name;
        }
    }
    if (sel0.empty() || room0.empty()) { printf("roster: faltan 041.ags o 112.ags en %s\n", ra.c_str()); return; }
    for (auto* c : cs) {
        std::string f = img(c, "big.png");
        if (f.empty()) continue;
        uint32_t a = (uint32_t)top + 1;
        char n1[64], n2[64], n3[64];
        snprintf(n1, sizeof(n1), "/%03u_new_%d.ags", a, c->sysid);
        snprintf(n2, sizeof(n2), "/%03u_new_%d_room.ags", a + 1, c->sysid);
        snprintf(n3, sizeof(n3), "/%03u_render_new_%d.rhg.png", a + 2, c->sysid);
        ro_force(rr + n1, sel0, files);
        ro_force(rr + n2, room0, files);
        ro_force(rr + n3, f, files);
        ro_big[roster::menu_of(c->sysid)] = {a, a + 1, a + 2};
        top += 3;
    }
    printf("roster: %d archivos de seleccion para %d personajes nuevos, %d renders grandes\n", files, (int)cs.size(), (int)ro_big.size());
}

// big render entry (1-based) of a mod character in the char_select pak, 0 none
int psp_cr_big(int sysid) {
    if (sysid < 0) return 0;
    auto it = ro_big.find(roster::menu_of(sysid));
    return it == ro_big.end() ? 0 : (int)it->second[2];
}

// map select images of new maps: mods/<id>/Stages/<ID>/big.png 300x152, small.png 64x44, name[_en|_es|_fr].png 256x22
// (@Nx accepted). Their states go after every original state (Random and the rest stay where they are): big render
// 16 + j, thumbnail 18 + j, name 12 + j; one new texture per map (names: one atlas per 11 maps, same number in every language)
int st_small_base = -1;   // first texture of the new maps' thumbnails in map_selector_bg.rhg (generated map renders)
int st_names_base = -1;   // first texture of the new maps' name atlases in 08_map_names_bg.rhg
static void stage_assets(const std::string& root_d) {
    auto& sd = stages::defs();
    if (sd.maps.empty()) return;
    std::string sh = disc_dir + "/Shared/", cache = disc_dir + "/cache/stages", dir = psp_shared_rel("Menus/map_select") + "/";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(cache), ec);
    int files = 0, nm = (int)sd.maps.size();
    auto img = [&](const stages::Map& m, const std::string& name) {
        std::string f = root_d + "/" + m.mod + "/Stages/" + m.id + "/" + name;
        return is_file(f) ? f : std::string();
    };
    auto add_png = [&](const std::string& rhg, int t, const std::string& f, int sw, int shh, int* w, int* h) {
        int cols = 0;
        if (f.empty() || !png_cabecera(f, w, h, &cols)) {
            std::string blank = cache + "/blank_" + std::to_string(sw) + "x" + std::to_string(shh) + ".png";
            roster_blank_png(blank, sw, shh);
            if (!f.empty()) printf("mapas: %s no es un png valido\n", f.c_str());
            *w = sw; *h = shh;
            char n[32]; snprintf(n, sizeof(n), "/%02d.png", t + 1);
            ro_force(rhg + n, blank, files);
            return;
        }
        int x = *w % sw == 0 && *h % shh == 0 && *w / sw == *h / shh && *w / sw > 1 ? *w / sw : 1;
        char n[32];
        if (x > 1) { snprintf(n, sizeof(n), "/%02d@%dx.png", t + 1, x); *w /= x; *h /= x; }
        else snprintf(n, sizeof(n), "/%02d.png", t + 1);
        ro_force(rhg + n, f, files);
    };
    struct Kind { const char* ags; const char* rhg; const char* png; int sw, sh, tpl, first; };
    static const Kind K[3] = {{"map_renders_big.ags", "map_renders_big.rhg", "big", 300, 152, 5, 16},
                              {"map_selector_bg.ags", "map_selector_bg.rhg", "small", 64, 44, 6, 18},
                              {"map_names_bg.ags", nullptr, "name", 256, 22, 1, 12}};
    static const char* PAKS[2][2] = {{"map_select_en.pak", "en"}, {"map_select_es.pak", "es"}};
    for (int k = 0; k < 3; k++) {
        std::map<int, std::vector<int>> tex;
        std::map<int, int> srcm;
        std::vector<std::string> rhgs;
        if (K[k].rhg) rhgs.push_back(dir + K[k].rhg);
        else for (auto& pk : PAKS) if (is_dir(sh + dir + pk[0])) rhgs.push_back(dir + pk[0] + "/08_map_names_bg.rhg");
        int base = -1;
        for (size_t r = 0; r < rhgs.size(); r++) {
            int b = ro_count_png(sh + rhgs[r]);
            if (!b) { printf("mapas: falta %s\n", rhgs[r].c_str()); continue; }
            if (base >= 0 && b != base) printf("mapas: %s tiene %d texturas y otro idioma %d\n", rhgs[r].c_str(), b, base);
            base = std::max(base, b);
        }
        if (base < 0) continue;
        if (k == 1) st_small_base = base;
        if (k == 2) st_names_base = base;
        if (k == 2) {   // names as the game has them: 256x256 atlases of 11 rows of 22, text right aligned at x 254, 218 wide at most
            for (size_t r = 0; r < rhgs.size(); r++) {
                std::string lang = PAKS[r][1];
                for (int a = 0; a * 11 < nm; a++) {
                    std::vector<std::vector<uint32_t>> ims; std::vector<std::array<int, 2>> sz;
                    int S = 1;
                    for (int j = a * 11; j < std::min(nm, a * 11 + 11); j++) {
                        std::string f;
                        for (const std::string& c : {"name_" + lang + ".png", std::string("name.png"), std::string("name_en.png")}) if (f.empty()) f = img(sd.maps[j], c);
                        std::vector<uint8_t> fb; std::vector<uint32_t> px; int w = 0, h = 0; std::string err;
                        if (!f.empty() && !(read_file(f, fb, true) && png_decode(fb, &w, &h, px, &err))) { printf("mapas: %s no es un png valido\n", f.c_str()); w = h = 0; px.clear(); }
                        if (w && h) S = std::max(S, std::min(4, std::max(1, (int)((double)h / 22 + 0.5))));
                        ims.push_back(px); sz.push_back({w, h});
                    }
                    int W = 256 * S, RH = 22 * S;
                    std::vector<uint32_t> at((size_t)W * W, 0);
                    for (size_t q = 0; q < ims.size(); q++) {
                        int w = sz[q][0], h = sz[q][1];
                        if (!w || !h) {   // no name image: its name written with the app's font (the id when it has no name)
                            static ft::Font fnt = ft::load("../../games/ttt/fonts/map_name");
                            ft::Img t = ft::compose(fnt, stages::name_of(stages::OLD + a * 11 + (int)q, lang == "es"), 256, 22, S, -1, 4, 218);
                            for (int y = 0; y < t.h; y++) for (int x = 0; x < t.w; x++) at[(size_t)(22 * S * (int)q + y) * W + x] = t.px[(size_t)y * t.w + x];
                            continue;
                        }
                        int x0 = w, x1 = -1;
                        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (ims[q][(size_t)y * w + x] >> 24) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
                        if (x1 < 0) continue;
                        double sc = (double)RH / h;
                        int cw = x1 - x0 + 1, dw = std::min(218 * S, std::max(1, (int)(cw * sc + 0.5)));   // the bar's clouds cover x < 37 (widest original name)
                        sc = std::min(sc, (double)dw / cw);
                        int dh = std::max(1, (int)(h * sc + 0.5)), dx = W - 2 * S - dw, dy = 22 * S * (int)q + (RH - dh) / 2;
                        for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
                            int sx = x0 + std::min(cw - 1, (int)(x / sc)), sy = std::min(h - 1, (int)(y / sc));
                            at[(size_t)(dy + y) * W + dx + x] = ims[q][(size_t)sy * w + sx];
                        }
                    }
                    std::string out = cache + "/" + lang + "_names_" + std::to_string(a) + ".png";
                    roster_png(out, W, W, at);
                    char n[32];
                    if (S > 1) snprintf(n, sizeof(n), "/%02d@%dx.png", base + a + 1, S); else snprintf(n, sizeof(n), "/%02d.png", base + a + 1);
                    ro_force(rhgs[r] + n, out, files);
                }
            }
            for (int j = 0; j < nm; j++) { int row = j % 11; tex[K[k].first + j] = {base + j / 11, 256, 256, 0, 22 * row, 256, 22 * row + 22}; srcm[K[k].first + j] = K[k].tpl; }
        }
        for (size_t r = 0; r < rhgs.size() && k < 2; r++) {
            std::string lang = K[k].rhg ? "" : PAKS[r][1];
            for (int j = 0; j < nm; j++) {
                const stages::Map& m = sd.maps[j];
                std::string f;
                if (!lang.empty()) for (const std::string& c : {"name_" + lang + ".png", std::string("name.png"), std::string("name_en.png")}) if (f.empty()) f = img(m, c);
                if (f.empty() && lang.empty()) f = img(m, std::string(K[k].png) + ".png");
                int w = 0, h = 0;
                add_png(rhgs[r], base + j, f, K[k].sw, K[k].sh, &w, &h);
                if (!r) tex[K[k].first + j] = k ? std::vector<int>{base + j, w, h} : std::vector<int>{base + j, w, h, -1};
                if (!r) srcm[K[k].first + j] = K[k].tpl;
            }
        }
        std::string arel = dir + K[k].ags, src;
        std::vector<std::string> vs;
        mod_shared_targets(arel, 0, vs);
        if (vs.empty()) { printf("mapas: Shared/%s no lo usa ningun menu (sin enlace en shared.txt)\n", arel.c_str()); continue; }
        auto mf = mod_files.find(lower(vs[0]));
        if (mf != mod_files.end()) src = mf->second;
        if (src.empty()) src = sh + arel;
        std::vector<uint8_t> in, out;
        if (!read_file(src, in, true)) { printf("mapas: no se pudo leer %s\n", arel.c_str()); continue; }
        std::vector<ags::State> st;
        ags::parse(in, st);
        if ((int)st.size() != K[k].first) { printf("mapas: %s tiene %d estados (se esperaban %d), no se amplia\n", arel.c_str(), (int)st.size(), K[k].first); continue; }
        if (!ags::remap(in, K[k].first + nm, srcm, tex, out)) { printf("mapas: no se pudo ampliar %s\n", arel.c_str()); continue; }
        std::string dst = cache + "/" + K[k].ags;
        std::vector<uint8_t> old;
        if (!read_file(dst, old, true) || old != out) { FILE* f = fopen(dst.c_str(), "wb"); if (f) { fwrite(out.data(), 1, out.size(), f); fclose(f); } }
        ro_force(arel, dst, files);
        printf("mapas: %s de %d a %d estados, textura nueva desde %d\n", K[k].ags, (int)st.size(), K[k].first + nm, base);
    }
    printf("mapas: %d archivos de seleccion para %d mapas nuevos\n", files, nm);
}

// big render PNG of a character for the SZ portrait: a mod character's Roster big.png, else the select's RHG 184+n (a mod's Shared copy first)
std::string psp_cr_file(const std::string& rel);
std::string psp_big_png(int sysid) {
    if (sysid < 0 || sysid >= roster::ALL) return "";
    if (!roster::vanilla(sysid)) {
        const roster::Char* c = roster::find(sysid);
        if (!c) return "";
        for (const std::string& sub : {c->id, std::to_string(c->sysid)}) {
            std::string f = mods_root_global + "/" + c->mod + "/Roster/" + sub + "/big.png";
            if (!sub.empty() && is_file(f)) return f;
        }
        return "";
    }
    int m = roster::rank_vanilla(sysid);
    if (m < 0) return "";
    std::string rel = psp_shared_rel("Menus/char_select/renders");
    std::vector<DirEntry> v;
    list_dir_fs(disc_dir + "/Shared/" + rel, v);
    char pre[8];
    snprintf(pre, sizeof pre, "%03d_", 184 + m);
    for (auto& e : v) {
        if (e.name.compare(0, 4, pre) || e.dir) continue;
        std::string f = psp_cr_file("Shared/" + rel + "/" + e.name);
        return f.empty() ? link_real(disc_dir + "/Shared/" + rel + "/" + e.name) : f;
    }
    return "";
}

bool psp_roster_big(int menu, uint32_t* sel, uint32_t* room, uint32_t* rhg) {
    auto it = ro_big.find(menu);
    if (it == ro_big.end()) return false;
    *sel = it->second[0]; *room = it->second[1]; *rhg = it->second[2];
    return true;
}

// voice packs (item 32): an enabled mod whose mod.txt has voces=<English>|<Spanish> names a voice language; the wavs and paks
// under its Lang/<id>/Packfile (and any mod's Lang/<id>/Packfile) are alternatives of the game's entries, never an overlay; its Packfile is a normal overlay
struct VoicePack { std::string id, name[3], code; std::unordered_map<std::string, std::string> wav, raw; };   // raw: whole entries (SFX paks)
static std::vector<VoicePack> voice_packs;
static std::string voice_wanted;
std::atomic<int> psp_voice_lang{0};   // 0 the game's own voices, k = voice_packs[k - 1]
static void voice_walk(const std::string& real, const std::string& rel, VoicePack& v, int& other) {
    std::vector<DirEntry> inp;
    list_dir_fs(real, inp);
    for (auto& e : inp) {
        std::string r = rel.empty() ? e.name : rel + "/" + e.name;
        if (e.dir) { voice_walk(real + "/" + e.name, r, v, other); continue; }
        if (ends_with(e.name, ".wav")) v.wav[lower(r)] = real + "/" + e.name;
        else if (ends_with(e.name, ".pak")) v.raw[lower(r)] = real + "/" + e.name;
        else if (!ignore_file(e.name)) other++;
    }
}
static void voice_resolve() {
    int k = 0;
    for (size_t i = 0; i < voice_packs.size(); i++) if (lower(voice_packs[i].id) == lower(voice_wanted)) k = (int)i + 1;
    psp_voice_lang = k;
}
void psp_voice_want(const std::string& id) { voice_wanted = id; voice_resolve(); }
std::string psp_voice_current() { int k = psp_voice_lang; return k > 0 && k <= (int)voice_packs.size() ? voice_packs[k - 1].id : std::string(); }
// names of the voice packs (lang 0 English, 1 Spanish) and their mod ids
int psp_voice_packs(std::vector<std::string>* names, std::vector<std::string>* ids, int lang) {
    for (auto& v : voice_packs) { if (names) names->push_back(v.name[lang < 0 || lang > 2 ? 0 : lang]); if (ids) ids->push_back(v.id); }
    return (int)voice_packs.size();
}
// short code of each voice pack (mod.txt codigo=, else the id after its last '_'), for buttons
int psp_voice_codes(std::vector<std::string>* codes) {
    for (auto& v : voice_packs) codes->push_back(v.code);
    return (int)voice_packs.size();
}

static void mods_init() {
    mod_active.clear();
    voice_packs.clear();
    std::string root_d = mods_root_of(disc_dir);
    mods_root_global = root_d;
    const char* e = getenv("TTT_MODS");
    if (e && !strcmp(e, "0")) return;
    std::vector<ModInfo> list;
    { DScope ds_(28); list = mods_list(root_d); }
    if (list.empty() && !sel_wanted()) return;   // the 3D view's select layout rides on the same overlay, with or without mods
    mod_pk = lower(disc_dir + "/Packfile/");
    for (auto& x : disc_pk) {
        std::string r = lower(x.fpath);
        mod_entries.insert(r);
        if (ends_with(r, ".pak")) mod_entries.insert(r.substr(0, r.size() - 4));
    }
    int active_list = 0;
    if (const char* sk = getenv("TTT_MODS_SKIP"))   // tests: "cosmetic" loads only the character and map mods of this instance
        for (auto& m : list)
            if (m.enabled && !strcmp(sk, "cosmetic") && !mod_is_game(root_d + "/" + m.id + "/Packfile")) { m.enabled = false; printf("mods: %s omitido en esta instancia (estetico)\n", m.id.c_str()); }
    for (auto& m : list) {
        if (!m.enabled) continue;
        auto ver = m.data_.find("version");
        auto vo = m.data_.find("voces");
        if (vo != m.data_.end() && !vo->second.empty()) {
            VoicePack v; v.id = m.id;
            auto co = m.data_.find("codigo");
            v.code = co != m.data_.end() && !co->second.empty() ? co->second : m.id.substr(m.id.find_last_of('_') == std::string::npos ? 0 : m.id.find_last_of('_') + 1);
            for (auto& ch : v.code) ch = (char)toupper((unsigned char)ch);
            if (v.code.size() > 5) v.code.resize(5);
            std::string t = vo->second;
            for (int l = 0; l < 3; l++) { size_t b = t.find('|'); v.name[l] = t.substr(0, b); t = b == std::string::npos ? v.name[l] : t.substr(b + 1); }
            for (int l = 1; l < 3; l++) if (v.name[l].empty()) v.name[l] = v.name[0];
            int other = 0;
            { DScope ds_(32); voice_walk(root_d + "/" + m.id + "/Lang/" + m.id + "/Packfile", "", v, other); }
            printf("mods: %s: voces %s, %u audios, %u paks%s\n", m.id.c_str(), v.name[0].c_str(), (unsigned)v.wav.size(), (unsigned)v.raw.size(), other ? (", " + std::to_string(other) + " archivos ignorados").c_str() : "");
            voice_packs.push_back(v);
            if (patch_mod_on("voice_lang") > 0 && voice_packs.size() == 1) printf("mods: %s: fila de idioma de voces activada\n", m.id.c_str());
        }
        auto pa = m.data_.find("parches");
        if (pa != m.data_.end()) {
            std::string t = pa->second;
            for (char& ch : t) if (ch == ',' || ch == ';' || ch == '|') ch = ' ';
            size_t i = 0;
            while (i < t.size()) {
                while (i < t.size() && t[i] == ' ') i++;
                size_t j = t.find(' ', i);
                std::string p = t.substr(i, j == std::string::npos ? std::string::npos : j - i);
                i = j == std::string::npos ? t.size() : j;
                if (p.empty()) continue;
                int r = patch_mod_on(p);
                printf("mods: %s: parche %s%s\n", m.id.c_str(), p.c_str(), r > 0 ? "" : r < 0 ? " (no activable por mods)" : " (desconocido)");
            }
        }
        mod_active.push_back({m.id, ver == m.data_.end() ? "" : ver->second});
        int n = 0, covered = 0;
        std::vector<std::string> added;
        { DScope ds_(29); mod_walk(root_d + "/" + m.id + "/Packfile", "", "", n, covered, added); }
        int unused = 0, shared_n = n;
        { DScope ds_(30); mod_walk_shared(root_d + "/" + m.id + "/Shared", "", n, covered, unused, added); }
        if (n > shared_n) printf("mods: %s: %d archivos desde Shared aplicados en sus menus\n", m.id.c_str(), n - shared_n);
        active_list++;
        if (covered) printf("mods: %d %s, %d archivos, %d tapados por mods de mas prioridad\n", active_list, m.id.c_str(), n, covered);
        else printf("mods: %d %s, %d archivos\n", active_list, m.id.c_str(), n);
        mod_check(m.id, added);
    }
    for (auto& m : list) {   // a mod's own voices in a voice pack's language: Lang/<pack id>/Packfile/...
        if (!m.enabled) continue;
        for (auto& v : voice_packs) {
            if (v.id == m.id) continue;
            std::string d = root_d + "/" + m.id + "/Lang/" + v.id + "/Packfile";
            if (!is_dir(d)) continue;
            size_t w0 = v.wav.size(), r0 = v.raw.size();
            int other = 0;
            { DScope ds_(32); voice_walk(d, "", v, other); }
            printf("mods: %s: %u audios y %u paks en %s\n", m.id.c_str(), (unsigned)(v.wav.size() - w0), (unsigned)(v.raw.size() - r0), v.name[0].c_str());
        }
    }
    psp_boot_mark("mods: carpetas recorridas");
    sel_layout();   // before the roster: its per-number AGS (015, 017...) are widened from the layout's when it has them
    roster_assets(root_d);
    psp_boot_mark("mods: seleccion de personajes");
    stage_assets(root_d);
    psp_boot_mark("mods: seleccion de mapas");
    mod_enabled = !mod_files.empty();
    voice_resolve();
    printf("mods: %d activos de %d instalados, %u archivos en uso\n", active_list, (unsigned)list.size(), (unsigned)mod_files.size());
}

// walks section subfolders; container folders are returned as entries and not entered
static void list_tree(const std::string& base, const std::string& rel, std::vector<Part>& parts) {
    DScope ds_(5);
    std::vector<DirEntry> inp;
    list_dir(rel.empty() ? base : base + "/" + rel, inp);
    for (auto& e : inp) {
        std::string r = rel.empty() ? e.name : rel + "/" + e.name;
        if (ignore_file(e.name)) continue;
        if (e.dir) {
            int t = container_type(e.name);
            if (t) { Part q; q.fpath = r; q.type = t; parts.push_back(q); }
            else list_tree(base, r, parts);
            continue;
        }
        std::string loose = tex_loose_name(e.name);
        if (!loose.empty()) { Part q; q.fpath = rel.empty() ? loose : rel + "/" + loose; q.type = 4; parts.push_back(q); continue; }
        Part q; q.fpath = r; q.sz = (uint32_t)e.sz;
        parts.push_back(q);
    }
}

// if every file has its own leading number, that number gives the order and the section is just a subfolder;
// if sections restart the numbering, the full path decides
static void sort_parts(std::vector<Part>& parts) {
    auto base = [](const std::string& r) { size_t s = r.find_last_of('/'); return s == std::string::npos ? r : r.substr(s + 1); };
    std::set<int> seen;
    bool by_name = true;
    for (auto& q : parts) {
        std::string b = base(q.fpath);
        if (!isdigit((unsigned char)b[0]) || !seen.insert(atoi(b.c_str())).second) { by_name = false; break; }
    }
    auto tpal = [](const std::string& r) { return r.size() > 5 && r.compare(r.size() - 5, 5, ".tpal") == 0; };   // toon palettes go last
    std::sort(parts.begin(), parts.end(), [&](const Part& a, const Part& b) {
        bool ta = tpal(a.fpath), tb = tpal(b.fpath);
        if (ta != tb) return tb;
        if (by_name) { int c = natural_less(base(a.fpath), base(b.fpath)); if (c) return c < 0; }
        return natural_less(a.fpath, b.fpath) < 0;
    });
}

// lookup without insert: building runs on several threads and operator[] would modify the map
static const Container* container_of(const std::string& key) {
    auto it = disc_containers.find(key);
    return it == disc_containers.end() ? nullptr : &it->second;
}

// ---------------- sounds exposed as wav ----------------
// Each region of a .vagp blob carries a 16-byte marker with its id; the sound runtime
// recognizes it wherever the game copied the data and plays the wav instead of the ADPCM.
static const uint8_t SFX_MARKER[8] = {0x00, 0x07, 'T', 'T', 'T', 'W', 'A', 'V'};

struct SfxWav {
    std::string fpath, name, pak;
    uint32_t freq = 0, length = 0;   // rate and ADPCM bytes declared by the original PPVA table
    int rate = 0, n = 0, loop_start = -1, loop_end = -1, state = 0, markers = -1;
    std::vector<int16_t> pcm;
};
static std::vector<SfxWav> sfx_reg;
static std::mutex sfx_mutex;

static uint32_t sfx_marker_id(const uint8_t* d) {
    if (memcmp(d, SFX_MARKER, 8)) return 0;
    uint32_t id;
    memcpy(&id, d + 8, 4);
    return id <= sfx_reg.size() ? id : 0;
}

static int32_t sfx_sample(const uint8_t* p, int bits, int float_v) {
    if (float_v) { float f; memcpy(&f, p, 4); return (int32_t)(f * 32767.0f); }
    if (bits == 8) return ((int32_t)p[0] - 128) << 8;
    if (bits == 16) return (int16_t)(p[0] | (p[1] << 8));
    if (bits == 24) return ((int32_t)(int8_t)p[2] << 8) | p[1];
    int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
    return v >> 16;
}

static bool sfx_load(SfxWav& w, bool base_only = false) {
    std::vector<uint8_t> d;
    if (!read_file(w.fpath, d, base_only) || d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return false;
    int channels = 0, bits = 0, float_v = 0;
    size_t data_ = 0, length = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t n;
        memcpy(&n, &d[p + 4], 4);
        size_t body = p + 8;
        if (body + n > d.size()) n = (uint32_t)(d.size() - body);
        if (!memcmp(&d[p], "fmt ", 4) && n >= 16) {
            uint16_t fmt, chans, bps;
            uint32_t sr;
            memcpy(&fmt, &d[body], 2); memcpy(&chans, &d[body + 2], 2);
            memcpy(&sr, &d[body + 4], 4); memcpy(&bps, &d[body + 14], 2);
            channels = chans; bits = bps; w.rate = (int)sr; float_v = (fmt == 3);
        } else if (!memcmp(&d[p], "data", 4)) {
            data_ = body; length = n;
        } else if (!memcmp(&d[p], "smpl", 4) && n >= 0x3C) {
            uint32_t loops;
            memcpy(&loops, &d[body + 28], 4);
            if (loops) {
                uint32_t a, b;
                memcpy(&a, &d[body + 44], 4); memcpy(&b, &d[body + 48], 4);
                w.loop_start = (int)a; w.loop_end = (int)b + 1;
            }
        }
        p = body + n + (n & 1);
    }
    if (channels < 1 || channels > 8 || !w.rate || !data_ || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return false;
    int step = bits / 8;
    int frames = (int)(length / (size_t)(step * channels));
    w.pcm.resize(frames);
    for (int i = 0; i < frames; i++) {
        int32_t sum = 0;
        for (int ch = 0; ch < channels; ch++) sum += sfx_sample(&d[data_ + (size_t)(i * channels + ch) * step], bits, float_v);
        sum /= channels;
        w.pcm[i] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
    }
    w.n = frames;
    if (w.loop_end > w.n) w.loop_end = w.n;
    return true;
}

extern bool psp_stall_on;
void psp_stall_note(int k, uint64_t us, uint32_t size);
void psp_stall_file(const char* name);
static uint64_t disc_us() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// BT3 stage sounds (item 65): Packfile/Sounds/bt3_water/<id decimal>_*.wav in a mod, read on first use
bool psp_bt3_snd_pcm(int id, const int16_t** pcm, int* n, int* rate) {
    static std::mutex m;
    static std::map<int, SfxWav> c;
    std::lock_guard<std::mutex> g(m);
    auto it = c.find(id);
    if (it == c.end()) {
        SfxWav w;
        char b[48]; snprintf(b, sizeof b, "sounds/bt3_water/%02d_", id & 0xFF);   // his extractor names: <id decimal, base 0>_<label>.wav
        std::string pre = b, r;
        for (auto& kv : mod_files)
            if (kv.first.size() > pre.size() + 4 && !kv.first.compare(0, pre.size(), pre) && ends_with(kv.first, ".wav")) { r = kv.second; break; }
        w.fpath = r;
        bool ok = !r.empty() && sfx_load(w, true);
        if (!ok) w.n = 0;
        printf("sonido: BT3 0x%02X (%02d_) %s\n", id & 0xFF, id & 0xFF, ok ? "cargado del mod" : "no esta en los mods");
        fflush(stdout);
        it = c.emplace(id, std::move(w)).first;
    }
    if (!it->second.n) return false;
    *pcm = it->second.pcm.data(); *n = it->second.n; *rate = it->second.rate;
    return true;
}

// the battle common bank's teleport sound for the select 3D view (sound 0x20 of bank 0), from the assets (or a mod), read on first use
bool psp_tp_snd_pcm(const int16_t** pcm, int* n, int* rate) {
    static std::mutex m;
    static SfxWav w;
    static int st = 0;
    std::lock_guard<std::mutex> g(m);
    if (!st) {
        w.fpath = disc_dir + "/Packfile/Battle_UI/sfx_battle/1_sfx.vagp/7_Movement_Flight_Teleportation/32_teleportation.wav";
        st = sfx_load(w) ? 1 : -1;
        printf("sonido: teletransporte de la seleccion %s (%s)\n", st > 0 ? "cargado" : "no encontrado", w.fpath.c_str()); fflush(stdout);
    }
    if (st < 0 || !w.n) return false;
    *pcm = w.pcm.data(); *n = w.n; *rate = w.rate;
    return true;
}

static SfxWav* sfx_wav(int id) {
    if (id < 1 || id > (int)sfx_reg.size()) return nullptr;
    SfxWav& w = sfx_reg[id - 1];
    if (!__atomic_load_n(&w.state, __ATOMIC_ACQUIRE)) {
        std::lock_guard<std::mutex> lock_(sfx_mutex);
        if (!w.state) {
            uint64_t t0 = psp_stall_on ? disc_us() : 0;
            int st = sfx_load(w) ? 1 : -1;
            if (st < 0 && mod_path(w.fpath) != w.fpath) {
                printf("sound: el wav del mod %s no es valido, uso el de assets\n", mod_path(w.fpath).c_str());
                st = sfx_load(w, true) ? 1 : -1;
            }
            __atomic_store_n(&w.state, st, __ATOMIC_RELEASE);
            if (t0) psp_stall_note(6, disc_us() - t0, 0);
            if (st < 0) printf("sound: no puedo leer %s\n", w.fpath.c_str());
        }
    }
    return __atomic_load_n(&w.state, __ATOMIC_ACQUIRE) > 0 ? &w : nullptr;
}

// ADPCM bytes the wav would take at the table rate: zero start block + data + end block
static uint32_t sfx_wav_length(const std::string& fpath, uint32_t freq) {
    FILE* f = open_file(fpath, "rb");
    if (!f) return 0;
    uint8_t hdr[12], ch[8];
    uint32_t rate = 0, data_ = 0; uint16_t chans = 0, bits = 0;
    if (fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
        while (fread(ch, 1, 8, f) == 8) {
            uint32_t n; memcpy(&n, ch + 4, 4);
            if (!memcmp(ch, "fmt ", 4) && n >= 16) {
                uint8_t fm[16];
                if (fread(fm, 1, 16, f) != 16) break;
                memcpy(&chans, fm + 2, 2); memcpy(&rate, fm + 4, 4); memcpy(&bits, fm + 14, 2);
                n -= 16;
            } else if (!memcmp(ch, "data", 4)) { data_ = n; break; }
            if (fseek(f, (long)(n + (n & 1)), SEEK_CUR)) break;
        }
    }
    fclose(f);
    if (!rate || !chans || !bits || !data_) return 0;
    uint64_t frames = data_ / ((uint32_t)chans * (bits / 8u));
    uint64_t samples = frames * (freq ? freq : rate) / rate;
    uint32_t length = (uint32_t)((samples + 27) / 28 + 2) * 16;
    return length < 48 ? 48 : length;
}

// ---------------- music and voices exposed as wav ----------------
// The ATRAC3 .unk entry is served from the .wav with the same name. The game sees a
// stream of empty frames with a 16-byte marker every 152 bytes: every frame starts at a
// multiple of 152 (voice 152, music 304), so the marker always lands at the start of the frame the
// game copies before decoding. The sceAtrac HLE recognizes it and outputs the resampled wav.
static const uint8_t AT3_MARKER[8] = {'T', 'T', 'T', 'A', 'T', '3', 0, 0};
#define AT3_STEP 152u
#define AT3_FRAME_STEREO 304u

struct At3Wav {
    std::string fpath, key;
    int rate = 0, channels = 0, bits = 0, is_float = 0, state = 0, fmt = -1;
    bool empty = false;
    uint32_t frame = AT3_STEP;
    uint64_t data_ = 0, frames = 0;
    FILE* f = nullptr;
    std::vector<uint8_t> buf;
    int64_t buf_start = 0, buf_n = 0;
    std::shared_ptr<const std::vector<uint8_t>> mem;   // samples read ahead by at3_voices_ahead (no file on the game thread)
};
static std::vector<At3Wav> at3_reg;
static std::unordered_map<uint32_t, std::vector<uint32_t>> at3_alt;   // voice id -> its id in each voice pack (0 none); read-only once the disc is up
static std::mutex at3_mutex;
static int at3_log = 0, at3_enabled = 1;

static bool at3_header_file(At3Wav& w);
static bool at3_header(At3Wav& w) {
    std::string real = hc_real(w.fpath, false);
    std::vector<int64_t> v;
    if (hc_get('W', real, v) && v.size() == 9) {
        w.channels = (int)v[1]; w.rate = (int)v[2]; w.bits = (int)v[3]; w.is_float = (int)v[4]; w.fmt = (int)v[5];
        w.data_ = (uint64_t)v[6]; w.frames = (uint64_t)v[7]; w.empty = v[8] != 0;
        return v[0] != 0;
    }
    bool ok = at3_header_file(w);
    hc_put('W', real, {ok ? 1 : 0, w.channels, w.rate, w.bits, w.is_float, w.fmt, (int64_t)w.data_, (int64_t)w.frames, w.empty ? 1 : 0});
    return ok;
}
static bool at3_header_file(At3Wav& w) {
    FILE* f = open_file(w.fpath, "rb");
    if (!f) return false;
    uint8_t hdr[12], ch[8];
    uint32_t data_ = 0;
    bool got = false;
    bool ok = fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4);
    while (ok && fread(ch, 1, 8, f) == 8) {
        uint32_t n;
        memcpy(&n, ch + 4, 4);
        if (!memcmp(ch, "fmt ", 4) && n >= 16) {
            uint8_t fm[16];
            if (fread(fm, 1, 16, f) != 16) break;
            uint16_t fmt, chans, bps;
            uint32_t sr;
            memcpy(&fmt, fm, 2); memcpy(&chans, fm + 2, 2); memcpy(&sr, fm + 4, 4); memcpy(&bps, fm + 14, 2);
            w.channels = chans; w.rate = (int)sr; w.bits = bps; w.is_float = fmt == 3; w.fmt = fmt;
            n -= 16;
        } else if (!memcmp(ch, "data", 4)) {
            w.data_ = (uint64_t)ftell(f); data_ = n; got = true; break;
        }
        if (fseek(f, (long)(n + (n & 1)), SEEK_CUR)) break;
    }
    fclose(f);
    if (w.channels < 1 || w.channels > 8 || w.rate < 4000) return false;
    if (w.bits != 8 && w.bits != 16 && w.bits != 24 && w.bits != 32) return false;
    w.frames = data_ / ((uint64_t)w.channels * (uint32_t)(w.bits / 8));
    w.empty = got && w.frames == 0;   // a wav without samples is an empty voice slot, like the game's own
    return w.frames > 0;
}

// entry size: whole frames plus the trailing byte of the original format
static uint32_t at3_register(const std::string& key, const std::string& fpath, bool stereo, int* id, bool* empty = nullptr) {
    DScope ds_(6);
    At3Wav w;
    w.fpath = fpath; w.key = key;
    if (!at3_header(w)) {
        if (w.empty && empty) { *empty = true; return 0; }
        printf("at3: %s no es un wav utilizable (formato %d, %d canales, %d Hz, %d bits, %llu cuadros; se acepta PCM o float de 8 a 32 bits)\n",
               mod_path(fpath).c_str(), w.fmt, w.channels, w.rate, w.bits, (unsigned long long)w.frames);
        return 0;
    }
    w.frame = stereo ? AT3_FRAME_STEREO : AT3_STEP;
    uint64_t per_frame = stereo ? 1024 : 2048;
    uint64_t samples = (w.frames * 44100 + (uint64_t)w.rate - 1) / (uint64_t)w.rate;
    uint64_t frames_n = (samples + per_frame - 1) / per_frame;
    if (!frames_n) frames_n = 1;
    at3_reg.push_back(w);
    *id = (int)at3_reg.size();
    if (at3_log) printf("at3: %s  %d Hz %d can %d bits, %llu tramas de %u bytes\n", key.c_str(), w.rate, w.channels, w.bits,
                        (unsigned long long)frames_n, w.frame);
    return (uint32_t)(frames_n * w.frame + 1);
}

static void at3_frame(At3Wav& w, int64_t i, int32_t* lr) {
    lr[0] = lr[1] = 0;
    if (i < 0 || (uint64_t)i >= w.frames || (!w.f && !w.mem)) return;
    int step = (w.bits / 8) * w.channels;
    if (w.mem) {
        size_t o = (size_t)i * (size_t)step;
        if (o + (size_t)step > w.mem->size()) return;
        const uint8_t* p = w.mem->data() + o;
        lr[0] = sfx_sample(p, w.bits, w.is_float);
        lr[1] = w.channels > 1 ? sfx_sample(p + w.bits / 8, w.bits, w.is_float) : lr[0];
        return;
    }
    if (i < w.buf_start || i >= w.buf_start + w.buf_n) {
        int64_t n = 16384;
        if ((uint64_t)(i + n) > w.frames) n = (int64_t)w.frames - i;
        w.buf.resize((size_t)(n * step));
        w.buf_start = i; w.buf_n = 0;
        if (fseek(w.f, (long)(w.data_ + (uint64_t)i * (uint64_t)step), SEEK_SET)) return;
        w.buf_n = (int64_t)(fread(w.buf.data(), 1, w.buf.size(), w.f) / (size_t)step);
        if (!w.buf_n) return;
    }
    if (i >= w.buf_start + w.buf_n) return;
    const uint8_t* p = w.buf.data() + (size_t)((i - w.buf_start) * step);
    lr[0] = sfx_sample(p, w.bits, w.is_float);
    lr[1] = w.channels > 1 ? sfx_sample(p + w.bits / 8, w.bits, w.is_float) : lr[0];
}

static int16_t at3_trim(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// a frame is always 2048 int16: 1024 stereo frames for music, 2048 plain samples for voice
// (the game reads a voice output as a mono stream, which is what doubles it to 44100)
static void lip_fill(uint32_t id, At3Wav& w, int64_t frame);
static bool lip_fresh = false;   // the first play of this at3_fill computed the mouth levels on the game thread
static std::unordered_set<uint32_t> at3_only;
static bool at3_fill(uint32_t id, int64_t frame, int16_t* dst, bool stereo) {
    if (!id || id > at3_reg.size() || frame < 0) return false;
    if (!psp_voice_lang && at3_only.count(id)) { memset(dst, 0, 2048 * sizeof(int16_t)); return true; }
    At3Wav& w = at3_reg[id - 1];
    std::lock_guard<std::mutex> lock_(at3_mutex);
    uint64_t tf0 = disc_us();
    bool first = !w.state;
    lip_fresh = false;
    struct SlowT { uint64_t t; bool& fr; bool first; const std::string& k; ~SlowT() {
        uint64_t ms = (disc_us() - t) / 1000; static int logs = 0;
        if (ms >= 4 && logs++ < 20) printf("at3: %s tardo %llu ms en el hilo del juego (%s%s)\n", k.c_str(), (unsigned long long)ms, first ? "primera apertura" : "lectura", fr ? ", boca calculada aqui" : ""); } } slow_{tf0, lip_fresh, first, w.key};
    if (!w.state && w.mem) w.state = 1;
    if (!w.state) {
        uint64_t t0 = psp_stall_on ? disc_us() : 0;
        w.f = open_file(w.fpath, "rb");
        if (t0) psp_stall_note(7, disc_us() - t0, 0);
        w.state = w.f ? 1 : -1;
        if (w.state < 0) printf("at3: no puedo abrir %s\n", w.fpath.c_str());
    }
    if (w.state < 0) return false;
    if (!stereo) lip_fill(id, w, frame);
    uint32_t n = stereo ? 1024 : 2048;
    int64_t base = frame * n;
    for (uint32_t i = 0; i < n; i++) {
        int64_t num = (base + (int64_t)i) * (int64_t)w.rate;
        int64_t p = num / 44100;
        int64_t fr = num % 44100;
        int32_t a[2], b[2];
        at3_frame(w, p, a);
        at3_frame(w, p + 1, b);
        int32_t l = a[0] + (int32_t)(((int64_t)(b[0] - a[0]) * fr) / 44100);
        int32_t r = a[1] + (int32_t)(((int64_t)(b[1] - a[1]) * fr) / 44100);
        if (stereo) { dst[i * 2] = at3_trim(l); dst[i * 2 + 1] = at3_trim(r); }
        else dst[i] = at3_trim((l + r) / 2);
    }
    return true;
}

// frames of the entry served for a wav (the game's 1024 stereo or 2048 mono samples at 44100)
static int64_t at3_frames_of(uint32_t id, bool stereo) {
    if (!id || id > at3_reg.size()) return -1;
    const At3Wav& w = at3_reg[id - 1];
    uint64_t per = stereo ? 1024 : 2048, smp = (w.frames * 44100 + (uint64_t)w.rate - 1) / (uint64_t)w.rate;
    return (int64_t)((smp + per - 1) / per);
}
static std::string at3_key_of(uint32_t id) { return id && id <= at3_reg.size() ? at3_reg[id - 1].key : std::string("?"); }

// ---------------- lip sync from the voice audio (item 29.2) ----------------
// each character voice gets a mouth level per 1/60 s (0 closed, 1 slightly open, 2 half, 3 open) from its wav the first time it plays;
// a voice that starts playing belongs to the character folder of its key (Characters/<x>/Voices/...)
struct LipTalk { uint32_t id; std::string dir; uint64_t t0, last; int64_t frames; };
struct LipShout { uint32_t id; int slot; uint64_t t0; int64_t lp0, lp1; std::vector<uint8_t> lv; };   // a fighter shout (ki charge, transformation) by its slot of the fighter table; lp0..lp1 = loop in steps
static std::vector<LipShout> lip_shouts;
std::atomic<int> lip_shout_n{0};
static std::map<uint32_t, std::vector<uint8_t>> lip_env;
static std::vector<LipTalk> lip_talk;
static std::mutex lip_mx;
static std::map<uint32_t, int> lip_paks;   // guest address where a Characters/ entry was read -> PACKFILE entry

static float lip_envf(const char* k, float def) { const char* e = getenv(k); return e && *e ? (float)atof(e) : def; }
template <class F> static void lip_levels(F sample, int64_t frames, int rate, std::vector<uint8_t>& lv) {
    int64_t win = std::max<int64_t>(1, rate / 60), n = frames / win;
    std::vector<float> rms((size_t)std::max<int64_t>(n, 0), 0.0f);
    float peak = 0.0f;
    for (int64_t i = 0; i < n; i++) {
        double acc = 0;
        for (int64_t j = 0; j < win; j++) { double v = sample(i * win + j); acc += v * v; }
        rms[(size_t)i] = (float)std::sqrt(acc / (double)win);
        peak = std::max(peak, rms[(size_t)i]);
    }
    lv.assign((size_t)n, 0);
    float gate = std::pow(10.0f, lip_envf("TTT_LIP_GATE", -40.0f) / 20.0f);
    if (peak <= gate) return;
    float e = 0.0f;
    for (int64_t i = 0; i < n; i++) {   // fast attack, short release
        float r = rms[(size_t)i];
        e = r > e ? r : e * 0.55f + r * 0.45f;
        float k = e / peak;
        lv[(size_t)i] = r < gate || k < 0.12f ? 0 : k < 0.36f ? 1 : k < 0.66f ? 2 : 3;
    }
    for (int64_t i = 1; i + 1 < n; i++) {   // the mouth closes between syllables: valleys of the raw energy
        float r = rms[(size_t)i], ml = 0.0f, mr = 0.0f;
        bool low = true;
        for (int64_t j = std::max<int64_t>(0, i - 3); j <= std::min<int64_t>(n - 1, i + 3); j++) if (rms[(size_t)j] < r) low = false;
        if (!low) continue;
        for (int64_t j = std::max<int64_t>(0, i - 8); j < i; j++) ml = std::max(ml, rms[(size_t)j]);
        for (int64_t j = i + 1; j <= std::min<int64_t>(n - 1, i + 8); j++) mr = std::max(mr, rms[(size_t)j]);
        if (r < 0.55f * std::min(ml, mr)) { lv[(size_t)i] = 0; lv[(size_t)i - 1] = std::min<uint8_t>(lv[(size_t)i - 1], 1); lv[(size_t)i + 1] = std::min<uint8_t>(lv[(size_t)i + 1], 1); }
    }
    for (int64_t i = 1; i + 1 < n; i++)   // no single-frame flicker
        if (lv[(size_t)i - 1] == lv[(size_t)i + 1] && lv[(size_t)i] != lv[(size_t)i - 1] && lv[(size_t)i]) lv[(size_t)i] = lv[(size_t)i - 1];
}

static bool lip_shout(const std::string& name) { std::string l = lower(name); return l.find("ki_charg") != std::string::npos || l.find("transformation") != std::string::npos; }
static void lip_shout_open(std::vector<uint8_t>& lv) {   // a shout keeps the mouth wide open while it sounds; gaps up to 0.2 s stay open
    int last = -1000;
    for (int i = 0; i < (int)lv.size(); i++) {
        if (!lv[(size_t)i]) continue;
        if (i - last <= 12) for (int j = std::max(0, last); j < i; j++) lv[(size_t)j] = 3;
        lv[(size_t)i] = 3; last = i;
    }
}
static void lip_analyze(At3Wav& w, std::vector<uint8_t>& lv) {
    lip_levels([&](int64_t i) { int32_t lr[2]; at3_frame(w, i, lr); return (lr[0] + lr[1]) * 0.5 / 32768.0; }, (int64_t)w.frames, w.rate, lv);
    if (lip_shout(w.key)) lip_shout_open(lv);
}

static size_t lip_wants(const std::string& key) {   // voice of a character: the folder length of its key, else npos
    std::string k = lower(key);
    size_t v = k.find("/voices/");
    if (!k.compare(0, 29, "menus/chara_reference_voices/")) return 28;   // Chi-Chi of the Character Reference
    if (!k.compare(0, 30, "menus/chara_reference_samples/")) return 29;   // character samples (viewer lip sync)
    if (v == std::string::npos || k.compare(0, 11, "characters/")) return std::string::npos;
    return v;
}
static void lip_fill(uint32_t id, At3Wav& w, int64_t frame) {   // at3_mutex held
    size_t v = lip_wants(w.key);
    if (v == std::string::npos) return;
    std::lock_guard<std::mutex> g(lip_mx);
    uint64_t now = disc_us();
    if (frame == 0) {
        if (!lip_env.count(id)) {
            uint64_t t0 = disc_us();
            lip_analyze(w, lip_env[id]);
            lip_fresh = true;
            static int logs = 0;
            if (logs++ < 20) printf("lip: %s, %u pasos en %llu ms (en el hilo del juego)\n", w.key.c_str(), (unsigned)lip_env[id].size(), (unsigned long long)((disc_us() - t0) / 1000));
        }
        lip_talk.erase(std::remove_if(lip_talk.begin(), lip_talk.end(), [&](const LipTalk& t) { return t.id == id; }), lip_talk.end());
        lip_talk.push_back({id, lower(w.key.substr(0, v)), now, now, 1});
        return;
    }
    for (auto& t : lip_talk) if (t.id == id) { t.last = now; t.frames = std::max(t.frames, frame + 1); }
}

// mouth level of the character whose PACKFILE entry was read at guest address pak (-1 = not speaking or unknown)
int psp_lip_level(uint32_t pak, int* known) {
    std::lock_guard<std::mutex> g(lip_mx);
    auto it = lip_paks.find(pak);
    *known = it != lip_paks.end();
    if (!*known) return -1;
    std::string f = lower(disc_pk[it->second].fpath);
    std::string dir = f.substr(0, f.find_last_of('/'));
    uint64_t now = disc_us(), delay = (uint64_t)std::max(0.0f, lip_envf("TTT_LIP_DELAY", 0.0f)) * 1000;
    for (size_t i = lip_talk.size(); i-- > 0;) {
        LipTalk& t = lip_talk[i];
        uint64_t dt = now - t.t0;
        auto env = lip_env.find(t.id);
        size_t n = env == lip_env.end() ? 0 : env->second.size();
        size_t k = dt < delay ? 0 : (size_t)((dt - delay) * 60 / 1000000);
        if (k >= n || dt > (uint64_t)t.frames * 46440 + 150000) { lip_talk.erase(lip_talk.begin() + (long)i); continue; }
        if (t.dir != dir) continue;
        return dt < delay ? 0 : env->second[k];
    }
    return -1;
}

// ms since the newest voice of the character whose pak was read at guest address pak started (-1 = none)
int psp_lip_started_ms(uint32_t pak) {
    std::lock_guard<std::mutex> g(lip_mx);
    auto it = lip_paks.find(pak);
    if (it == lip_paks.end()) return -1;
    std::string f = lower(disc_pk[it->second].fpath);
    std::string dir = f.substr(0, f.find_last_of('/'));
    uint64_t now = disc_us();
    for (size_t i = lip_talk.size(); i-- > 0;)
        if (lip_talk[i].dir == dir) return (int)((now - lip_talk[i].t0) / 1000);
    return -1;
}

// same, but matching only the character folder ("characters/<name>"): voices of another form or pack of the same character
static std::string lip_char_dir(const std::string& d) {
    size_t a = d.find('/'), b = a == std::string::npos ? a : d.find('/', a + 1);
    return b == std::string::npos ? d : d.substr(0, b);
}
int psp_lip_started_char_ms(uint32_t pak, std::string* last) {
    std::lock_guard<std::mutex> g(lip_mx);
    uint64_t now = disc_us();
    if (last) *last = lip_talk.empty() ? std::string() : lip_talk.back().dir;
    auto it = lip_paks.find(pak);
    if (it == lip_paks.end()) return -2;
    std::string f = lower(disc_pk[it->second].fpath), cd = lip_char_dir(f.substr(0, f.find_last_of('/')));
    for (size_t i = lip_talk.size(); i-- > 0;)
        if (lip_char_dir(lip_talk[i].dir) == cd) return (int)((now - lip_talk[i].t0) / 1000);
    return -1;
}

// character folder of a PACKFILE index (voice entry) and of the pak that holds a fighter's model: the speaker of a battle line
int psp_lip_same_char(uint32_t pak, uint32_t idx, std::string* seen) {
    std::lock_guard<std::mutex> g(lip_mx);
    if (!disc_ready || idx >= disc_pk.size()) return -2;
    std::string v = lower(disc_pk[idx].fpath);
    if (seen) *seen = v;
    if (v.compare(0, 11, "characters/")) return -3;
    auto it = lip_paks.find(pak);
    if (it == lip_paks.end()) return -1;
    std::string f = lower(disc_pk[it->second].fpath);
    return lip_char_dir(f) == lip_char_dir(v) ? 1 : 0;
}

// mouth level of whatever plays from a PACKFILE folder (lower case, no trailing '/'), -1 = silent
int psp_lip_dir(const std::string& dir) {
    std::lock_guard<std::mutex> g(lip_mx);
    uint64_t now = disc_us();
    for (size_t i = lip_talk.size(); i-- > 0;) {
        LipTalk& t = lip_talk[i];
        if (t.dir != dir) continue;
        uint64_t dt = now - t.t0;
        auto env = lip_env.find(t.id);
        size_t n = env == lip_env.end() ? 0 : env->second.size(), k = (size_t)(dt * 60 / 1000000);
        return k < n ? env->second[k] : -1;
    }
    return -1;
}

// a fighter's shouts (ki charge, transformation) move the mouth like a voice. The fighter is the owner slot of the battle sound queue
// entry that started the sound (EBOOT_TTT 113.1), so it works for sounds exposed as wav and for raw PSP ADPCM (BT3 sound paks)
static void lip_shout_off(uint32_t id) {   // lip_mx held
    lip_shouts.erase(std::remove_if(lip_shouts.begin(), lip_shouts.end(), [&](const LipShout& t) { return t.id == id; }), lip_shouts.end());
    lip_shout_n = (int)lip_shouts.size();
}
void psp_lip_shout_off(uint32_t id) { if (!lip_shout_n) return; std::lock_guard<std::mutex> g(lip_mx); lip_shout_off(id); }
static void lip_shout_on(uint32_t id, int slot, std::vector<uint8_t>&& lv, int64_t lp0, int64_t lp1, const char* what, int snd, int bank) {
    lip_shout_open(lv);
    std::lock_guard<std::mutex> g(lip_mx);
    lip_shout_off(id);
    static int logs = 0;
    if (logs++ < 20) printf("lip: grito %s (sonido %d, banco %d) del luchador %d, %u pasos%s\n", what, snd, bank, slot, (unsigned)lv.size(), lp1 > lp0 ? ", en bucle" : "");
    lip_shouts.push_back({id, slot, disc_us(), lp0, lp1, std::move(lv)});
    lip_shout_n = (int)lip_shouts.size();
}
uint32_t psp_lip_sfx(int wav, bool loop, int slot, int snd, int bank) {
    SfxWav* w = sfx_wav(wav);
    if (!w || !w->n || !w->rate || !lip_shout(w->name) || strncasecmp(w->pak.c_str(), "characters/", 11)) return 0;
    std::vector<uint8_t> lv;
    const int16_t* pcm = w->pcm.data();
    lip_levels([&](int64_t i) { return pcm[i] / 32768.0; }, w->n, w->rate, lv);
    int64_t win = std::max(1, w->rate / 60), n = (int64_t)lv.size(), l0 = -1, l1 = -1;
    if (loop) { bool lp = w->loop_end > w->loop_start; l0 = lp && w->loop_start > 0 ? std::min<int64_t>(n, w->loop_start / win) : 0; l1 = lp ? std::min<int64_t>(n, w->loop_end / win) : n; }
    uint32_t id = 0x80000000u | (uint32_t)wav;
    lip_shout_on(id, slot, std::move(lv), l0, l1, w->name.c_str(), snd, bank);
    return id;
}
uint32_t psp_lip_raw(uint32_t addr, uint32_t size, int pitch, bool loop, int slot, int snd, int bank) {
    if ((snd != 35 && snd != 36) || !bank || size < 32 || pitch <= 0 || !psp_mem) return 0;   // bank 0 = common battle sounds; 35 / 36 of a fighter's bank = ki charge / transformation
    static const int F[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    std::vector<int16_t> pcm; pcm.reserve(size / 16 * 28);
    int h1 = 0, h2 = 0; int64_t ls = 0;
    for (uint32_t b = 0; b + 16 <= size; b += 16) {
        const uint8_t* q = psp_mem + ((addr + b) & psp_mem_mask);
        int sh = q[0] & 15, fl = std::min(q[0] >> 4, 4);
        if (q[1] & 4) ls = (int64_t)pcm.size();
        for (int k = 0; k < 28; k++) {
            int nib = (q[2 + k / 2] >> ((k & 1) * 4)) & 15;
            int v = (int16_t)(nib << 12) >> (sh > 12 ? 9 : sh);
            v += (h1 * F[fl][0] + h2 * F[fl][1] + 32) / 64;
            v = std::max(-32768, std::min(32767, v)); h2 = h1; h1 = v; pcm.push_back((int16_t)v);
        }
        if (q[1] & 1) break;
    }
    int rate = (int)((int64_t)44100 * pitch / 4096);
    if (rate < 600 || pcm.empty()) return 0;
    std::vector<uint8_t> lv;
    lip_levels([&](int64_t i) { return pcm[(size_t)i] / 32768.0; }, (int64_t)pcm.size(), rate, lv);
    int64_t win = std::max(1, rate / 60), n = (int64_t)lv.size();
    uint32_t id = 0xC0000000u | ((addr >> 4) & 0x3FFFFFFFu);
    lip_shout_on(id, slot, std::move(lv), loop ? std::min<int64_t>(ls / win, n) : -1, loop ? n : -1, "adpcm", snd, bank);
    return id;
}
// mouth level of the shout of the fighter in this slot of the fighter table (-1 = none)
int psp_lip_shout_level(int slot) {
    if (!lip_shout_n) return -1;
    std::lock_guard<std::mutex> g(lip_mx);
    uint64_t now = disc_us();
    int best = -1;
    for (size_t i = lip_shouts.size(); i-- > 0;) {
        LipShout& t = lip_shouts[i];
        size_t n = t.lv.size(), k = (size_t)((now - t.t0) * 60 / 1000000);
        if (k >= n && t.lp1 > t.lp0 && t.lp0 >= 0) k = (size_t)t.lp0 + (k - (size_t)t.lp0) % (size_t)(t.lp1 - t.lp0);
        if (k >= n) { lip_shouts.erase(lip_shouts.begin() + (long)i); continue; }
        if (t.slot == slot) best = std::max(best, (int)t.lv[k]);
    }
    lip_shout_n = (int)lip_shouts.size();
    return best;
}

// a fighter's voices read ahead when its pak is read: samples in memory and mouth levels, so the first play opens no file
// on the game thread (TTT_VOICES_AHEAD=0 off)
static void at3_voices_ahead(const std::string& pk_fpath) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_VOICES_AHEAD"); on = !(e && *e == '0'); }
    size_t a = pk_fpath.find('/'), b = a == std::string::npos ? a : pk_fpath.find('/', a + 1);
    if (!on || b == std::string::npos) return;
    std::string dir = pk_fpath.substr(0, b + 1);   // the character folder: every form's Voices folder under it (1_Base, 2_SSJ...)
    static std::mutex m;
    static std::set<std::string> done;
    { std::lock_guard<std::mutex> g(m); if (!done.insert(lower(dir)).second) return; }
    std::thread([dir] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        static std::atomic<uint64_t> total{0};
        auto t0 = std::chrono::steady_clock::now();
        std::vector<uint32_t> ids;
        size_t n = at3_reg.size();
        for (size_t i = 0; i < n; i++) {
            const std::string& k = at3_reg[i].key;
            if (k.size() < dir.size() || strncasecmp(k.c_str(), dir.c_str(), dir.size()) || lower(k).find("/voices/") == std::string::npos) continue;
            ids.push_back((uint32_t)i + 1);
            auto it = at3_alt.find((uint32_t)i + 1);
            if (it != at3_alt.end()) for (uint32_t x : it->second) if (x) ids.push_back(x);
        }
        int got = 0; uint64_t bytes = 0;
        for (uint32_t id : ids) {
            if (total.load() > ((uint64_t)384 << 20)) break;
            At3Wav w;
            { std::lock_guard<std::mutex> g(at3_mutex); const At3Wav& x = at3_reg[id - 1]; if (x.state || x.mem) continue; w.fpath = x.fpath; w.key = x.key; w.rate = x.rate; w.channels = x.channels; w.bits = x.bits; w.is_float = x.is_float; w.data_ = x.data_; w.frames = x.frames; }
            uint64_t len = w.frames * (uint64_t)(w.bits / 8) * (uint64_t)w.channels;
            if (!len || len > ((uint64_t)8 << 20)) continue;
            FILE* f = open_file(w.fpath, "rb");
            if (!f) continue;
            auto v = std::make_shared<std::vector<uint8_t>>((size_t)len);
            bool ok = !fseek(f, (long)w.data_, SEEK_SET) && fread(v->data(), 1, v->size(), f) == v->size();
            fclose(f);
            if (!ok) continue;
            w.mem = v;
            std::vector<uint8_t> lv;
            bool lip = lip_wants(w.key) != std::string::npos;
            if (lip) lip_analyze(w, lv);
            { std::lock_guard<std::mutex> g(at3_mutex); At3Wav& x = at3_reg[id - 1]; if (!x.state && !x.mem) x.mem = v; }
            if (lip) { std::lock_guard<std::mutex> g(lip_mx); lip_env.emplace(id, std::move(lv)); }
            total += len; bytes += len; got++;
        }
        printf("at3: voces de %s leidas por adelantado: %d de %d en %.0f ms (%.1f MB)\n", dir.c_str(), got, (int)ids.size(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), bytes / 1048576.0);
        fflush(stdout);
    }).detach();
}
// the shared sound effects (General_Audios/SFX) read ahead once per session at the first battle load: a first play opened its wav
// on the game thread (s125: sfx_explosion_1 took 2 s) (TTT_SFX_AHEAD=0 off)
static void at3_sfx_ahead() {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_SFX_AHEAD"); on = !(e && *e == '0'); }
    static std::atomic<bool> done{false};
    if (!on || done.exchange(true)) return;
    std::thread([] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        auto t0 = std::chrono::steady_clock::now();
        static const char PRE[] = "General_Audios/SFX/";
        std::vector<uint32_t> ids;
        size_t n = at3_reg.size();
        for (size_t i = 0; i < n; i++) {
            const std::string& k = at3_reg[i].key;
            if (k.size() < sizeof(PRE) - 1 || strncasecmp(k.c_str(), PRE, sizeof(PRE) - 1)) continue;
            ids.push_back((uint32_t)i + 1);
        }
        int got = 0; uint64_t bytes = 0;
        for (uint32_t id : ids) {
            if (bytes > ((uint64_t)128 << 20)) break;
            At3Wav w;
            { std::lock_guard<std::mutex> g(at3_mutex); const At3Wav& x = at3_reg[id - 1]; if (x.state || x.mem) continue; w.fpath = x.fpath; w.bits = x.bits; w.channels = x.channels; w.frames = x.frames; w.data_ = x.data_; }
            uint64_t len = w.frames * (uint64_t)(w.bits / 8) * (uint64_t)w.channels;
            if (!len || len > ((uint64_t)8 << 20)) continue;
            FILE* f = open_file(w.fpath, "rb");
            if (!f) continue;
            auto v = std::make_shared<std::vector<uint8_t>>((size_t)len);
            bool ok = !fseek(f, (long)w.data_, SEEK_SET) && fread(v->data(), 1, v->size(), f) == v->size();
            fclose(f);
            if (!ok) continue;
            { std::lock_guard<std::mutex> g(at3_mutex); At3Wav& x = at3_reg[id - 1]; if (!x.state && !x.mem) x.mem = v; }
            bytes += len; got++;
        }
        printf("at3: efectos generales leidos por adelantado: %d de %d en %.0f ms (%.1f MB)\n", got, (int)ids.size(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), bytes / 1048576.0);
        fflush(stdout);
    }).detach();
}
void psp_battle_icons_ahead();
static void lip_read(uint32_t guest, int idx) {
    if (idx < 0 || strncasecmp(disc_pk[idx].fpath.c_str(), "Characters/", 11)) return;
    at3_voices_ahead(disc_pk[idx].fpath);
    at3_sfx_ahead();
    psp_battle_icons_ahead();
    std::lock_guard<std::mutex> g(lip_mx);
    lip_paks[guest] = idx;
}

static bool at3_marker(const uint8_t* d, uint32_t* id, uint32_t* off) {
    if (!at3_enabled || memcmp(d, AT3_MARKER, 8)) return false;
    memcpy(id, d + 8, 4);
    memcpy(off, d + 12, 4);
    if (!*id || *id > at3_reg.size()) return false;
    int k = psp_voice_lang;
    if (k > 0) { auto it = at3_alt.find(*id); if (it != at3_alt.end() && k <= (int)it->second.size() && it->second[k - 1]) *id = it->second[k - 1]; }
    return true;
}

// ---------------- textures exposed as png ----------------
// No metadata: the .atex/.rhg folder holds NN.png (or a loose name.atex.png for a single texture)
// and the structure is generated from each png's size and palette. NN@2x.png: image at twice what the
// game sees. A 16-byte marker at the start of the pixels tells the GL rasterizer to upload the png.
// Model and effect ATEX store alpha in 0..0x80 (the game doubles it): the png uses 0..255.
static const uint8_t TEX_MARKER[8] = {'T', 'T', 'T', 'P', 'N', 'G', 0, 0};

struct TexPng {
    std::string fpath, name, pak;
    int w = 0, h = 0, width0 = 0, height0 = 0, dw = 0, dh = 0, bpp = 8, state = 0, scale = 1, colors = 0, icon = -2;
    bool psp = false;
    uint32_t pal_off = 0, pal_size = 0, off = 0, sz = 0;
    std::vector<uint32_t> siblings;
    std::vector<std::pair<int, uint32_t>> mats;   // material -> texture id (mNN.png in the same container)
    bool mat_tex = false;
    int bt3 = 0;   // BT3 CTEX entry + 1, decoded from the P file named by fpath
    uint32_t dmg1 = 0;   // level 1 damage: texture id of the same image in 1_p/011_textures_dmg.*
    bool fit = false;   // BT3 HUD portrait: resized to the donor's size (integer multiple)
    int gk = -2, gs = -1, gc = 0, gp = 0, gm = -1;   // generated render served instead (disc/renders.h): kind, sysid, colour, page, menu number
    int mqk = -2, mqm = -1;   // generated map render (disc/renders.h gm): kind, map
    int tnk = -2, tna = 0, tnl = 0;   // names as text (disc/text_names.h): kind, atlas, language
    std::vector<uint32_t> px, pal;
    std::vector<uint8_t> idx;
};
struct TexHeader { uint64_t sz = 0; int64_t date = 0; int w = 0, h = 0, colors = 0; };
static std::vector<TexPng> tex_reg;
static std::map<std::string, TexHeader> tex_hdr;
static bool tex_hdr_dirty = false;
static std::mutex tex_mutex;
static int tex_log = -1, tex_enabled = 1, tex_fit = -1;
static std::set<uint64_t> tex_seen;
static int tex_without_n = 0;
static thread_local std::vector<uint32_t> tex_fit_px;

static bool tex_psp(const std::string& key) {
    std::string r = key;
    for (auto& ch : r) ch = (char)tolower((unsigned char)ch);
    return ends_with(r, "_textures.atex") || ends_with(r, "_texture.atex") || (ends_with(r, ".atex") && r.find("/4_effects/") != std::string::npos);
}

// strips a trailing "@Nx" and returns N
static int tex_scale(std::string& base) {
    size_t a = base.rfind('@');
    if (a == std::string::npos || base.size() < a + 3 || tolower((unsigned char)base.back()) != 'x') return 1;
    int n = 0;
    for (size_t k = a + 1; k + 1 < base.size(); k++) { if (!isdigit((unsigned char)base[k])) return 1; n = n * 10 + (base[k] - '0'); }
    base.resize(a);
    return n > 0 ? n : 1;
}

// @Nx in the name (also @1x) overrides the recorded size
static bool tex_explicit(const std::string& name) {
    std::string b = name.size() > 4 ? name.substr(0, name.size() - 4) : name;
    size_t n = b.size();
    tex_scale(b);
    return b.size() != n;
}

// "x.rhg.png" o "x.rhg@2x.png" -> "x.rhg"
static std::string tex_loose_name(const std::string& name, int* esc) {
    if (!ends_with(name, ".png")) return "";
    std::string b = name.substr(0, name.size() - 4);
    int e = tex_scale(b);
    if (!ends_with(b, ".atex") && !ends_with(b, ".rhg")) return "";
    if (esc) *esc = e;
    return b;
}

static void tex_parent(const std::string& folder, std::string& parent, std::string& name) {
    size_t s = folder.find_last_of("/\\");
    parent = s == std::string::npos ? "." : folder.substr(0, s);
    name = s == std::string::npos ? folder : folder.substr(s + 1);
}

static bool tex_loose(const std::string& folder, DirEntry* outp = nullptr, int* esc = nullptr) {
    static std::string last;
    static std::vector<DirEntry> inp;
    std::string parent, name;
    tex_parent(folder, parent, name);
    if (parent != last) { last = parent; inp.clear(); list_dir(parent, inp); }
    for (auto& e : inp) {
        if (e.dir) continue;
        int k = 1;
        std::string b = tex_loose_name(e.name, &k);
        if (b.empty() || b.size() != name.size()) continue;
        bool igual = true;
        for (size_t i = 0; i < b.size() && igual; i++) igual = tolower((unsigned char)b[i]) == tolower((unsigned char)name[i]);
        if (!igual) continue;
        if (outp) *outp = e;
        if (esc) *esc = k;
        return true;
    }
    return false;
}

static void tex_hdr_read() {
    FILE* f = fopen((disc_dir + "/cache/tex.cache").c_str(), "r");
    if (!f) return;
    char line[1200], fpath[1024];
    unsigned long long sz; long long date; int w, h, col;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "%1023[^\t]\t%llu\t%lld\t%d\t%d\t%d", fpath, &sz, &date, &w, &h, &col) == 6) {
            TexHeader c; c.sz = sz; c.date = date; c.w = w; c.h = h; c.colors = col;
            tex_hdr[fpath] = c;
        }
    fclose(f);
}

// assets/tamanos_texturas.txt: game size of every texture since extraction; an edited image without @N adapts to it
static std::map<std::string, std::pair<int, int>> tex_orig;
static bool tex_orig_dirty = false;

static void tex_orig_read() {
    FILE* f = fopen((disc_dir + "/tamanos_texturas.txt").c_str(), "r");
    if (!f) return;
    char line[2048]; int w, h, n;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%d %d %n", &w, &h, &n) < 2) continue;
        std::string k = line + n;
        while (!k.empty() && (k.back() == '\n' || k.back() == '\r')) k.pop_back();
        if (!k.empty()) tex_orig[lower(k)] = {w, h};
    }
    fclose(f);
}

static void tex_orig_write() {
    if (!tex_orig_dirty) return;
    FILE* f = fopen((disc_dir + "/tamanos_texturas.txt").c_str(), "w");
    if (!f) return;
    for (auto& kv : tex_orig) fprintf(f, "%d %d %s\n", kv.second.first, kv.second.second, kv.first.c_str());
    fclose(f);
}

static bool png_cabecera(const std::string& fpath, int* w, int* h, int* colors);

// game size of texture num in assets, ignoring mods
static bool tex_base_size(const std::string& folder, int num, int* w, int* h) {
    DScope ds_(0);
    std::vector<DirEntry> inp;
    std::string fpath; int esc = 1;
    list_dir_fs(folder, inp);
    for (auto& e : inp)
        if (!e.dir && ends_with(e.name, ".png") && atoi(e.name.c_str()) == num) { fpath = folder + "/" + e.name; std::string b = e.name.substr(0, e.name.size() - 4); esc = tex_scale(b); break; }
    if (fpath.empty() && num == 1) {
        std::string parent, name;
        tex_parent(folder, parent, name);
        inp.clear();
        list_dir_fs(parent, inp);
        for (auto& e : inp) { int s = 1; if (!e.dir && lower(tex_loose_name(e.name, &s)) == lower(name)) { fpath = parent + "/" + e.name; esc = s; break; } }
    }
    if (fpath.empty()) return false;
    bool m = mod_enabled; mod_enabled = false;
    int c = 0;
    bool ok = png_cabecera(fpath, w, h, &c);
    mod_enabled = m;
    if (ok) { *w = std::max(1, *w / esc); *h = std::max(1, *h / esc); }
    return ok;
}

static void tex_hdr_write() {
    tex_orig_write();
    if (!tex_hdr_dirty) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache"), ec);
    FILE* f = fopen((disc_dir + "/cache/tex.cache").c_str(), "w");
    if (!f) return;
    for (auto& kv : tex_hdr) fprintf(f, "%s\t%llu\t%lld\t%d\t%d\t%d\n", kv.first.c_str(), (unsigned long long)kv.second.sz, (long long)kv.second.date, kv.second.w, kv.second.h, kv.second.colors);
    fclose(f);
}

// IHDR and PLTE colour count (0 when not indexed) without decoding the image
static bool hc_get(char k, const std::string& real, std::vector<int64_t>& v);
static void hc_put(char k, const std::string& real, const std::vector<int64_t>& v);
static std::string hc_real(const std::string& fpath, bool base_only);
static bool png_cabecera_file(const std::string& fpath, int* w, int* h, int* colors);
static bool png_cabecera(const std::string& fpath, int* w, int* h, int* colors) {
    DScope ds_(7);
    std::string real = hc_real(fpath, false);
    std::vector<int64_t> v;
    if (hc_get('N', real, v) && v.size() == 4) { *w = (int)v[1]; *h = (int)v[2]; *colors = (int)v[3]; return v[0] != 0; }
    bool ok = png_cabecera_file(fpath, w, h, colors);
    hc_put('N', real, {ok ? 1 : 0, ok ? *w : 0, ok ? *h : 0, ok ? *colors : 0});
    return ok;
}
static bool png_cabecera_file(const std::string& fpath, int* w, int* h, int* colors) {
    FILE* f = open_file(fpath, "rb");
    if (!f) return false;
    uint8_t b[33];
    bool ok = fread(b, 1, 33, f) == 33 && !memcmp(b + 12, "IHDR", 4);
    if (ok) {
        auto u32 = [](const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; };
        *w = (int)u32(b + 16); *h = (int)u32(b + 20); *colors = 0;
        if (b[25] == 3) {
            uint8_t c[8];
            while (fread(c, 1, 8, f) == 8) {
                uint32_t n = u32(c);
                if (!memcmp(c + 4, "PLTE", 4)) { *colors = (int)(n / 3); break; }
                if (!memcmp(c + 4, "IDAT", 4) || !memcmp(c + 4, "IEND", 4) || fseek(f, (long)n + 4, SEEK_CUR)) break;
            }
            if (!*colors) *colors = 256;
        }
    }
    fclose(f);
    return ok && *w > 0 && *h > 0;
}

// ---------------- BT3 character files (item 29) ----------------
// a BT3 P file dropped in a colour's 1_p is read in place: its entries replace the TTT ones it provides
struct Bt3Src { std::string real; int rank = 0; };
struct Bt3Data { uint64_t sz = 0; int64_t date = 0; uint64_t chk = 0; std::shared_ptr<bt3::File> f; std::map<int, std::vector<uint8_t>> out; bt3::TexKeys tk; std::string texdir[17]; };
static std::mutex bt3_mx, bt3_of_mx;   // bt3_of_mx: writes of bt3_of (with bt3_mx) and quick reads without waiting for BT3 work
static thread_local bool bt3_bg = false;   // the background warm thread: never counted as the game waiting
static std::atomic<int> bt3_fg_n{0};
static std::atomic<int64_t> bt3_fg_ms{0};
static int64_t bt3_now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static bool bt3_fg_enter() { if (!bt3_bg) bt3_fg_n++; return !bt3_bg; }
struct Bt3Lock {   // bt3_mx for the game's own reads: the background warm waits while one is pending or recent
    bool fg; std::unique_lock<std::mutex> g;
    Bt3Lock() : fg(bt3_fg_enter()), g(bt3_mx) {}
    ~Bt3Lock() { g.unlock(); if (fg) { bt3_fg_ms = bt3_now_ms(); bt3_fg_n--; } }
};
static std::map<std::string, Bt3Src> bt3_of, bt3_tex_of, bt3_hud_of;   // pak key / texture container key -> P file
static std::map<std::string, Bt3Src> bt3_dmg_of;   // damaged pak key -> BT3 P of its 1_p_dmg (type 6 parts with idx 1)
static std::map<std::string, std::string> dmg_raw;   // damaged pak key of a closed colour pak -> that pak (type 10 parts: its entries)
static std::map<std::string, Bt3Data> bt3_cache;

static int mod_rank(const std::string& real) {
    std::string r = lower(real);
    for (size_t i = 0; i < mod_active.size(); i++) if (r.find("/" + lower(mod_active[i].first) + "/packfile/") != std::string::npos) return (int)i;
    return 1 << 30;
}

// opened P file (tools/bt3_open.py): NNN_name files, the .dbt ones as folders of PNGs; served as a P file in memory
static bool bt3_folder(const std::string& dir, uint64_t* sz, int64_t* date, std::vector<uint8_t>* d, std::string* newest = nullptr) {
    std::error_code ec;
    std::vector<std::string> ent(252);
    *sz = 0; *date = 0;
    for (auto& e : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(dir), ec)) {
        if (!e.is_regular_file(ec)) continue;
        *sz += (uint64_t)e.file_size(ec);
        int64_t wt = (int64_t)e.last_write_time(ec).time_since_epoch().count();
        if (wt > *date && newest) *newest = e.path().lexically_relative(std::filesystem::u8path(dir)).generic_u8string();
        *date = std::max(*date, wt);
        std::string name = e.path().filename().u8string();
        if (e.path().parent_path() == std::filesystem::u8path(dir) && name.size() > 4 && isdigit((unsigned char)name[0]) && isdigit((unsigned char)name[1]) && isdigit((unsigned char)name[2]) && name[3] == '_') {
            int n = atoi(name.c_str());
            if (n <= 42) ent[n] = e.path().u8string();   // 43 on: TTT files of the same 1_p (move list, HUD)
        }
    }
    if (!d) return *sz > 0;
    d->assign(0x400, 0);
    uint32_t c = 252; memcpy(&(*d)[0], &c, 4);
    for (int i = 0; i <= 252; i++) {
        uint32_t o = (uint32_t)d->size();
        memcpy(&(*d)[4 + 4 * i], &o, 4);
        if (i == 252) break;
        std::vector<uint8_t> x;
        if (!ent[i].empty() && !read_file(ent[i], x, true)) return false;
        d->insert(d->end(), x.begin(), x.end());
        d->resize(bt3::al((uint32_t)d->size(), 16), 0);
    }
    return true;
}

static int bt3_png_glow(const Bt3Data* b, int ent, int img) {   // opened .dbt: tools/bt3_open.py keeps the texture entry bytes 0x28..0x2F in a "bt3" tEXt chunk
    if (ent < 0 || ent > 16 || b->texdir[ent].empty()) return -1;
    char pn[16]; snprintf(pn, sizeof(pn), "/%02d.png", img + 1);
    std::vector<uint8_t> d;
    if (!read_file(b->texdir[ent] + pn, d, true)) return -1;
    for (size_t p = 8; p + 12 <= d.size();) {
        uint32_t len = (uint32_t)d[p] << 24 | (uint32_t)d[p + 1] << 16 | (uint32_t)d[p + 2] << 8 | d[p + 3];
        if (p + 12 + len > d.size()) break;
        if (!memcmp(&d[p + 4], "tEXt", 4) && len >= 4 + 16 && !memcmp(&d[p + 8], "bt3", 4)) {
            unsigned v[8] = {0};
            if (sscanf((const char*)&d[p + 12], "%2x%2x%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) return (int)v[5];
        }
        if (!memcmp(&d[p + 4], "IDAT", 4)) break;
        p += 12 + len;
    }
    return -1;
}

static Bt3Data* bt3_data(const std::string& real) {   // bt3_mx held; real = P file or opened folder
    static int gate = -2;   // TTT_BT3_STAT = ms between file checks (0 = every call); unset = checked once per session
    if (gate == -2) { const char* e = getenv("TTT_BT3_STAT"); gate = e && *e ? atoi(e) : -1; }
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto hit = bt3_cache.find(real);
    if (gate != 0 && hit != bt3_cache.end() && hit->second.f && (gate < 0 || now - hit->second.chk < (uint64_t)gate)) return &hit->second;
    std::error_code ec;
    auto p = std::filesystem::u8path(real);
    bool dir = std::filesystem::is_directory(p, ec);
    uint64_t sz = 0; int64_t date = 0;
    if (dir) bt3_folder(real, &sz, &date, nullptr);
    else { sz = (uint64_t)std::filesystem::file_size(p, ec); date = ec ? 0 : (int64_t)std::filesystem::last_write_time(p, ec).time_since_epoch().count(); }
    Bt3Data& b = bt3_cache[real];
    if (b.f && b.sz == sz && b.date == date) { b.chk = now; return &b; }
    b = Bt3Data(); b.sz = sz; b.date = date;
    DScope ds_l(19);
    auto f = std::make_shared<bt3::File>();
    if (!(dir ? bt3_folder(real, &sz, &date, &f->d) : read_file(real, f->d, true)) || !bt3::is_p(f->d.data(), f->d.size())) return nullptr;
    b.f = f;
    for (int e = 0; e <= 16; e = e ? e + 1 : 11) {   // images per CTEX: the opened folder (NNN_name.dbt/NN.png) or the CTEX itself
        const uint8_t* t; uint32_t n;
        if (dir) {
            char pre[8]; snprintf(pre, sizeof(pre), "%03d_", e);
            for (auto& x : std::filesystem::directory_iterator(p, ec))
                if (x.is_directory(ec) && x.path().filename().u8string().compare(0, 4, pre) == 0 && lower(x.path().filename().u8string()).compare(0, 17, "011_textures_dmg.")) { b.texdir[e] = x.path().u8string(); break; }
            if (!b.texdir[e].empty()) {
                int c = 0; char pn[16];
                for (; c < 256; c++) { snprintf(pn, sizeof(pn), "/%02d.png", c + 1); if (!std::filesystem::exists(std::filesystem::u8path(b.texdir[e] + pn), ec)) break; }
                b.tk.count[e] = c;
                continue;
            }
        }
        if (f->entry(e, &t, &n) && n >= 0x20 && bt3::u32(t + 4) == 8) b.tk.count[e] = (int)bt3::u32(t);
    }
    bt3::keys_of(*f, b.tk);
    for (auto& kv : b.tk.keys) if (kv.first >= 0 && kv.first < bt3::KEY_FACE && kv.second.first == 11) {
        int g = bt3::tex_glow(*f, 11, kv.second.second);
        if (g < 0) g = bt3_png_glow(&b, 11, kv.second.second);
        b.tk.glow[kv.first] = (uint8_t)std::max(0, std::min(g, 0x80));
    }
    bt3::plan_layers(b.tk);
    b.chk = now;
    return &b;
}

uint32_t psp_tpal_sig(const uint8_t* m, size_t n);
int psp_tpal_bt3(uint32_t sig, uint32_t src, const uint8_t* d, int n);
void psp_bt3_pak(uint32_t pak, uint32_t src);
static uint32_t bt3_src_id(const std::string& real) { uint32_t h = 2166136261u; for (unsigned char c : real) h = (h ^ c) * 16777619u; return h ? h : 1; }
// a character pak read at guest address: its BT3 source, so ramps and metal follow the colour (and damaged) and not the model signature
static void bt3_pak_read(uint32_t guest, int idx) {
    if (idx < 0 || strncasecmp(disc_pk[idx].fpath.c_str(), "Characters/", 11)) return;
    const std::string& key = disc_pk[idx].key;
    std::string real;
    if (dmg_key(key)) { Bt3Lock g; auto it = bt3_dmg_of.find(key); if (it != bt3_dmg_of.end()) real = it->second.real; }
    else { std::lock_guard<std::mutex> g(bt3_of_mx); auto it = bt3_of.find(key); if (it != bt3_of.end()) real = it->second.real; }
    uint32_t src = real.empty() ? 0 : bt3_src_id(real);
    psp_bt3_pak(guest, src);
    static int logs = 0;
    if (src && logs++ < 40) printf("cel: pak BT3 leido en 0x%08X, origen 0x%08X, %s\n", guest, src, key.c_str());
}
// BT3 cel (Graphics "Cel BT3"): the shading ramps of the character's own file, handed to the GPU by model signature
static void bt3_shade_reg(Bt3Data* b, const std::vector<uint8_t>& model, const std::string& real) {
    const bt3::File& f = *b->f;
    int first = bt3::shade_first(f);
    if (first < 0) return;
    std::string dir = b->texdir[11];
    auto clut = [&](int s, std::vector<uint32_t>& px) {
        const uint8_t* t; uint32_t n; int w = 0, h = 0;
        if (f.entry(11, &t, &n) && n >= 0x20) return bt3::texture(t, n, first + s, &w, &h, px);
        if (dir.empty()) return false;
        char pn[16]; snprintf(pn, sizeof(pn), "/%02d.png", first + s + 1);
        std::vector<uint8_t> d; std::string err; std::vector<uint32_t> q;
        if (!read_file(dir + pn, d, true) || !png_decode(d, &w, &h, q, &err) || w != 16 || h != 16) return false;
        auto lum = [](uint32_t c) { return (int)(c & 255) + (int)(c >> 8 & 255) + (int)(c >> 16 & 255); };
        bool flip = lum(q[15 * 16 + 7]) > lum(q[7]);   // PNG exports may be bottom-up: the lit end is the brighter one
        px.resize(256);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) px[y * 16 + x] = q[(flip ? 15 - y : y) * 16 + x];
        return true;
    };
    auto glow = [&](int ent, int img) { return bt3_png_glow(b, ent, img); };
    std::vector<uint8_t> tp;
    bool ok = bt3::shade_tpal(f, b->tk, clut, glow, tp);
    uint32_t sig = psp_tpal_sig(model.data(), model.size());
    int st = psp_tpal_bt3(sig, bt3_src_id(real), ok ? tp.data() : nullptr, ok ? (int)tp.size() : 0);
    static int logs = 0;
    if (logs < 400 && st != 1) {
        logs++;
        std::string nm = real; for (int k = 0, c = 0; k < (int)nm.size(); k++) if (nm[nm.size() - 1 - k] == '/' && ++c == 4) { nm = nm.substr(nm.size() - k); break; }
        static const char* SN[] = {"nueva", "igual", "reemplaza otra tabla con la misma firma", "sin rampas, se conserva la anterior", "sin rampas"};
        printf("cel: rampas BT3 del personaje, %d materiales, firma 0x%08X, %s, %s\n", ok ? (int)(tp.size() / 72) : 0, sig, SN[st], nm.c_str());
    }
}

int psp_metal_bt3(uint32_t sig, uint32_t src, const uint8_t* d, int n);
// BT3 metal (inside Cel BT3 / Mixed BT3): sphere and mask of every marked material, 128x128 each, handed to the GPU by model signature
static void bt3_metal_reg(Bt3Data* b, const std::vector<uint8_t>& model, const std::string& real) {
    const bt3::File& f = *b->f;
    std::string dir = b->texdir[11];
    auto tex = [&](int k, int* w, int* h, std::vector<uint32_t>& px) {
        const uint8_t* t; uint32_t n;
        if (f.entry(11, &t, &n) && n >= 0x20) return bt3::texture(t, n, k, w, h, px);
        if (dir.empty()) return false;
        char pn[16]; snprintf(pn, sizeof(pn), "/%02d.png", k + 1);
        std::vector<uint8_t> d; std::string err;
        if (!read_file(dir + pn, d, true) || !png_decode(d, w, h, px, &err)) return false;
        for (auto& c : px) { uint32_t a = c >> 24; c = (c & 0xFFFFFFu) | (a ? (a + 1) / 2 : 0) << 24; }   // bt3_open.py writes a * 2 - 1
        return true;
    };
    std::vector<bt3::Metal> ms; float nr = 40.0f, fr = 120.0f;
    uint32_t sig = psp_tpal_sig(model.data(), model.size());
    if (!bt3::metal(f, tex, ms, &nr, &fr)) {
        const uint8_t* m; uint32_t n;
        static int fl = 0;
        if (f.entry(2, &m, &n) && n >= 0x70 && (bt3::u32(m + 0x0C) & 1) && fl++ < 20) printf("cel: metal BT3 sin datos (esfera %d, mascaras %d+%d, texturas %s), %s\n", m[0x4D], m[0x52], m[0x53], dir.empty() ? "CTEX" : "PNG", real.c_str());
        psp_metal_bt3(sig, bt3_src_id(real), nullptr, 0);
        return;
    }
    std::vector<uint8_t> o(16);
    memcpy(&o[0], "MTL1", 4); memcpy(&o[4], &nr, 4); memcpy(&o[8], &fr, 4); uint32_t c = (uint32_t)ms.size(); memcpy(&o[12], &c, 4);
    for (auto& e : ms) {
        size_t at = o.size(); o.resize(at + 4 + 128 * 128 * 5);
        memcpy(&o[at], &e.key, 4);
        uint8_t* s = &o[at + 4]; uint8_t* k = s + 128 * 128 * 4;
        for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) {
            uint32_t p = e.sphere[(size_t)(y * e.sh / 128) * e.sw + x * e.sw / 128];
            s[(y * 128 + x) * 4] = (uint8_t)p; s[(y * 128 + x) * 4 + 1] = (uint8_t)(p >> 8); s[(y * 128 + x) * 4 + 2] = (uint8_t)(p >> 16); s[(y * 128 + x) * 4 + 3] = 255;
            uint32_t q = e.mask[(size_t)(y * e.mh / 128) * e.mw + x * e.mw / 128] >> 24;
            k[y * 128 + x] = (uint8_t)std::min<uint32_t>(255, q * 255 / 128);
        }
    }
    int st = psp_metal_bt3(sig, bt3_src_id(real), o.data(), (int)o.size());
    static int logs = 0;
    if (logs < 60 && st != 1) {
        logs++;
        std::string nm = real; for (int k = 0, cc = 0; k < (int)nm.size(); k++) if (nm[nm.size() - 1 - k] == '/' && ++cc == 4) { nm = nm.substr(nm.size() - k); break; }
        std::string ks; for (auto& e : ms) ks += " " + std::to_string(e.key);
        printf("cel: metal BT3, materiales%s, distancia %.0f a %.0f, firma 0x%08X, %s\n", ks.c_str(), nr, fr, sig, nm.c_str());
    }
}

static bool bt3_bytes(const std::string& real, int num, std::vector<uint8_t>** out) {
    Bt3Lock g;
    Bt3Data* b = bt3_data(real);
    if (!b) return false;
    auto it = b->out.find(num);
    if (it == b->out.end()) {
        std::vector<uint8_t> o;
        bt3::TexKeys tk = b->tk;
        if (!bt3::ttt_entry(*b->f, num, o, &tk)) return false;
        it = b->out.emplace(num, std::move(o)).first;
        if (num == 2) { bt3_shade_reg(b, it->second, real); bt3_metal_reg(b, it->second, real); }
    }
    *out = &it->second;
    return true;
}

// sizes of the BT3 conversions (assets/cache/bt3_sizes.txt): the startup scan needs only the sizes; the bytes are converted
// when the game reads them or by the warm-up thread once the window is open (psp_disc_warm)
struct Bt3Size { uint64_t sz = 0; int64_t date = 0; int hud = 0; std::map<int, uint32_t> num; std::string path; std::vector<std::array<int, 7>> tex; std::map<int, std::string> tdir; };   // H: hud = w << 16 | h, path = PNG of an opened P
static std::map<std::string, Bt3Size> bt3_sizes;   // "P|real" (entries of a P file) or "C<kind>|real" (one converted file)
static bool bt3_sizes_dirty = false, bt3_sizes_read_done = false;
static std::vector<std::pair<std::string, int>> bt3_warm_p, bt3_warm_c;
static std::unordered_map<std::string, std::unordered_map<std::string, DirEntry>> sig_memo;   // startup only
static void dir_sig(const std::string& d, uint64_t* sz, int64_t* date) {
    std::vector<DirEntry> v;
    list_dir_os(d, v);
    for (auto& e : v) { if (e.dir) dir_sig(d + "/" + e.name, sz, date); else { *sz += e.sz; *date = std::max(*date, e.date); } }
}
static std::mutex sig_mx;
static std::unordered_map<std::string, std::pair<uint64_t, int64_t>> dirsig_memo;   // startup only, sig_mx held
static bool fs_sig(const std::string& path, uint64_t* sz, int64_t* date) {   // size and date as the folder listing gives them
    std::string q = path;
    for (auto& ch : q) if (ch == '\\') ch = '/';
    size_t c = q.find_last_of('/');
    if (c == std::string::npos) return false;
    if (!scan_memo) {   // outside the startup scan: always the file itself (tools may have changed it)
        std::vector<DirEntry> one;
#ifdef _WIN32
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(to_utf16(q).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return false;
        FindClose(h);
        *sz = 0; *date = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dir_sig(q, sz, date); return true; }
        *sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        *date = ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        return true;
#else
        std::error_code ec;
        auto pth = std::filesystem::u8path(q);
        *sz = 0; *date = 0;
        if (std::filesystem::is_directory(pth, ec)) { dir_sig(q, sz, date); return true; }
        *sz = (uint64_t)std::filesystem::file_size(pth, ec);
        if (ec) return false;
        *date = (int64_t)std::filesystem::last_write_time(pth, ec).time_since_epoch().count();
        return true;
#endif
    }
    std::lock_guard<std::mutex> g(sig_mx);
    std::string dir = q.substr(0, c), name = lower(q.substr(c + 1));
    auto it = sig_memo.find(dir);
    if (it == sig_memo.end()) {
        std::vector<DirEntry> v;
        list_dir_os(dir, v);
        auto& m = sig_memo[dir];
        for (auto& e : v) m[lower(e.name)] = e;
        it = sig_memo.find(dir);
    }
    auto e = it->second.find(name);
    if (e == it->second.end()) return false;
    *sz = 0; *date = 0;
    if (e->second.dir) {
        auto m = dirsig_memo.find(q);
        if (m == dirsig_memo.end()) { DScope ds_(33); dir_sig(q, sz, date); m = dirsig_memo.emplace(q, std::make_pair(*sz, *date)).first; }
        *sz = m->second.first; *date = m->second.second;
    } else { *sz = e->second.sz; *date = e->second.date; }
    return true;
}
// small headers read at startup (wav format, PNG size, first bytes of BT3 files) kept in assets/cache/headers.txt by file
// size and date: a cold start opens none of those files again
static std::unordered_map<std::string, std::vector<int64_t>> hc_map;   // "K|real" -> size, date, values
static std::mutex hc_mx;
static bool hc_dirty = false, hc_loaded = false;
static void hc_load() {
    hc_loaded = true;
    FILE* f = fopen((disc_dir + "/cache/headers.txt").c_str(), "r");
    if (!f) return;
    char line[4096];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "# headers v1", 12)) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) {
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        std::vector<int64_t> v;
        for (char* p = tab + 1; *p && *p != '\n' && *p != '\r';) { char* e; v.push_back(strtoll(p, &e, 10)); if (e == p) break; p = e; while (*p == ' ') p++; }
        if (v.size() >= 2) hc_map[line] = v;
    }
    fclose(f);
}
static void hc_write() {
    std::lock_guard<std::mutex> g(hc_mx);
    if (!hc_dirty) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache"), ec);
    FILE* f = fopen((disc_dir + "/cache/headers.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "# headers v1\n");
    for (auto& kv : hc_map) { fprintf(f, "%s\t", kv.first.c_str()); for (size_t i = 0; i < kv.second.size(); i++) fprintf(f, i ? " %lld" : "%lld", (long long)kv.second[i]); fprintf(f, "\n"); }
    fclose(f);
    hc_dirty = false;
}
static bool fs_sig(const std::string& path, uint64_t* sz, int64_t* date);
static bool hc_get(char k, const std::string& real, std::vector<int64_t>& v) {
    uint64_t sz = 0; int64_t date = 0;
    if (!fs_sig(real, &sz, &date)) return false;
    std::lock_guard<std::mutex> g(hc_mx);
    if (!hc_loaded) hc_load();
    auto it = hc_map.find(std::string(1, k) + "|" + real);
    if (it == hc_map.end() || it->second.size() < 2 || (uint64_t)it->second[0] != sz || it->second[1] != date) return false;
    v.assign(it->second.begin() + 2, it->second.end());
    return true;
}
static void hc_put(char k, const std::string& real, const std::vector<int64_t>& v) {
    uint64_t sz = 0; int64_t date = 0;
    if (!fs_sig(real, &sz, &date)) return;
    std::lock_guard<std::mutex> g(hc_mx);
    std::vector<int64_t> x = {(int64_t)sz, date};
    x.insert(x.end(), v.begin(), v.end());
    hc_map[std::string(1, k) + "|" + real] = x;
    hc_dirty = true;
}
static std::string hc_real(const std::string& fpath, bool base_only) { std::string r = base_only ? fpath : mod_path(fpath); return r == fpath ? link_real(fpath) : r; }
// the first n (<= 16) bytes of a file, through the cache
static int head_bytes(const std::string& real, uint8_t* h, int n) {
    std::vector<int64_t> v;
    if (hc_get('B', real, v) && v.size() == 17) { int got = (int)v[0]; for (int i = 0; i < 16; i++) if (i < n) h[i] = (uint8_t)v[1 + i]; return std::min(got, n); }
    FILE* f = open_file(real, "rb", true);
    if (!f) return -1;
    uint8_t b[16] = {0};
    int got = (int)fread(b, 1, 16, f);
    fclose(f);
    v.assign(17, 0); v[0] = got; for (int i = 0; i < 16; i++) v[1 + i] = b[i];
    hc_put('B', real, v);
    for (int i = 0; i < n && i < 16; i++) h[i] = b[i];
    return std::min(got, n);
}
static std::string bt3_sizes_path() { return disc_dir + "/cache/bt3_sizes.txt"; }
static void bt3_sizes_read() {
    bt3_sizes_read_done = true;
    FILE* f = fopen(bt3_sizes_path().c_str(), "r");
    if (!f) return;
    char line[8192];
    char head[64]; snprintf(head, sizeof(head), "# bt3 v%d p%d", (int)bt3::VERSION, patch_bt3_chars.val);
    bool ok = fgets(line, sizeof(line), f) && !strncmp(line, head, strlen(head));
    while (ok && fgets(line, sizeof(line), f)) {
        std::vector<std::string> t;
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        for (size_t a = 0;;) { size_t b = l.find('\t', a); t.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
        if (t.size() != 6) continue;
        Bt3Size& z = bt3_sizes[t[0] + "|" + t[4]];
        z.sz = strtoull(t[1].c_str(), nullptr, 10); z.date = strtoll(t[2].c_str(), nullptr, 10); z.hud = atoi(t[3].c_str());
        if (t[0] == "H") { z.path = t[5] == "-" ? std::string() : t[5]; continue; }
        if (t[0] == "T") {   // m:e:k:w:h:colours:png items, dE=<texture folder after the source path>
            for (size_t a = 0; a < t[5].size();) {
                size_t b = t[5].find(',', a);
                std::string x = t[5].substr(a, b == std::string::npos ? std::string::npos : b - a);
                std::array<int, 7> r{};
                if (x.size() > 2 && x[0] == 'd') { size_t q = x.find('='); if (q != std::string::npos) z.tdir[atoi(x.c_str() + 1)] = x.substr(q + 1); }
                else if (sscanf(x.c_str(), "%d:%d:%d:%d:%d:%d:%d", &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6]) >= 5) z.tex.push_back(r);
                a = b == std::string::npos ? t[5].size() : b + 1;
            }
            continue;
        }
        for (size_t a = 0; a < t[5].size();) {
            size_t b = t[5].find(',', a);
            std::string x = t[5].substr(a, b == std::string::npos ? std::string::npos : b - a);
            size_t k = x.find(':');
            if (k != std::string::npos) z.num[atoi(x.c_str())] = (uint32_t)strtoul(x.c_str() + k + 1, nullptr, 10);
            a = b == std::string::npos ? t[5].size() : b + 1;
        }
    }
    fclose(f);
}
static void bt3_sizes_write() {
    if (!bt3_sizes_dirty) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache"), ec);
    FILE* f = fopen(bt3_sizes_path().c_str(), "w");
    if (!f) return;
    fprintf(f, "# bt3 v%d p%d\n", (int)bt3::VERSION, patch_bt3_chars.val);
    for (auto& kv : bt3_sizes) {
        size_t bar = kv.first.find('|');
        std::string nums;
        for (auto& n : kv.second.num) nums += (nums.empty() ? "" : ",") + std::to_string(n.first) + ":" + std::to_string(n.second);
        if (kv.first.compare(0, 2, "H|") == 0) nums = kv.second.path.empty() ? "-" : kv.second.path;
        if (kv.first.compare(0, 2, "T|") == 0) {
            nums.clear();
            for (auto& r : kv.second.tex) { char b[96]; snprintf(b, sizeof b, "%s%d:%d:%d:%d:%d:%d:%d", nums.empty() ? "" : ",", r[0], r[1], r[2], r[3], r[4], r[5], r[6]); nums += b; }
            for (auto& d : kv.second.tdir) nums += (nums.empty() ? "d" : ",d") + std::to_string(d.first) + "=" + d.second;
            if (nums.empty()) nums = "-";
        }
        fprintf(f, "%s\t%llu\t%lld\t%d\t%s\t%s\n", kv.first.substr(0, bar).c_str(), (unsigned long long)kv.second.sz, (long long)kv.second.date, kv.second.hud, kv.first.substr(bar + 1).c_str(), nums.c_str());
    }
    fclose(f);
    bt3_sizes_dirty = false;
}
// the cached sizes of real when it did not change; with make, a fresh record (old sizes dropped) otherwise
static Bt3Size* bt3_size_of(const std::string& kind, const std::string& real, bool make) {
    if (!bt3_sizes_read_done) bt3_sizes_read();
    uint64_t sz = 0; int64_t date = 0;
    if (!fs_sig(real, &sz, &date)) return nullptr;
    auto it = bt3_sizes.find(kind + "|" + real);
    if (it != bt3_sizes.end() && it->second.sz == sz && it->second.date == date) return &it->second;
    if (!make) return nullptr;
    Bt3Size& z = bt3_sizes[kind + "|" + real];
    z = Bt3Size(); z.sz = sz; z.date = date; z.hud = -1;
    bt3_sizes_dirty = true;
    return &z;
}

// ---------------- BT3 stages (item 65): a PCK2 stage file in Maps/ served as TTT's map pak, built from the file at read time ----------------
struct StageSrc { std::string real; uint32_t id0 = 0; int ntex = 0, nscroll = 0, nsurf = 0, nobj = 0, nwt = 0, net = 0, nst = 0; std::string fxbin; int fx[3] = {-1, -1, -1}; std::vector<int> banks; std::vector<uint32_t> sizes; int count() const { return ntex + (int)banks.size() + nscroll + nsurf + nobj + nwt + net + nst; } };
static std::map<std::string, StageSrc> bt3_stage_of;   // pak key -> stage file
static std::mutex bt3_stage_mx;
static std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> bt3_stage_files;
static std::map<std::string, std::shared_ptr<std::vector<std::vector<uint8_t>>>> bt3_stage_built;

static std::shared_ptr<std::vector<uint8_t>> bt3_stage_file(const std::string& real) {
    std::lock_guard<std::mutex> g(bt3_stage_mx);
    auto& f = bt3_stage_files[real];
    if (!f) { f = std::make_shared<std::vector<uint8_t>>(); if (!read_file(real, *f, true)) f->clear(); }
    return f;
}
static bool bt3_stage_tex(const std::string& real, int k, int* w, int* h, std::vector<uint32_t>& px) {
    auto f = bt3_stage_file(real);
    return !f->empty() && bt3::stage_texture(f->data(), f->size(), k, w, h, px);
}
// TTT's constant table of every map (entry 15) from the first original map
static const std::vector<uint8_t>& bt3_stage_e15() {
    static std::vector<uint8_t> e15;
    static bool done = false;
    if (done) return e15;
    done = true;
    const PkEntry& x = disc_pk[0x495];
    const Container* c = x.folder ? container_of(x.key) : nullptr;
    if (c && c->parts.size() > 15 && c->parts[15].type == 0) { read_file(disc_dir + "/Packfile/" + x.key + "/" + c->parts[15].fpath, e15); return e15; }
    std::vector<uint8_t> d;
    if (!read_file(disc_dir + "/Packfile/" + x.fpath, d) || d.size() < 0x80) return e15;
    auto be = [&](size_t o) { return o + 4 <= d.size() ? (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3] : 0u; };
    uint32_t a = be(4 + 4 * 15), b = be(4 + 4 * 16);
    if (be(0) > 15 && a <= b && b <= d.size()) e15.assign(d.begin() + a, d.begin() + b);
    return e15;
}
void psp_gpu_layers_set(uint32_t token, const std::vector<uint8_t>& d);
// map effects from a mod (item 65): <map>.fx.txt (haze, bubbles, storm = 0 / 1) and <map>.fx.bin (PCK2 with entries 13 / 17) next to the map's pak path
struct MapFx { bool any = false; int haze = -1, bub = -1, storm = -1; std::string bin, txt; };
static MapFx map_fx(const std::string& pak_path) {
    MapFx o;
    std::string p = pak_path;
    size_t sl = p.find_last_of('/'), dot = p.find_last_of('.');
    if (dot != std::string::npos && (sl == std::string::npos || dot > sl)) p.resize(dot);
    std::string t = p + ".fx.txt", rt = mod_path(t), b = p + ".fx.bin", rb = mod_path(b);
    if (rb != b) o.bin = rb;
    std::vector<uint8_t> f;
    if (rt == t || !read_file(rt, f, true)) return o;
    o.any = true; o.txt = rt;
    std::string all(f.begin(), f.end());
    size_t a = 0;
    while (a < all.size()) {
        size_t e = all.find('\n', a);
        std::string ln = all.substr(a, e == std::string::npos ? std::string::npos : e - a);
        a = e == std::string::npos ? all.size() : e + 1;
        size_t q = ln.find('=');
        if (q == std::string::npos) continue;
        std::string k = lower(ln.substr(0, q)), v = ln.substr(q + 1);
        while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
        while (!k.empty() && isspace((unsigned char)k[0])) k.erase(0, 1);
        int n = atoi(v.c_str()) != 0 ? 1 : 0;
        if (k == "haze") o.haze = n; else if (k == "bubbles") o.bub = n; else if (k == "storm") o.storm = n;
    }
    return o;
}
static std::shared_ptr<std::vector<std::vector<uint8_t>>> bt3_stage_make(const StageSrc& s, bt3::StageInfo* info, bool with_ids) {
    auto f = bt3_stage_file(s.real);
    std::shared_ptr<std::vector<uint8_t>> fxf;
    bt3::StageFx fx; fx.haze = s.fx[0]; fx.bub = s.fx[1]; fx.storm = s.fx[2];
    if (!s.fxbin.empty()) { fxf = bt3_stage_file(s.fxbin); if (!fxf->empty() && bt3::pck2_list(fxf->data(), fxf->size(), fx.e)) fx.d = fxf->data(); }
    auto r = std::make_shared<std::vector<std::vector<uint8_t>>>();
    bt3::StageInfo si;
    uint32_t id0 = with_ids ? s.id0 : 0;
    int nt = s.ntex;
    auto tid = [&](int k) -> uint32_t { if (!id0) return 0; if (k >= bt3::PTCL_TEX) { int i = k - bt3::PTCL_TEX, base = s.count() - s.nwt - s.net - s.nst; return i < 0x20 ? (i < s.nwt ? id0 + (uint32_t)(base + i) : 0) : i < 0x40 ? (i - 0x20 < s.net ? id0 + (uint32_t)(base + s.nwt + i - 0x20) : 0) : (i - 0x40 < s.nst ? id0 + (uint32_t)(base + s.nwt + s.net + i - 0x40) : 0); } if (k < 0x80) return k < nt ? id0 + (uint32_t)k : 0; if (k >= bt3::SURF_TEX) return k - bt3::SURF_TEX < s.nsurf ? id0 + (uint32_t)(nt + (int)s.banks.size() + s.nscroll + k - bt3::SURF_TEX) : 0; if (k >= bt3::SCROLL_TEX) return k - bt3::SCROLL_TEX < s.nscroll ? id0 + (uint32_t)(nt + (int)s.banks.size() + k - bt3::SCROLL_TEX) : 0; if (k >= bt3::OBJ_TEX) return k - bt3::OBJ_TEX < s.nobj ? id0 + (uint32_t)(nt + (int)s.banks.size() + s.nscroll + s.nsurf + k - bt3::OBJ_TEX) : 0; for (size_t i = 0; i < s.banks.size(); i++) if (0x80 + s.banks[i] == k) return id0 + (uint32_t)(nt + i); return 0; };
    if (f->empty() || !bt3::stage_pak(f->data(), f->size(), bt3_stage_e15(), tid, *r, si, &fx)) r->clear();
    else if (id0 && !si.layers.empty()) psp_gpu_layers_set(id0, si.layers);   // BT3 layers (clouds, surfaces) kept on the port side, found by token
    if (info) *info = si;
    return r;
}
// battle map buffer: 4 MB in TTT; with BT3 stages it grows to the biggest built stage pak (TTT_MAP_BUF=0 keeps 4 MB)
uint32_t psp_map_buf_size() {
    uint32_t m = 0x400000u;
    if (const char* e = getenv("TTT_MAP_BUF")) if (*e == '0') return m;
    if (!patch_bt3_maps.val) return m;
    std::lock_guard<std::mutex> g(bt3_stage_mx);
    for (auto& s : bt3_stage_of) { uint32_t t = 4 + 4 * ((uint32_t)s.second.sizes.size() + 1); t = (t + 31) & ~31u; for (uint32_t z : s.second.sizes) t += (z + 31) & ~31u; m = std::max(m, t); }
    return (m + 0xFFFFu) & ~0xFFFFu;
}
static std::shared_ptr<std::vector<std::vector<uint8_t>>> bt3_stage_get(const std::string& key) {
    StageSrc s;
    { std::lock_guard<std::mutex> g(bt3_stage_mx); auto it = bt3_stage_of.find(key); if (it == bt3_stage_of.end()) return nullptr; s = it->second; auto b = bt3_stage_built.find(key); if (b != bt3_stage_built.end()) return b->second; }
    auto t0 = std::chrono::steady_clock::now();
    auto r = bt3_stage_make(s, nullptr, true);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("mapas: %s: escenario BT3 armado en %.0f ms\n", key.c_str(), ms);
    std::lock_guard<std::mutex> g(bt3_stage_mx);
    bt3_stage_built[key] = r;
    return r;
}
// a map pak whose file is a BT3 stage becomes a 26-entry container of stage parts (type 11), its textures registered by marker
static bool bt3_stage_entry(PkEntry& x, const std::string& fpath, uint64_t fsz) {
    if (!patch_bt3_maps.val || x.fpath.compare(0, 5, "Maps/") || !x.fpath.compare(0, 9, "Maps/SFX/") || fsz < 0x400) return false;
    std::string real = mod_path(fpath);
    uint8_t h[0x200];
    FILE* f = open_file(real, "rb", true);
    if (!f) return false;
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    if (!bt3::is_stage(h, got, (size_t)fsz)) return false;
    auto t0 = std::chrono::steady_clock::now();
    StageSrc s; s.real = real;
    MapFx mf = map_fx(fpath);
    if (mf.any) { s.fxbin = mf.bin; s.fx[0] = mf.haze; s.fx[1] = mf.bub; s.fx[2] = mf.storm; }
    bt3::StageInfo si;
    auto r = bt3_stage_make(s, &si, false);
    if (r->empty()) { printf("mapas: %s: el escenario BT3 no se pudo leer\n", x.fpath.c_str()); return false; }
    s.ntex = si.ntex; s.banks = si.banks; s.nscroll = si.scroll_tex; s.nsurf = si.surf_tex; s.nobj = si.obj_tex; s.nwt = si.ptcl_wtex; s.net = si.ptcl_etex; s.nst = si.ptcl_stex;
    s.id0 = (uint32_t)tex_reg.size() + 1;
    int nb = (int)si.banks.size();
    for (int k = 0; k < s.count(); k++) {
        TexPng t; t.fpath = real; t.pak = x.fpath;
        int img = k < si.ntex ? k : k < si.ntex + nb ? 0x80 + si.banks[k - si.ntex] : k < si.ntex + nb + si.scroll_tex ? bt3::SCROLL_TEX + (k - si.ntex - nb) : k < si.ntex + nb + si.scroll_tex + si.surf_tex ? bt3::SURF_TEX + (k - si.ntex - nb - si.scroll_tex) : bt3::OBJ_TEX + (k - si.ntex - nb - si.scroll_tex - si.surf_tex);
        int pk = k - (s.count() - s.nwt - s.net - s.nst);
        if (pk >= 0) img = bt3::PTCL_TEX + (pk < s.nwt ? pk : pk < s.nwt + s.net ? 0x20 + pk - s.nwt : 0x40 + pk - s.nwt - s.net);
        if (pk >= 0 && pk < (int)si.ptcl_src.size() && si.ptcl_src[pk]) t.fpath = s.fxbin;
        t.name = "bt3 stage " + std::to_string(img); t.bt3 = (img >= bt3::PTCL_TEX ? bt3::PTCL_KEY << 8 | (img - bt3::PTCL_TEX) : bt3::STAGE_TEX << 8 | img) + 1; t.colors = 256;
        t.width0 = t.dw = 16; t.height0 = t.dh = 16;
        tex_reg.push_back(t);
    }
    Container c; c.type = 1;
    for (int i = 0; i < bt3::STAGE_ENTRIES; i++) { Part q; q.type = 11; q.id = i; q.sz = (uint32_t)(*r)[i].size(); q.fpath = std::to_string(i); c.parts.push_back(q); s.sizes.push_back(q.sz); }
    { std::lock_guard<std::mutex> g(bt3_stage_mx); bt3_stage_of[x.fpath] = s; bt3_stage_built.erase(x.fpath); }
    disc_containers[x.fpath] = c;
    x.key = x.fpath; x.folder = true;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("mapas: %s: escenario BT3 leido en %.0f ms: %d texturas, %d grupos, %d partes, %d tiras, %d vertices, %d nodos, octree %d, %d zonas, %d mallas de colision, %d REL, %d EX, bancos %d\n",
           x.fpath.c_str(), ms, si.ntex, si.groups, si.parts, si.strips, si.verts, si.nodes, si.octree, si.zones, si.meshes, si.rel, si.ex, (int)si.banks.size());
    if (si.scroll_tex) printf("mapas: %s: lamina de nubes BT3: %d triangulos, %.0f vertices, %d texturas, tamano %.0f, velocidad %.5f, angulo %.1f\n", x.fpath.c_str(), si.scroll_tris, si.scroll[3], si.scroll_tex, si.scroll[0], si.scroll[1], si.scroll[2]);
    if (si.tex_anims) printf("mapas: %s: texturas animadas BT3: %d materiales\n", x.fpath.c_str(), si.tex_anims);
    if (mf.any) printf("mapas: %s: efectos del mod (%s): calor %d, burbujas %d, tormenta %d, texturas %s; aplicado: burbujas del mod %s, tormenta del mod %s, tormenta apagada %s, burbujas apagadas %s\n", x.fpath.c_str(), mf.txt.c_str(), mf.haze, mf.bub, mf.storm,
                       mf.bin.empty() ? "no" : mf.bin.c_str(), si.ptcl_fx & 1 ? "si" : "no", si.ptcl_fx & 2 ? "si" : "no", si.ptcl_fx & 4 ? "si" : "no", si.ptcl_fx & 8 ? "si" : "no");
    if (si.ptcl_stex) printf("mapas: %s: tormenta BT3: %d texturas\n", x.fpath.c_str(), si.ptcl_stex);
    if (si.ptcl_wtex || si.ptcl_etex) printf("mapas: %s: particulas BT3: clima %d (%d texturas), geiseres %d, humo %d, vapor %d, salpicaduras %s (%d texturas)\n", x.fpath.c_str(), si.ptcl_weather, si.ptcl_wtex, si.ptcl_geysers, si.ptcl_smoke, si.ptcl_steam, si.ptcl_water ? "si" : "no", si.ptcl_etex);
    if (si.obj_count) printf("mapas: %s: objetos animados BT3: %d animaciones, %d objetos, %d texturas, %d vertices\n", x.fpath.c_str(), si.obj_anims, si.obj_count, si.obj_tex, si.obj_verts);
    if (si.surf_tex) printf("mapas: %s: superficies animadas BT3: %d mallas, %d triangulos, %d texturas, %d con reflejo (%d cuadros de normales)\n", x.fpath.c_str(), si.surf_meshes, si.surf_tris, si.surf_tex, si.surf_skipped, si.surf_reflect);
    return true;
}

// a TTT map with a mod's <map>.fx.txt asking for bubbles or storm: its .fx.bin images registered by marker, layers kept on the port side
// by token (no F0 markers in TTT maps: disc_map_check hands the token to the GPU, which takes the camera from the game)
static std::map<std::string, uint32_t> ttt_fx_tok;
int psp_map_fx_tok = 0;
std::string psp_orig_map_stem(int i) {   // original map i: game table entry 0x495 + 2 i (its SFX pak is the next one)
    size_t k = 0x495 + 2 * (size_t)i;
    if (i < 0 || k >= disc_pk.size()) return "";
    const std::string& f = disc_pk[k].fpath;
    std::string l = lower(f);
    return l.compare(0, 5, "maps/") == 0 && l.size() > 9 && l.compare(l.size() - 4, 4, ".pak") == 0 ? f.substr(5, f.size() - 9) : "";
}
static int ttt_fx_seen[3] = {0, 0, 0};
static void ttt_map_fx_entry(const PkEntry& x, const std::string& fpath) {
    std::string l = lower(x.fpath);
    if (!patch_bt3_maps.val || l.compare(0, 5, "maps/") || !l.compare(0, 9, "maps/sfx/")) return;
    ttt_fx_seen[0]++;
    MapFx mf = map_fx(fpath);
    if (mf.any) ttt_fx_seen[1]++;
    if (!mf.any || (mf.bub != 1 && mf.storm != 1)) return;
    ttt_fx_seen[2]++;
    if (mf.bin.empty()) { printf("mapas: %s: efectos del mod sin %s\n", x.fpath.c_str(), "archivo .fx.bin"); return; }
    auto f = bt3_stage_file(mf.bin);
    std::vector<std::pair<uint32_t, uint32_t>> e;
    if (f->empty() || !bt3::pck2_list(f->data(), f->size(), e)) { printf("mapas: %s: el .fx.bin no se pudo leer\n", x.fpath.c_str()); return; }
    auto cnt = [&](size_t en) { return en < e.size() && e[en].second >= 0x20 && bt3::u32(f->data() + e[en].first + 4) == 8 ? (int)std::min<uint32_t>(bt3::u32(f->data() + e[en].first), 32) : 0; };
    int ne = mf.bub == 1 && cnt(17) >= 6 ? cnt(17) : 0, ns = mf.storm == 1 ? cnt(13) : 0;
    if (!ne && !ns) { printf("mapas: %s: efectos del mod: el .fx.bin no trae imagenes de burbujas (6+) ni de tormenta\n", x.fpath.c_str()); return; }
    uint32_t id0 = (uint32_t)tex_reg.size() + 1;
    for (int k = 0; k < ne + ns; k++) {
        TexPng t; t.fpath = mf.bin; t.pak = x.fpath;
        int img = bt3::PTCL_TEX + (k < ne ? 0x20 + k : 0x40 + k - ne);
        t.name = "bt3 stage " + std::to_string(img); t.bt3 = (bt3::PTCL_KEY << 8 | (img - bt3::PTCL_TEX)) + 1; t.colors = 256;
        t.width0 = t.dw = 16; t.height0 = t.dh = 16;
        tex_reg.push_back(t);
    }
    std::vector<uint8_t> lay;
    bt3::fx_layers((uint32_t)ne, (uint32_t)ns, [&](int k) -> uint32_t { int i = k - bt3::PTCL_TEX; return i >= 0x40 ? id0 + (uint32_t)(ne + i - 0x40) : id0 + (uint32_t)(i - 0x20); }, lay);
    psp_gpu_layers_set(id0, lay);
    ttt_fx_tok[x.fpath] = id0;
    printf("mapas: %s: efectos del mod en mapa TTT: burbujas %d texturas, tormenta %d texturas\n", x.fpath.c_str(), ne, ns);
}

static bool bt3_tex_decode(const std::string& real, int ek, int* w, int* h, std::vector<uint32_t>& px) {   // ek = CTEX entry << 8 | image
    if ((ek >> 8) == bt3::STAGE_TEX) return bt3_stage_tex(real, ek & 0xFF, w, h, px);
    if ((ek >> 8) == bt3::PTCL_KEY) return bt3_stage_tex(real, bt3::PTCL_TEX + (ek & 0xFF), w, h, px);
    std::shared_ptr<bt3::File> f;
    { Bt3Lock g; Bt3Data* b = bt3_data(real); if (b) f = b->f; }
    const uint8_t* t; uint32_t n;
    return f && f->entry(ek >> 8, &t, &n) && bt3::texture(t, n, ek & 0xFF, w, h, px);
}

static void tex_fit_bt3(TexPng& t) {
    if (t.bt3) for (auto& p : t.px) { uint32_t a = p >> 24; p = (p & 0x00FFFFFFu) | (a ? std::min(255u, a * 2 - 1) : 0u) << 24; }
    if (t.width0 <= 0 || t.height0 <= 0 || t.w <= 0 || t.h <= 0) return;
    int s = std::max(1, std::min(t.w / t.width0, t.h / t.height0)), W = t.width0 * s, H = t.height0 * s;
    if (W == t.w && H == t.h) return;
    std::vector<uint32_t> o((size_t)W * H);
    for (int y = 0; y < H; y++) {
        float fy = std::max(0.0f, (y + 0.5f) * t.h / H - 0.5f); int y0 = std::min((int)fy, t.h - 1), y1 = std::min(y0 + 1, t.h - 1); float ay = fy - y0;
        for (int x = 0; x < W; x++) {
            float fx = std::max(0.0f, (x + 0.5f) * t.w / W - 0.5f); int x0 = std::min((int)fx, t.w - 1), x1 = std::min(x0 + 1, t.w - 1); float ax = fx - x0;
            uint32_t a = t.px[(size_t)y0 * t.w + x0], b = t.px[(size_t)y0 * t.w + x1], c = t.px[(size_t)y1 * t.w + x0], d = t.px[(size_t)y1 * t.w + x1], r = 0;
            for (int k = 0; k < 32; k += 8) {
                float v = ((a >> k & 255) * (1 - ax) + (b >> k & 255) * ax) * (1 - ay) + ((c >> k & 255) * (1 - ax) + (d >> k & 255) * ax) * ay;
                r |= (uint32_t)std::min(255.0f, v + 0.5f) << k;
            }
            o[(size_t)y * W + x] = r;
        }
    }
    t.px.swap(o); t.w = W; t.h = H; t.idx.clear(); t.pal.clear();
}

static TexPng* tex_load(uint32_t id) {
    if (!id || id > tex_reg.size()) return nullptr;
    TexPng& t = tex_reg[id - 1];
    if (!t.state) {
        static std::mutex locks[64];
        std::lock_guard<std::mutex> lock_(locks[id & 63]);
        if (!t.state) {
            std::vector<uint8_t> f; std::string err;
            int st = t.bt3 ? (bt3_tex_decode(t.fpath, t.bt3 - 1, &t.w, &t.h, t.px) ? 1 : -1) : read_file(t.fpath, f) && png_decode(f, &t.w, &t.h, t.px, &err, &t.idx, &t.pal) ? 1 : -1;
            if (st > 0 && t.psp && !t.bt3) {
                for (auto& p : t.px) p = (p & 0x00FFFFFFu) | ((((p >> 24) + 1) >> 1) << 24);
                for (auto& p : t.pal) p = (p & 0x00FFFFFFu) | ((((p >> 24) + 1) >> 1) << 24);
            }
            if (st > 0 && t.fit) tex_fit_bt3(t);
            std::atomic_thread_fence(std::memory_order_release);
            t.state = st;
            if (t.state < 0) printf("texture: no puedo usar %s: %s\n", t.fpath.c_str(), err.empty() ? "no se puede leer" : err.c_str());
            else if (tex_log > 0) { std::string real = mod_path(t.fpath); printf("texture: %s :: %s %dx%d (juego %dx%d)%s%s\n", t.pak.c_str(), t.name.c_str(), t.w, t.h, t.width0, t.height0, real == t.fpath ? "" : " mod ", real == t.fpath ? "" : real.c_str()); }
            fflush(stdout);
        }
    }
    return t.state > 0 ? &t : nullptr;
}

// preload decodes into a local copy: building the whole game does not keep every png in memory
static thread_local bool tex_no_retain = false;
static TexPng* tex_copy(uint32_t id, TexPng& t) {
    const TexPng& r = tex_reg[id - 1];
    t.fpath = r.fpath; t.psp = r.psp; t.bpp = r.bpp; t.width0 = r.width0; t.height0 = r.height0; t.dw = r.dw; t.dh = r.dh;
    t.off = r.off; t.sz = r.sz; t.pal_off = r.pal_off; t.pal_size = r.pal_size; t.bt3 = r.bt3; t.fit = r.fit;
    if (t.bt3) { if (!bt3_tex_decode(t.fpath, t.bt3 - 1, &t.w, &t.h, t.px)) return nullptr; if (t.fit) tex_fit_bt3(t); return &t; }
    std::vector<uint8_t> f; std::string err;
    if (!read_file(t.fpath, f) || !png_decode(f, &t.w, &t.h, t.px, &err, &t.idx, &t.pal)) return nullptr;
    if (t.fit) tex_fit_bt3(t);
    if (t.psp) {
        for (auto& p : t.px) p = (p & 0x00FFFFFFu) | ((((p >> 24) + 1) >> 1) << 24);
        for (auto& p : t.pal) p = (p & 0x00FFFFFFu) | ((((p >> 24) + 1) >> 1) << 24);
    }
    return &t;
}

// one line per texture + GE state combination: shows stride, swizzle and declared size
static void tex_point(const TexPng& t, uint32_t id, int gw, int gh, int gp, int gs, int gf) {
    uint64_t k = ((uint64_t)id << 32) ^ ((uint64_t)gw << 20) ^ ((uint64_t)gh << 8) ^ ((uint64_t)gp << 3) ^ (uint64_t)(gs * 2 + (gf & 1));
    std::lock_guard<std::mutex> lock_(tex_mutex);
    if (!tex_seen.insert(k).second) return;
    int step = t.bpp == 4 ? t.dw / 2 : t.dw;
    int swz_sup = step % 16 == 0 && t.dh % 8 == 0 ? 1 : 0;
    char et[96]; et[0] = 0;
    if (gw != t.width0 || gh != t.height0) strcat(et, " TAM");
    if (gp != t.dw) strcat(et, " PASO");
    if (gs != swz_sup) strcat(et, " SWZ");
    if (!et[0]) strcpy(et, " ok");
    printf("texge: %s :: %s ge %dx%d paso %d swz %d fmt %d juego %dx%d bpp %d png %dx%d sup %d herm %d%s\n",
           t.pak.c_str(), t.name.c_str(), gw, gh, gp, gs, gf, t.width0, t.height0, t.bpp, t.w, t.h, swz_sup, (int)t.siblings.size(), et);
    fflush(stdout);
}

static void tex_point_without(uint32_t dir, int gw, int gh, int gp, int gs, int gf) {
    std::lock_guard<std::mutex> lock_(tex_mutex);
    if (tex_without_n >= 200) return;
    uint64_t k = 0x8000000000000000ull ^ ((uint64_t)dir << 20) ^ ((uint64_t)gw << 8) ^ (uint64_t)gh;
    if (!tex_seen.insert(k).second) return;
    tex_without_n++;
    printf("texsin: dir 0x%08X ge %dx%d paso %d swz %d fmt %d\n", dir, gw, gh, gp, gs, gf);
    fflush(stdout);
}

// button pages (item 16b): composed for the device in use by input/icons.h
int icons_page_of(const char* virt, const char* real);
uint32_t icons_epoch(int page);
int icons_slot_page(int k);
void icons_bound(int page);
bool icons_compose(int page, uint32_t id, int w0, int h0, int w, int h, const uint32_t* px, bool psp, int* ow, int* oh, const uint32_t** opx);

static int tex_icon_page(TexPng& t) {
    if (t.icon != -2) return t.icon;
    auto rel = [](std::string p) {
        for (auto& ch : p) if (ch == '\\') ch = '/';
        std::string root = disc_dir + "/";
        for (auto& ch : root) if (ch == '\\') ch = '/';
        return p.compare(0, root.size(), root) == 0 ? p.substr(root.size()) : p;
    };
    std::string v = rel(t.fpath), r = rel(link_real(t.fpath));
    t.icon = icons_page_of(v.c_str(), r.c_str());
    return t.icon;
}

// TTT_WINDOWS=1: textures bound in each Ctrl+F7 window, to ventanas_tex.txt
extern bool psp_st_mark;
extern int psp_windows_on;
static void tex_window(uint32_t id) {
    static std::mutex m;
    static std::map<uint32_t, uint32_t> seen;
    static bool was = false;
    static int idx = 0;
    std::lock_guard<std::mutex> lock_(m);
    if (psp_st_mark != was) {
        was = psp_st_mark;
        if (was) { idx++; seen.clear(); }
        else if (FILE* f = fopen("ventanas_tex.txt", idx == 1 ? "w" : "a")) {
            fprintf(f, "ventana %d\n", idx);
            for (auto& e : seen) fprintf(f, "tex %u %s\n", e.second, tex_reg[e.first - 1].fpath.c_str());
            fclose(f);
        }
    }
    if (was) seen[id]++;
}

static uint32_t rov_variant(uint32_t id);
static int patch_tex_idkey = 1;   // the texture id enters the GPU cache key (reload stage 2)
static bool rov_serve(uint32_t id, int* w, int* h, const uint32_t** px);
static uint32_t tw_variant(uint32_t id);
static bool tw_serve(uint32_t id, int* w, int* h, const uint32_t** px);
static uint32_t rq_variant(uint32_t id);
static bool rq_serve(uint32_t id, int* w, int* h, const uint32_t** px);
static uint32_t mq_variant(uint32_t id);
static bool mq_serve(uint32_t id, int* w, int* h, const uint32_t** px);
static uint32_t tn_variant(uint32_t id);
static bool tn_serve(uint32_t id, int* w, int* h, const uint32_t** px);
static uint32_t tex_variant(const uint8_t* d) {
    uint32_t id, chk;
    if (memcmp(d, TEX_MARKER, 8)) return 0;
    memcpy(&id, d + 8, 4); memcpy(&chk, d + 12, 4);
    if (chk != ~id || !id || id > tex_reg.size()) return 0;
    if (psp_windows_on > 0) tex_window(id);
    int pg = tex_icon_page(tex_reg[id - 1]);
    if (pg >= 0) icons_bound(pg);
    return (pg >= 0 ? icons_epoch(pg) : 0) ^ rov_variant(id) ^ tw_variant(id) ^ rq_variant(id) ^ mq_variant(id) ^ tn_variant(id) ^ (patch_tex_idkey ? id * 0x9E3779B1u : 0);
}

extern uint32_t psp_tex_force;
// texture id of the material's own image for the container that holds the texture at d (0 = use the texture itself)
static uint32_t tex_mat_id(const uint8_t* d, int mat) {
    uint32_t id, chk;
    if (mat < 0 || memcmp(d, TEX_MARKER, 8)) return 0;
    memcpy(&id, d + 8, 4); memcpy(&chk, d + 12, 4);
    if (chk != ~id || !id || id > tex_reg.size()) return 0;
    uint32_t own = 0, blink = 0;
    for (auto& m : tex_reg[id - 1].mats) { if (m.first == (mat & 0xFFF)) own = m.second; if (m.first == bt3::KEY_BLINK) blink = m.second; }
    // blinking (mat | 0x1000): a main texture of a face part with the size of the blink texture is drawn with it
    if ((mat & 0x1000) && own && blink && (mat & 0xFFF) < bt3::KEY_FACE) {
        const TexPng& a = tex_reg[own - 1]; const TexPng& b = tex_reg[blink - 1];
        if (a.width0 == b.width0 && a.height0 == b.height0) return blink;
    }
    return own;
}
// level 1 damage: the twin of the image drawn (a material's own image when force is set, else the texture itself)
static uint32_t tex_dmg1_id(const uint8_t* d, uint32_t force) {
    if (force && force <= tex_reg.size()) return tex_reg[force - 1].dmg1;
    uint32_t id, chk;
    if (memcmp(d, TEX_MARKER, 8)) return 0;
    memcpy(&id, d + 8, 4); memcpy(&chk, d + 12, 4);
    return chk == ~id && id && id <= tex_reg.size() ? tex_reg[id - 1].dmg1 : 0;
}
static bool tex_png_buscar(const uint8_t* d, uint32_t dir, uint32_t cdir, int gw, int gh, int gp, int gs, int gf, int* w, int* h, const uint32_t** px) {
    uint32_t id, chk;
    if (memcmp(d, TEX_MARKER, 8)) { if (tex_log > 1) tex_point_without(dir, gw, gh, gp, gs, gf); return false; }
    memcpy(&id, d + 8, 4); memcpy(&chk, d + 12, 4);
    if (chk != ~id) return false;
    if (psp_tex_force && psp_tex_force <= tex_reg.size()) {   // a model material with its own image: drawn at its own size
        TexPng* m = tex_load(psp_tex_force);
        if (!m) return false;
        static int logs = 0;
        if (logs < 40) { logs++; printf("texture: material con textura propia %s :: %s %dx%d\n", m->pak.c_str(), m->name.c_str(), m->w, m->h); }
        *w = m->w; *h = m->h; *px = m->px.data();
        return true;
    }
    uint64_t sync_t0 = __atomic_load_n(&tex_reg[id - 1].state, __ATOMIC_ACQUIRE) ? 0 : disc_us();
    TexPng* t = tex_load(id);
    if (sync_t0) {
        static int logs = 0;
        double ms = (disc_us() - sync_t0) / 1000.0;
        if (t && ms >= 5.0 && logs < 40) { logs++; printf("texture: decodificada al dibujarse en %.0f ms, %dx%d, %s\n", ms, t->w, t->h, t->fpath.c_str()); }
    }
    if (!t) return false;
    // colour variants: the game may use one texture's pixels with another's palette; the distance
    // between the loaded palette and the pixels tells which, wherever the container landed
    if (t->siblings.size() > 1 && cdir) {
        int64_t jump = (int64_t)cdir - (int64_t)dir, base = (int64_t)t->off;
        for (uint32_t k : t->siblings) {
            TexPng& c = tex_reg[k - 1];
            if ((int64_t)c.pal_off - base != jump) continue;
            if (k != id) { TexPng* v = tex_load(k); if (v) { t = v; id = k; } }
            break;
        }
    }
    if (tex_log > 1) tex_point(*t, id, gw, gh, gp, gs, gf);
    int tw = t->w, th = t->h;
    const uint32_t* tpx = t->px.data();
    { int sw = 0, sh = 0; const uint32_t* spx = nullptr; if (!psp_tex_force && rov_serve(id, &sw, &sh, &spx)) { tw = sw; th = sh; tpx = spx; } }   // generated render (same game size, any resolution)
    { int sw = 0, sh = 0; const uint32_t* spx = nullptr; if (!psp_tex_force && tw_serve(id, &sw, &sh, &spx)) { tw = sw; th = sh; tpx = spx; } }   // transformation wheel portraits
    { int sw = 0, sh = 0; const uint32_t* spx = nullptr; if (!psp_tex_force && rq_serve(id, &sw, &sh, &spx)) { tw = sw; th = sh; tpx = spx; } }   // map select 3D: Random's mark
    { int sw = 0, sh = 0; const uint32_t* spx = nullptr; if (!psp_tex_force && mq_serve(id, &sw, &sh, &spx)) { tw = sw; th = sh; tpx = spx; } }   // generated map renders
    { int sw = 0, sh = 0; const uint32_t* spx = nullptr; if (!psp_tex_force && tn_serve(id, &sw, &sh, &spx)) { tw = sw; th = sh; tpx = spx; } }   // names as text
    int page = tex_icon_page(*t);
    if (page >= 0) icons_compose(page, id, t->width0, t->height0, t->w, t->h, t->px.data(), t->psp, &tw, &th, &tpx);
    // the texel space the GE declares can be larger than the game image (non power of two sizes):
    // the png goes top left and the edge is replicated, which the hardware never samples
    // if the GE declares less than the image (the model carries its own size), the whole png covers the declared area
    if (tex_fit > 0 && gw >= t->width0 && gh >= t->height0 && (gw != t->width0 || gh != t->height0) && t->width0 > 0 && t->height0 > 0) {
        int aw = (int)((int64_t)gw * tw / t->width0), ah = (int)((int64_t)gh * th / t->height0);
        if (aw > 0 && ah > 0 && (int64_t)aw * ah <= (int64_t)16 << 20) {
            tex_fit_px.resize((size_t)aw * ah);
            for (int y = 0; y < ah; y++) {
                const uint32_t* fo = tpx + (size_t)(y < th ? y : th - 1) * tw;
                uint32_t* de = tex_fit_px.data() + (size_t)y * aw;
                for (int x = 0; x < aw; x++) de[x] = fo[x < tw ? x : tw - 1];
            }
            *w = aw; *h = ah; *px = tex_fit_px.data();
            return true;
        }
    }
    *w = tw; *h = th; *px = tpx;
    return true;
}

static uint32_t tex_align(uint32_t x, uint32_t a) { return (x + a - 1) / a * a; }
static int tex_pot2(int x) { int p = 1; while (p < x) p <<= 1; return p; }

// level 1 damage (item 30): 1_p/011_textures_dmg.atex (folder NN.png/mNN.png or loose png) or, BT3, 011_textures_dmg.dbt/NN.png;
// each image becomes the twin (TexPng.dmg1) of the same image of 011, drawn instead while the fighter is at level 1
static uint32_t tex_dmg1_reg(const std::string& key, const std::string& vpath, const std::string& name, int esc, bool psp) {
    std::string fpath = mod_path(vpath);   // a copy that comes from a mod lives in its folder, not in the assets
    int w = 0, h = 0, cols = 0;
    if (!png_cabecera(fpath, &w, &h, &cols)) { printf("disc: %s no es un png valido\n", fpath.c_str()); return 0; }
    TexPng t; t.fpath = fpath; t.name = name; t.pak = key; t.scale = esc; t.psp = psp; t.colors = cols; t.mat_tex = true;
    t.width0 = t.dw = std::max(1, w / esc); t.height0 = t.dh = std::max(1, h / esc);
    tex_reg.push_back(t);
    return (uint32_t)tex_reg.size();
}
static std::set<std::string> lv1_keys;   // colour keys (lower case, up to color_N) with level 1 textures
static std::set<std::string> blink_keys;   // colour keys with a blink texture (BT3 CTEX 12)
static void tex_dmg1(const std::string& key, const std::string& folder, bool psp, const std::vector<uint32_t>& ids, const std::vector<std::pair<int, uint32_t>>& mats,
                     const std::map<int, std::pair<int, int>>& bkeys, const std::string& bt3_real) {
    std::string parent = folder.substr(0, folder.find_last_of('/')), dir = parent + "/011_textures_dmg.atex";
    int n = 0;
    auto twin = [&](uint32_t id, uint32_t t) { if (id && t) { tex_reg[id - 1].dmg1 = t; n++; } };
    auto mat_id = [&](int m) { for (auto& x : mats) if (x.first == m) return x.second; return 0u; };
    DirEntry le; int esc = 1;
    if (is_dir(dir)) {
        std::vector<DirEntry> inp;
        list_dir(dir, inp);
        for (auto& e : inp) {
            if (e.dir || !ends_with(lower(e.name), ".png")) continue;
            std::string b = e.name.substr(0, e.name.size() - 4);
            int sc = tex_scale(b);
            bool m = (b[0] == 'm' || b[0] == 'M') && b.size() > 1 && isdigit((unsigned char)b[1]);
            if (!m && !isdigit((unsigned char)b[0])) continue;
            int num = atoi(b.c_str() + (m ? 1 : 0));
            uint32_t id = m ? mat_id(num) : num >= 1 && num <= (int)ids.size() ? ids[num - 1] : 0;
            if (id) twin(id, tex_dmg1_reg(key, dir + "/" + e.name, "dmg1 " + e.name, sc, psp));
        }
    } else if (!ids.empty() && tex_loose(dir, &le, &esc)) twin(ids[0], tex_dmg1_reg(key, parent + "/" + le.name, "dmg1 " + le.name, esc, psp));
    std::string prel;
    if (!n && mod_rel(parent, prel)) {   // the copy only in a mod (original colour from the assets): read the mod's files directly
        std::string pre = prel + "/011_textures_dmg.";
        for (auto it = mod_files.lower_bound(pre); it != mod_files.end() && !it->first.compare(0, pre.size(), pre); ++it) {
            std::string rest = it->first.substr(prel.size() + 1);
            if (!ends_with(rest, ".png")) continue;
            size_t sl = rest.find('/');
            if (sl == std::string::npos) { int sc = 1; if (!tex_loose_name(rest, &sc).empty() && !ids.empty()) twin(ids[0], tex_dmg1_reg(key, it->second, "dmg1 " + rest, sc, psp)); continue; }
            std::string b = rest.substr(sl + 1, rest.size() - sl - 5);
            if (b.empty() || b.find('/') != std::string::npos) continue;
            bool m = (b[0] == 'm') && b.size() > 1 && isdigit((unsigned char)b[1]);
            if (!m && !isdigit((unsigned char)b[0])) continue;
            int num = atoi(b.c_str() + (m ? 1 : 0));
            uint32_t id = m ? mat_id(num) : num >= 1 && num <= (int)ids.size() ? ids[num - 1] : 0;
            if (id) twin(id, tex_dmg1_reg(key, it->second, "dmg1 " + rest, tex_scale(b), psp));
        }
    }
    std::error_code ec;
    std::string bd = bt3_real.empty() ? std::string() : std::filesystem::is_directory(std::filesystem::u8path(bt3_real), ec) ? bt3_real : bt3_real.substr(0, bt3_real.find_last_of("/\\"));
    if (!bd.empty()) bd += "/011_textures_dmg.dbt";
    if (!bd.empty() && std::filesystem::is_directory(std::filesystem::u8path(bd), ec))
        for (auto& kv : bkeys) {
            if (kv.second.first != 11 || kv.first > 63) continue;
            char pn[16]; snprintf(pn, sizeof(pn), "/%02d.png", kv.second.second + 1);
            if (std::filesystem::exists(std::filesystem::u8path(bd + pn), ec)) twin(mat_id(kv.first), tex_dmg1_reg(key, bd + pn, "dmg1 bt3 " + std::to_string(kv.first), 1, psp));
        }
    static int logs = 0;
    if (n && logs++ < 40) printf("disc: %s: %d texturas de rotura nivel 1\n", key.c_str(), n);
    if (!n) {   // a copy is there but nothing matched: say what was seen
        std::vector<DirEntry> inp;
        list_dir(parent, inp);
        for (auto& e : inp) if (lower(e.name).compare(0, 17, "011_textures_dmg.") == 0 && logs++ < 40)
            printf("disc: %s: %s presente sin texturas enlazadas (%s, %d imagenes, carpeta %s)\n", key.c_str(), e.name.c_str(), e.dir ? "carpeta" : "archivo", (int)ids.size(), parent.c_str());
    }
    if (n) { std::string lk = lower(key); size_t p = lk.find("/1_p/"); if (p != std::string::npos) lv1_keys.insert(lk.substr(0, p)); }
}

// the folder (or loose png) defines everything: texture number from the name, size and depth from the png
static bool list_tex(Container& c, const std::string& key, const std::string& folder) {
    DScope ds_(8);
    c.type = 4; c.date = 0; c.sz = 0; c.parts.clear();
    struct P { int num, esc; std::string name, fpath; uint64_t sz; int64_t date; int slot = -1; };
    std::vector<P> pngs, mpngs;
    if (is_dir(folder)) {
        std::vector<DirEntry> inp;
        list_dir(folder, inp);
        for (auto& e : inp) {
            if (patch_char_textures.val && !e.dir && ends_with(lower(e.name), ".png") && (e.name[0] == 'm' || e.name[0] == 'M') && isdigit((unsigned char)e.name[1])) {
                std::string b = e.name.substr(0, e.name.size() - 4);
                int esc = tex_scale(b);
                mpngs.push_back({atoi(b.c_str() + 1), esc, e.name, folder + "/" + e.name, e.sz, e.date});
                continue;
            }
            if (e.dir || !ends_with(e.name, ".png") || !isdigit((unsigned char)e.name[0])) continue;
            std::string b = e.name.substr(0, e.name.size() - 4);
            int esc = tex_scale(b);
            pngs.push_back({atoi(b.c_str()), esc, e.name, folder + "/" + e.name, e.sz, e.date});
        }
        std::sort(pngs.begin(), pngs.end(), [](const P& a, const P& b) { return a.num < b.num; });
        for (size_t k = 0; k < pngs.size(); k++)
            if (pngs[k].num != (int)k + 1) { printf("disc: %s: las texturas deben numerarse 1, 2, 3... (%s en la posicion %u)\n", key.c_str(), pngs[k].name.c_str(), (unsigned)k + 1); break; }
        // text glyphs: 26 more parts for the command combos (patch button_icons), drawn over a copy of glyph 04
        if (pngs.size() == 31 && ends_with(lower(key), "buttons.atex") && icons_slot_page(0) >= 0)
            for (int k = 0; k < 26; k++) { P v = pngs[3]; v.num = 32 + k; v.name = std::to_string(32 + k) + ".png"; v.slot = k; pngs.push_back(v); }
    } else {
        DirEntry e; int esc = 1;
        if (!tex_loose(folder, &e, &esc)) { printf("disc: falta %s\n", key.c_str()); return false; }
        std::string parent, name;
        tex_parent(folder, parent, name);
        pngs.push_back({1, esc, e.name, parent + "/" + e.name, e.sz, e.date});
    }
    if (pngs.empty()) { printf("disc: %s sin png\n", key.c_str()); return false; }
    bool rhg = ends_with(key, ".rhg"), psp = tex_psp(key);
    uint32_t n = (uint32_t)pngs.size(), cur;
    std::vector<uint32_t> ids;
    for (auto& p : pngs) {
        std::string real, k;
        std::map<std::string, TexHeader>::iterator it;
        {
            DScope ds_h(1);
            real = mod_path(p.fpath);
            k = real == p.fpath ? key + "/" + p.name : "mod:" + real;
            it = tex_hdr.find(k);
            if (it == tex_hdr.end() || it->second.sz != p.sz || it->second.date != p.date) {
                DScope ds_m(26);
                TexHeader hdr; hdr.sz = p.sz; hdr.date = p.date;
                if (!png_cabecera(p.fpath, &hdr.w, &hdr.h, &hdr.colors)) { printf("disc: %s no es un png valido\n", p.fpath.c_str()); return false; }
                tex_hdr[k] = hdr; tex_hdr_dirty = true;
                it = tex_hdr.find(k);
            }
        }
        TexPng t; t.fpath = p.fpath; t.name = p.name; t.pak = key; t.scale = p.esc; t.psp = psp;
        t.colors = it->second.colors;
        int tw = std::max(1, it->second.w / p.esc), th = std::max(1, it->second.h / p.esc);
        if (p.slot >= 0) t.icon = icons_slot_page(p.slot);
        else if (p.esc == 1 && !tex_explicit(p.name)) {
            std::string ko = lower(key) + "#" + std::to_string(p.num);
            auto o = tex_orig.find(ko);
            int ow = 0, oh = 0;
            if (o != tex_orig.end()) { ow = o->second.first; oh = o->second.second; }
            else if (real != p.fpath) { if (tex_base_size(folder, p.num, &ow, &oh)) { tex_orig[ko] = {ow, oh}; tex_orig_dirty = true; } }
            else { tex_orig[ko] = {tw, th}; tex_orig_dirty = true; }
            if (ow > 0 && oh > 0 && (ow != tw || oh != th)) { tw = ow; th = oh; t.scale = std::max(1, it->second.w / ow); }
        } else {
            std::string ko = lower(key) + "#" + std::to_string(p.num);
            if (!tex_orig.count(ko)) { tex_orig[ko] = {tw, th}; tex_orig_dirty = true; }
        }
        t.bpp = t.colors > 0 && t.colors <= 16 ? 4 : 8;
        t.pal_size = t.bpp == 4 ? 64 : 1024;
        if (rhg) { t.width0 = tw; t.height0 = th; t.dw = (int)tex_align((uint32_t)tw, t.bpp == 4 ? 32 : 16); t.dh = (int)tex_align((uint32_t)th, 8); }
        else { t.dw = t.width0 = tex_pot2(tw); t.dh = t.height0 = tex_pot2(th); }
        t.sz = (uint32_t)t.dw * t.dh * t.bpp / 8;
        tex_reg.push_back(t);
        ids.push_back((uint32_t)tex_reg.size());
    }
    if (rhg && !ids.empty()) {   // BT3 HUD portrait: the donor keeps the size, the image comes from the BT3 file (opened: 000_hud.dbt/01.png)
        Bt3Src src; std::string dir;
        { Bt3Lock g; auto it = bt3_hud_of.find(key); if (it != bt3_hud_of.end()) src = it->second; }
        Bt3Size* hz = src.real.empty() ? nullptr : bt3_size_of("H", src.real, false);   // the portrait's size without reading the P file
        if (!src.real.empty() && !hz) {
            Bt3Lock g;
            Bt3Data* b = bt3_data(src.real);
            if (b) dir = b->texdir[0]; else src.real.clear();
        }
        if (!src.real.empty()) {
            TexPng& t = tex_reg[ids[0] - 1];
            int w = 0, h = 0, cols = 0;
            std::string png = dir.empty() ? std::string() : dir + "/01.png";
            if (hz) {
                w = hz->hud >> 16; h = hz->hud & 0xFFFF;
                if (!hz->path.empty()) { t.fpath = hz->path; t.bt3 = 0; } else { t.fpath = src.real; t.bt3 = 1; }
            } else if (!png.empty() && png_cabecera(png, &w, &h, &cols)) { t.fpath = png; t.bt3 = 0; }
            else {
                std::shared_ptr<bt3::File> f;
                { Bt3Lock g; Bt3Data* b = bt3_data(src.real); if (b) f = b->f; }
                const uint8_t* bt; uint32_t bn;
                if (f && f->entry(0, &bt, &bn) && bt3::tex_size(bt, bn, 0, &w, &h)) { t.fpath = src.real; t.bt3 = 1; }
                else w = 0;
            }
            if (!hz && w > 0 && w < 65536 && h < 65536) if (Bt3Size* z = bt3_size_of("H", src.real, true)) { z->hud = w << 16 | h; z->path = t.bt3 ? std::string() : t.fpath; }
            if (w > 0) {
                t.fit = true; t.name = "bt3 hud"; t.scale = 1; t.siblings.clear();
                static int logs = 0;
                if (logs++ < 40) printf("disc: %s: retrato HUD BT3 %dx%d servido a %dx%d\n", key.c_str(), w, h, t.width0 * std::max(1, std::min(w / t.width0, h / t.height0)), t.height0 * std::max(1, std::min(w / t.width0, h / t.height0)));
            }
        }
    }
    // ATEX: table at 0x20, each texture's pixels and palette back to back; RHG: tables aligned to 16 and each block to 128
    if (rhg) {
        cur = 0x30 + tex_align(n * 40, 16);
        cur += tex_align(n * 36, 16) + n * 32;
        for (uint32_t id : ids) { TexPng& t = tex_reg[id - 1]; cur = tex_align(cur, 128); t.off = cur; cur += t.sz; }
        for (uint32_t id : ids) { TexPng& t = tex_reg[id - 1]; cur = tex_align(cur, 128); t.pal_off = cur; cur += t.pal_size; }
    } else {
        cur = 0x20 + n * 0x60;
        for (uint32_t id : ids) { TexPng& t = tex_reg[id - 1]; t.off = cur; cur += t.sz; t.pal_off = cur; cur += t.pal_size; }
    }
    c.sz = cur;
    for (uint32_t k = 0; k < n; k++) {
        TexPng& t = tex_reg[ids[k] - 1];
        if (n > 1) t.siblings = ids;
        Part q; q.fpath = t.name; q.sz = t.sz; q.off = t.off; q.type = 4; q.idx = (int)k + 1; q.id = (int)ids[k];
        c.parts.push_back(q);
    }
    // material textures: decoded and drawn by the port only, the game's container never sees them
    std::vector<std::pair<int, uint32_t>> mats;
    std::vector<int> mrank;
    for (auto& p : mpngs) {
        if (p.num < 0 || p.num > 63) { printf("disc: %s: %s fuera de rango (material 0 a 63)\n", key.c_str(), p.name.c_str()); continue; }
        TexHeader hdr;
        if (!png_cabecera(p.fpath, &hdr.w, &hdr.h, &hdr.colors)) { printf("disc: %s no es un png valido\n", p.fpath.c_str()); continue; }
        TexPng t; t.fpath = p.fpath; t.name = p.name; t.pak = key; t.scale = p.esc; t.psp = psp; t.colors = hdr.colors; t.mat_tex = true;
        t.width0 = t.dw = std::max(1, hdr.w / p.esc); t.height0 = t.dh = std::max(1, hdr.h / p.esc);
        tex_reg.push_back(t);
        mats.push_back({p.num, (uint32_t)tex_reg.size()});
        mrank.push_back(mod_rank(mod_path(p.fpath)));
    }
    Bt3Src src; std::map<int, std::pair<int, int>> bkeys; std::shared_ptr<bt3::File> bf; std::string texdir[17];
    const Bt3Size* tcache = nullptr;
    static std::set<std::string> t_skip;
    {
        DScope ds_b(18);
        {
            Bt3Lock g;
            auto it = bt3_tex_of.find(key);
            if (it != bt3_tex_of.end()) src = it->second;
        }
        if (!src.real.empty()) {
            static int use = -1;
            if (use < 0) { const char* e = getenv("TTT_BT3_TEXCACHE"); use = e && *e == '0' ? 0 : 1; }
            if (use && !t_skip.count(src.real)) { const Bt3Size* z = bt3_size_of("T", src.real, false); if (z && z->hud == 1) tcache = z; }
            if (tcache) dprof[27].n++;
            else {
                Bt3Lock g;
                Bt3Data* b = bt3_data(src.real);
                if (b) { bkeys = b->tk.keys; bf = b->f; for (int e = 0; e < 17; e++) texdir[e] = b->texdir[e]; }
            }
        }
        if (bf || tcache) {   // BT3 textures by key (model, faces, blink); opened: NNN_name.dbt/NN.png; an mNN.png of the same or a higher mod wins for 0..63
            int nb = 0;
            std::vector<std::array<int, 7>> rec;
            bool from_file = true;
            auto add = [&](int m, int e, int k, int w, int h, int cols, bool is_png, const std::string& png) {
                size_t i = 0;
                while (i < mats.size() && mats[i].first != m) i++;
                if (i < mats.size() && m < 64 && mrank[i] <= src.rank) return;
                TexPng t; t.fpath = is_png ? png : src.real; t.name = "bt3 " + std::to_string(m); t.pak = key; t.psp = psp; t.mat_tex = true; t.bt3 = is_png ? 0 : (e << 8 | k) + 1; t.colors = cols;
                t.width0 = t.dw = w; t.height0 = t.dh = h;
                tex_reg.push_back(t);
                if (i < mats.size()) mats[i].second = (uint32_t)tex_reg.size();
                else { mats.push_back({m, (uint32_t)tex_reg.size()}); mrank.push_back(src.rank); }
                nb++;
            };
            if (tcache) {
                for (auto& d : tcache->tdir) if (d.first >= 0 && d.first < 17) texdir[d.first] = src.real + d.second;
                for (auto& r : tcache->tex) {
                    bkeys[r[0]] = {r[1], r[2]};
                    if (r[3] <= 0) continue;
                    std::string png;
                    if (r[6]) { char pn[16]; snprintf(pn, sizeof(pn), "/%02d.png", r[2] + 1); png = texdir[r[1]] + pn; }
                    add(r[0], r[1], r[2], r[3], r[4], r[5], r[6] != 0, png);
                }
            } else {
                for (auto& kv : bkeys) {
                    int m = kv.first, e = kv.second.first, k = kv.second.second, w, h, cols = 0;
                    char pn[16]; snprintf(pn, sizeof(pn), "/%02d.png", k + 1);
                    std::string png = texdir[e].empty() ? std::string() : texdir[e] + pn;
                    bool is_png = !png.empty() && png_cabecera(png, &w, &h, &cols);
                    const uint8_t* bt; uint32_t bn;
                    if (!is_png && !(bf->entry(e, &bt, &bn) && bt3::tex_size(bt, bn, k, &w, &h))) { rec.push_back({m, e, k, -1, -1, 0, 0}); continue; }
                    rec.push_back({m, e, k, w, h, cols, is_png ? 1 : 0});
                    add(m, e, k, w, h, cols, is_png, png);
                }
                std::map<int, std::string> tdir;
                for (int e = 0; e < 17; e++) {
                    if (texdir[e].empty()) continue;
                    std::string tail = texdir[e].compare(0, src.real.size(), src.real) == 0 ? texdir[e].substr(src.real.size()) : std::string();
                    if (tail.empty() || tail.find_first_of(",\t\n=") != std::string::npos) from_file = false;
                    else tdir[e] = tail;
                }
                if (!from_file) t_skip.insert(src.real);
                else if (bf) if (Bt3Size* z = bt3_size_of("T", src.real, true)) { z->tex = rec; z->tdir = tdir; z->hud = 1; bt3_sizes_dirty = true; }
            }
            static int logs = 0;
            if (logs++ < 40) printf("disc: %s: %d texturas BT3 por material\n", key.c_str(), nb);
        }
    }
    if (ends_with(lower(key), "/1_p/011_textures.atex")) tex_dmg1(key, folder, psp, ids, mats, bkeys, src.real);
    for (auto& m : mats) if (m.first == bt3::KEY_BLINK) { std::string lk = lower(key); size_t p = lk.find("/1_p/"); if (p != std::string::npos) blink_keys.insert(lk.substr(0, p)); break; }
    if (!mats.empty()) {
        for (uint32_t id : ids) tex_reg[id - 1].mats = mats;
        static int logs = 0;
        if (logs++ < 40) printf("disc: %s: %d texturas por material\n", key.c_str(), (int)mats.size());
    }
    return true;
}

// indices in PSP order: 16-byte x 8-row blocks when stride and height allow it
static void tex_write_indices(uint8_t* d, uint32_t sz, const std::vector<uint8_t>& idx, int w, int h, int bpp) {
    int step = bpp == 4 ? w / 2 : w;
    bool swz = step % 16 == 0 && h % 8 == 0;
    for (int y = 0; y < h; y++)
        for (int xb = 0; xb < step; xb++) {
            uint32_t n = swz ? (uint32_t)(((y / 8) * (step / 16) + xb / 16) * 128 + (y % 8) * 16 + xb % 16) : (uint32_t)(y * step + xb);
            if (n >= sz) continue;
            if (bpp == 4) d[n] = (uint8_t)((idx[(size_t)y * w + xb * 2] & 15) | ((idx[(size_t)y * w + xb * 2 + 1] & 15) << 4));
            else d[n] = idx[(size_t)y * w + xb];
        }
}

static void build_tex(const Container& c, const std::string& key, uint8_t* d) {
    bool rhg = ends_with(key, ".rhg");
    uint32_t n = (uint32_t)c.parts.size();
    auto p32 = [&](uint32_t o, uint32_t v) { if (o + 4 <= c.sz) memcpy(d + o, &v, 4); };
    uint32_t bd = 0x30 + tex_align(n * 40, 16), bp = bd + tex_align(n * 36, 16);
    if (rhg) {
        memcpy(d, "RHG", 4);
        p32(4, 1); p32(0x14, n); p32(0x1C, n); p32(0x24, n); p32(0x28, 2);
    } else { p32(0, n); p32(4, 8); }
    for (uint32_t k = 0; k < n; k++) {
        const Part& q = c.parts[k];
        if (!q.id || (uint32_t)q.id > tex_reg.size()) continue;
        TexPng& r = tex_reg[q.id - 1];
        if (rhg) {
            uint32_t e = 0x30 + k * 40;
            p32(e, 1); p32(e + 4, k); p32(e + 8, k); p32(e + 20, (uint32_t)r.width0); p32(e + 24, (uint32_t)r.height0);
            e = bd + k * 36;
            p32(e, 2); p32(e + 4, r.bpp == 4 ? 0x12 : 0x11); p32(e + 8, (uint32_t)r.dw); p32(e + 12, (uint32_t)r.dh); p32(e + 20, r.sz); p32(e + 24, (uint32_t)(r.bpp == 4 ? r.dw / 2 : r.dw));
            e = bp + k * 32;
            p32(e, 0x14); p32(e + 8, r.pal_size); p32(e + 12, r.pal_size / 4); p32(e + 16, 2);
        } else {
            uint32_t e = 0x20 + k * 0x60;
            p32(e, r.off / 4); p32(e + 4, r.pal_off / 4); p32(e + 8, r.sz); p32(e + 12, r.pal_size); p32(e + 16, r.pal_size / 4); p32(e + 20, r.bpp == 4 ? 1 : 4);
            if (e + 0x40 <= c.sz) {
                d[e + 0x28] = (uint8_t)(31 - __builtin_clz((unsigned)r.dh)); d[e + 0x29] = (uint8_t)(31 - __builtin_clz((unsigned)r.dw)); d[e + 0x2B] = r.bpp == 4 ? 8 : 9;
                memset(d + e + 0x34, 0xFF, 12);
            }
            if (!k) { p32(8, r.pal_size / 4); p32(12, r.bpp == 4 ? 1 : 4); }
        }
        TexPng local;
        TexPng* t = tex_no_retain && tex_reg[q.id - 1].state == 0 ? tex_copy(q.id, local) : tex_load((uint32_t)q.id);
        if (!t) continue;
        int nc = t->bpp == 4 ? 16 : 256;
        std::vector<uint8_t> idx; std::vector<uint32_t> pal;
        if (t->w == t->width0 && t->h == t->height0 && !t->idx.empty() && (int)t->pal.size() <= nc) { idx = t->idx; pal = t->pal; }
        else img_index(t->px.data(), t->w, t->h, t->width0, t->height0, nc, idx, pal);
        if (t->dw != t->width0 || t->dh != t->height0) {
            std::vector<uint8_t> ext((size_t)t->dw * t->dh);
            for (int y = 0; y < t->dh; y++)
                for (int x = 0; x < t->dw; x++) ext[(size_t)y * t->dw + x] = idx[(size_t)std::min(y, t->height0 - 1) * t->width0 + std::min(x, t->width0 - 1)];
            idx.swap(ext);
        }
        if (t->off + t->sz <= c.sz) tex_write_indices(d + t->off, t->sz, idx, t->dw, t->dh, t->bpp);
        for (size_t k2 = 0; k2 < pal.size() && k2 * 4 + 4 <= t->pal_size; k2++)
            if (t->pal_off + k2 * 4 + 4 <= c.sz) memcpy(d + t->pal_off + k2 * 4, &pal[k2], 4);
        if (t->off + 16 > c.sz || t->sz < 16 || !tex_enabled) continue;
        uint32_t id = (uint32_t)q.id, chk = ~id;
        memcpy(d + t->off, TEX_MARKER, 8);
        memcpy(d + t->off + 8, &id, 4);
        memcpy(d + t->off + 12, &chk, 4);
    }
}

// PPVA table of the sibling .pphd: offset, rate and length per index (length 0 is an empty slot)
static bool pphd_table(const std::string& fpath, std::vector<Part>& table) {
    FILE* f = open_file(fpath, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { d.resize((size_t)n); if (fread(d.data(), 1, (size_t)n, f) != (size_t)n) d.clear(); }
    fclose(f);
    uint32_t sz = (uint32_t)d.size();
    if (sz < 0x20 || memcmp(d.data(), "PPHD", 4)) return false;
    std::vector<uint32_t> bloques;
    for (uint32_t pos = 0x10; pos + 4 <= sz; pos += 4) {
        uint32_t b; memcpy(&b, d.data() + pos, 4);
        if (b == 0xFFFFFFFFu || b >= sz) break;
        bloques.push_back(b);
    }
    bloques.push_back(sz);
    for (size_t k = 0; k + 1 < bloques.size(); k++) {
        uint32_t b = bloques[k], end_pos = bloques[k + 1];
        if (end_pos < b + 0x20 || memcmp(d.data() + b, "PPVA", 4)) continue;
        uint32_t declared; memcpy(&declared, d.data() + b + 4, 4);
        uint32_t cnt = (8 + declared - 0x20) / 16, fits = (end_pos - b - 0x20) / 16;
        if (declared < 0x18 || cnt > fits) cnt = fits;
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t* e = d.data() + b + 0x20 + i * 16;
            uint32_t o, fr, l; memcpy(&o, e, 4); memcpy(&fr, e + 4, 4); memcpy(&l, e + 8, 4);
            if (!o && !fr && !l) break;
            Part q; q.off = o; q.freq = fr; q.sz = l >= 0x80000000u ? 0 : l; q.idx = (int)i + 1; q.type = 3;
            table.push_back(q);
        }
        if (!table.empty()) return true;
    }
    return false;
}

// open vagp: the PPVA table of the sibling .pphd gives index, offset, rate and length; each wav in the folder
// pairs by its numeric prefix (base 0 if any prefix is 0, else base 1). An empty slot (length 0) whose wav exists
// is appended to the blob and the .pphd table is corrected on build
static bool list_vagp(Container& c, const std::string& key, const std::string& folder) {
    DScope ds_(9);
    c.type = 3; c.date = 0; c.sz = 0; c.parts.clear();
    size_t cut = folder.find_last_of("/\\");
    std::string parent = cut == std::string::npos ? "." : folder.substr(0, cut);
    std::string base = cut == std::string::npos ? folder : folder.substr(cut + 1);
    int pref = isdigit((unsigned char)base[0]) ? atoi(base.c_str()) : -1;
    std::vector<DirEntry> inp;
    list_dir(parent, inp);
    std::vector<Part> table;
    std::string chosen;
    for (auto& e : inp) {
        if (e.dir || !ends_with(e.name, ".pphd") || !isdigit((unsigned char)e.name[0])) continue;
        if (pref >= 0 && atoi(e.name.c_str()) == pref + 1) { chosen = e.name; break; }
    }
    if (!chosen.empty()) pphd_table(parent + "/" + chosen, table);
    if (table.empty())
        for (auto& e : inp) if (!e.dir && ends_with(e.name, ".pphd") && pphd_table(parent + "/" + e.name, table)) break;
    if (table.empty()) { printf("disc: %s sin tabla PPVA en un .pphd hermano\n", key.c_str()); return false; }
    std::vector<Part> wavs;
    list_tree(folder, "", wavs);
    std::map<int, std::string> by_prefix;
    bool base0 = false;
    for (auto& w : wavs) {
        if (w.type || !ends_with(w.fpath, ".wav")) continue;
        size_t s = w.fpath.find_last_of('/');
        const char* p = w.fpath.c_str() + (s == std::string::npos ? 0 : s + 1);
        if (!isdigit((unsigned char)*p)) continue;
        int pre = atoi(p);
        if (pre == 0) base0 = true;
        if (!by_prefix.count(pre)) by_prefix[pre] = w.fpath;
        else printf("disc: %s prefijo %d repetido, uso %s y no %s\n", key.c_str(), pre, by_prefix[pre].c_str(), w.fpath.c_str());
    }
    size_t marker_reg = sfx_reg.size();
    int missing = 0;
    std::vector<Part> gaps;
    std::map<uint32_t, int> freqs;
    uint32_t end_pos = 0;
    for (auto& t : table) {
        auto it = by_prefix.find(base0 ? t.idx - 1 : t.idx);
        std::string nm = it == by_prefix.end() ? "" : it->second;
        if (t.sz < 16) {
            if (!nm.empty()) { Part q = t; q.fpath = nm; gaps.push_back(q); }
            continue;
        }
        if (t.off + t.sz > end_pos) end_pos = t.off + t.sz;
        if (t.freq) freqs[t.freq]++;
        if (nm.empty()) { char tmp[48]; snprintf(tmp, sizeof(tmp), "%d-%X.wav", t.idx, t.idx); nm = tmp; missing++; }
        SfxWav w; w.fpath = folder + "/" + nm; w.name = nm; w.freq = t.freq; w.length = t.sz; sfx_reg.push_back(w);
        Part q = t; q.fpath = nm; q.id = (int)sfx_reg.size();
        c.parts.push_back(q);
    }
    c.sz = (end_pos + 63) & ~63u;
    uint32_t bank_freq = 11025; int times = 0;
    for (auto& kv : freqs) if (kv.second > times) { times = kv.second; bank_freq = kv.first; }
    for (auto& q : gaps) {
        if (!q.freq) q.freq = bank_freq;
        q.sz = sfx_wav_length(folder + "/" + q.fpath, q.freq);
        if (!q.sz) { printf("disc: %s hueco %d: no puedo leer %s\n", key.c_str(), q.idx, q.fpath.c_str()); continue; }
        q.off = (c.sz + 15) & ~15u;
        c.sz = q.off + q.sz;
        SfxWav w; w.fpath = folder + "/" + q.fpath; w.name = q.fpath; w.freq = q.freq; w.length = q.sz; sfx_reg.push_back(w);
        q.id = (int)sfx_reg.size();
        c.parts.push_back(q);
        printf("disc: %s hueco %d lleno con %s (%u bytes en 0x%X, %u Hz)\n", key.c_str(), q.idx, q.fpath.c_str(), q.sz, q.off, q.freq);
    }
    if (missing) printf("disc: %s %d de %d sonidos sin wav\n", key.c_str(), missing, (int)table.size());
    for (size_t k = marker_reg; k < sfx_reg.size(); k++) sfx_reg[k].pak = key;
    if (mod_touches(key)) {
        int nm = 0;
        for (size_t k = marker_reg; k < sfx_reg.size(); k++) if (mod_path(sfx_reg[k].fpath) != sfx_reg[k].fpath) nm++;
        printf("mods: sonidos %s: %d de %d desde mods, tabla %s\n", key.c_str(), nm, (int)(sfx_reg.size() - marker_reg), chosen.empty() ? "por busqueda" : (mod_path(parent + "/" + chosen) != parent + "/" + chosen ? "del mod" : "de assets"));
    }
    if (!c.sz) { printf("disc: %s sin sonidos\n", key.c_str()); return false; }
    return true;
}

// open txt1: N-HEX.txt in order. The exact size comes from the cache or is computed later in parallel
static std::vector<std::string> txt1_pending;

// the joined txt1 is named after its folder: sm_txt_ch01_es.txt1/sm_txt_ch01_es.txt (texto.txt accepted)
static std::string txt1_unique_name(const std::string& folder) {
    size_t b = folder.find_last_of("/\\");
    std::string n = b == std::string::npos ? folder : folder.substr(b + 1);
    if (ends_with(n, ".txt1")) n.resize(n.size() - 5);
    return n + ".txt";
}

static bool list_txt1(Container& c, const std::string& key, const std::string& folder) {
    DScope ds_(10);
    std::vector<std::pair<int, Part>> nums;
    int64_t date = 0;
    bool hdr = false;
    std::vector<DirEntry> inp;
    list_dir(folder, inp);
    std::string single; int64_t date_single = 0;
    std::string propio = txt1_unique_name(folder);
    for (auto& e : inp) {
        if (e.dir) continue;
        if (e.name == "_txt1.bin") { hdr = true; continue; }
        if (e.name == propio || (e.name == "texto.txt" && single.empty())) { single = e.name; date_single = e.date + (int64_t)e.sz * 7919; continue; }
        int num = 0; char hex[32];
        if (!ends_with(e.name, ".txt") || sscanf(e.name.c_str(), "%d-%31[0-9A-Fa-f].", &num, hex) != 2 || num <= 0) continue;
        date += e.date + (int64_t)e.sz * 7919;
        Part q; q.fpath = e.name; q.sz = (uint32_t)e.sz;
        nums.push_back({num, q});
    }
    std::sort(nums.begin(), nums.end(), [](const std::pair<int, Part>& a, const std::pair<int, Part>& b) { return a.first < b.first; });
    for (size_t k = 0; k < nums.size(); k++)
        if (nums[k].first != (int)k + 1) { printf("disc: %s salta de %d a %d (los ids son posiciones)\n", key.c_str(), (int)k, nums[k].first); return false; }
    if (!hdr && !txt1_info.count(lower(key))) { printf("disc: falta la cabecera de %s en txt1.txt\n", key.c_str()); return false; }
    if (!single.empty()) {
        c.type = 2; c.date = date_single; c.sz = 0; c.parts.clear();
        bool touches = mod_touches(key);
        auto it = disc_cache_dir.find(key);
        if (!disc_rescan && !touches && it != disc_cache_dir.end() && it->second.type == 2 && it->second.date == date_single && !it->second.parts.empty() && it->second.parts[0].fpath == single) {
            c.parts = it->second.parts;
            return true;
        }
        std::vector<uint8_t> d, u; std::vector<std::vector<uint8_t>> texts; std::string err;
        if (!read_file(folder + "/" + single, d) || !txt1_split(d, texts, &err)) { printf("disc: %s invalido en %s: %s\n", single.c_str(), key.c_str(), err.c_str()); return false; }
        for (auto& t : texts) { txt_decode(t, u); Part q; q.fpath = single; q.sz = (uint32_t)u.size(); c.parts.push_back(q); }
        if (!touches) { disc_cache_dir[key] = c; disc_cache_dirty = true; }
        return true;
    }
    c.type = 2; c.date = date; c.sz = 0; c.parts.clear();
    auto it = disc_cache_dir.find(key);
    if (!disc_rescan && !mod_touches(key) && it != disc_cache_dir.end() && it->second.type == 2 && it->second.date == date && it->second.parts.size() == nums.size()) {
        c.parts = it->second.parts;
        return true;
    }
    for (auto& q : nums) { Part p; p.fpath = q.second.fpath; c.parts.push_back(p); }
    txt1_pending.push_back(key);
    return true;
}

// open pak: its files in natural order form the entries; a .pak or .txt1 subfolder is one more entry
// a file in 1_p whose name does not start with a digit and whose header is a BT3 P file; type 6 parts are served from it
static void bt3_scan(Container& c, const std::string& key, const std::string& folder) {
    DScope ds_(11);
    {
        Bt3Lock g;
        { std::lock_guard<std::mutex> g2(bt3_of_mx); bt3_of.erase(key); }
        for (auto* m : {&bt3_tex_of, &bt3_hud_of})
            for (auto it = m->lower_bound(key + "/"); it != m->end() && it->first.compare(0, key.size() + 1, key + "/") == 0;) it = m->erase(it);
    }
    if (!patch_bt3_chars.val) return;
    int bi = -1; std::string real;
    for (size_t i = 0; i < c.parts.size() && bi < 0; i++) {
        const Part& q = c.parts[i];
        if (q.type || q.sz < 0x1000 || q.fpath.size() < 5 || lower(q.fpath.substr(0, 4)) != "1_p/") continue;
        std::string b = q.fpath.substr(4);
        if (isdigit((unsigned char)b[0]) || b.find('/') != std::string::npos) continue;
        std::string r = mod_path(folder + "/" + q.fpath);
        if (r == folder + "/" + q.fpath) r = link_real(r);
        uint8_t h[16];
        bool ok = head_bytes(r, h, 16) == 16 && bt3::is_p(h, 16);
        if (ok) { bi = (int)i; real = r; }
    }
    std::string name;
    bool opened = bi < 0;
    if (opened) {   // opened P file: its files straight in 1_p (tools/bt3_open.py); BT3 when 002 is not a TTT pMdl
        if (!mod_touches(key)) return;
        for (auto& q : c.parts) {
            if (q.type || q.fpath.size() < 8 || lower(q.fpath.substr(0, 4)) != "1_p/" || atoi(q.fpath.c_str() + 4) != 2 || !isdigit((unsigned char)q.fpath[4]) || q.fpath.find('/', 4) != std::string::npos) continue;
            std::string r = mod_path(folder + "/" + q.fpath);
            if (r == folder + "/" + q.fpath) r = link_real(r);
            uint8_t h[4] = {0, 0, 0, 0};
            int got = head_bytes(r, h, 4);
            if (got < 0) continue;
            bool rd = got == 4;
            if (rd && memcmp(h, "pMdl", 4)) { real = r.substr(0, r.find_last_of('/')); name = "1_p"; }
            break;
        }
        if (real.empty()) return;
        // the .dbt folders are read as BT3 textures, never as pak parts
        c.parts.erase(std::remove_if(c.parts.begin(), c.parts.end(), [&](const Part& q) {
            std::string l = lower(q.fpath);
            size_t k = l.find(".dbt/");
            return l.compare(0, 4, "1_p/") == 0 && k != std::string::npos;
        }), c.parts.end());
    } else {
        name = c.parts[bi].fpath.substr(4);
        c.parts.erase(c.parts.begin() + bi);
    }
    int rank = mod_rank(real), n = 0, kept = 0, huds = 0, hud_cnt = 0;
    Bt3Size* sc = bt3_size_of("P", real, true);
    if (sc && sc->hud >= 0) hud_cnt = sc->hud;
    else {
        Bt3Lock g;
        Bt3Data* b = bt3_data(real);
        if (b) hud_cnt = b->tk.count[0];
        if (sc) sc->hud = hud_cnt;
    }
    std::string tex_key;
    std::vector<std::string> hud_keys;
    for (auto& q : c.parts) {
        if (q.fpath.size() < 5 || lower(q.fpath.substr(0, 4)) != "1_p/" || !isdigit((unsigned char)q.fpath[4])) continue;
        int num = atoi(q.fpath.c_str() + 4);
        if (num == 11 && q.type == 4) tex_key = key + "/" + q.fpath;
        if ((num == 45 || num == 47 || num == 49) && q.type == 4 && hud_cnt > 0) {   // HUD portraits: image 1 of the BT3 000_hud.dbt
            std::string f = folder + "/" + q.fpath;
            if (std::min(mod_rank(mod_path(f)), mod_rank(mod_path(f + ".png"))) < rank) kept++;
            else { hud_keys.push_back(key + "/" + q.fpath); huds++; }
            continue;
        }
        if (q.type || !bt3::provides(num, patch_bt3_chars.val) || (opened && num == 43)) continue;   // opened: the move list is the TTT 043 + txt
        if (mod_rank(mod_path(folder + "/" + q.fpath)) < rank) { kept++; continue; }
        auto cs = sc ? sc->num.find(num) : std::map<int, uint32_t>::iterator();
        uint32_t size = 0;
        if (sc && cs != sc->num.end()) size = cs->second;
        else {
            std::vector<uint8_t>* o;
            if (!bt3_bytes(real, num, &o)) { printf("disc: %s: %s no se pudo leer la entrada %d\n", key.c_str(), name.c_str(), num); continue; }
            size = (uint32_t)o->size();
            if (sc) { sc->num[num] = size; bt3_sizes_dirty = true; }
        }
        q.type = 6; q.id = num; q.sz = size; n++;
        bt3_warm_p.push_back({real, num});
    }
    {
        Bt3Lock g;
        { std::lock_guard<std::mutex> g2(bt3_of_mx); bt3_of[key] = {real, rank}; }
        if (!tex_key.empty()) bt3_tex_of[tex_key] = {real, rank};
        for (auto& k : hud_keys) bt3_hud_of[k] = {real, rank};
    }
    static int logs = 0;
    if (logs++ < 40) printf("disc: %s: personaje BT3 %s, %d entradas leidas de BT3, %d retratos del HUD%s\n", key.c_str(), name.c_str(), n, huds, kept ? (", " + std::to_string(kept) + " de un mod con mas prioridad").c_str() : "");
}

// BT3 character files read in place: a closed <name>_anm.pak or <name>_eff.pak dropped in 1_p, 2_anims or 3_effects,
// and the opened files straight in the TTT folders (tools/bt3_open.py): .canm in 2_anims, little-endian VFX paks in 3_effects
static size_t bt3_section(const std::string& fpath) {
    std::string l = lower(fpath);
    return l.compare(0, 4, "1_p/") == 0 ? 4 : l.compare(0, 8, "2_anims/") == 0 ? 8 : l.compare(0, 10, "3_effects/") == 0 ? 10 : 0;
}
static std::string bt3_real(const std::string& folder, const std::string& fpath) {
    std::string r = mod_path(folder + "/" + fpath);
    return r == folder + "/" + fpath ? link_real(r) : r;
}
// a non-numbered file of 1_p, 2_anims or 3_effects whose header passes ok(); removed from the parts
static bool bt3_closed(Container& c, const std::string& folder, bool (*ok)(const uint8_t*, size_t), std::string& real, std::string& name) {
    for (size_t i = 0; i < c.parts.size(); i++) {
        const Part& q = c.parts[i];
        size_t s = bt3_section(q.fpath);
        if (q.type || q.sz < 0x100 || !s || q.fpath.size() <= s || isdigit((unsigned char)q.fpath[s]) || q.fpath.find('/', s) != std::string::npos) continue;
        std::string r = bt3_real(folder, q.fpath);
        uint8_t h[16];
        bool good = head_bytes(r, h, 16) == 16 && ok(h, 16);
        if (!good) continue;
        real = r; name = q.fpath.substr(s);
        c.parts.erase(c.parts.begin() + i);
        return true;
    }
    return false;
}

struct Bt3Anm { uint64_t sz = 0; int64_t date = 0; bool ok = false; std::vector<std::vector<uint8_t>> out; int conv = 0, empty = 0, bad = 0; double ms = 0; };
static std::map<std::string, Bt3Anm> bt3_anm_cache;
static std::map<std::string, Bt3Src> bt3_anm_of;

static void bt3_parallel(size_t n, const std::function<void(size_t)>& job) {
    std::atomic<size_t> next(0);
    auto run = [&]() { for (size_t i; (i = next++) < n;) job(i); };
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned k = 1; k < nt && k < n; k++) th.emplace_back(run);
    run();
    for (auto& t : th) t.join();
}

static Bt3Anm* bt3_anm_data(const std::string& real) {   // bt3_mx held
    std::error_code ec;
    auto p = std::filesystem::u8path(real);
    uint64_t sz = (uint64_t)std::filesystem::file_size(p, ec);
    int64_t date = ec ? 0 : (int64_t)std::filesystem::last_write_time(p, ec).time_since_epoch().count();
    Bt3Anm& b = bt3_anm_cache[real];
    if (b.ok && b.sz == sz && b.date == date) return &b;
    b = Bt3Anm(); b.sz = sz; b.date = date;
    std::vector<uint8_t> d;
    if (!read_file(real, d, true) || !bt3::is_anm(d.data(), d.size())) return nullptr;
    auto t0 = std::chrono::steady_clock::now();
    b.out.assign(bt3::ANM_COUNT, {});
    std::vector<int> kinds(bt3::ANM_COUNT, 0);
    bt3_parallel(bt3::ANM_COUNT, [&](size_t i) {
        const uint8_t* e = nullptr; uint32_t n = 0;
        if (bt3::pak_entry(d, (uint32_t)i, &e, &n)) kinds[i] = bt3::anim_tanm(e, n, false, b.out[i]);
    });
    for (int k : kinds) { if (k == 1) b.conv++; if (!k) b.empty++; if (k < 0) b.bad++; }
    b.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    b.ok = true;
    return &b;
}

static void bt3_anm_scan(Container& c, const std::string& key, const std::string& folder) {
    DScope ds_(12);
    { Bt3Lock g; bt3_anm_of.erase(key); }
    std::string real, name;
    if (!patch_bt3_chars.val || !bt3_closed(c, folder, bt3::is_anm, real, name)) return;
    int rank = mod_rank(real), n = 0, kept = 0;
    std::vector<uint32_t> sizes;
    double ms = 0; int conv = 0, empty = 0, bad = 0;
    {
        Bt3Lock g;
        Bt3Anm* b = bt3_anm_data(real);
        if (!b) { printf("disc: %s: %s no es un pak de animaciones BT3 valido\n", key.c_str(), name.c_str()); return; }
        for (auto& o : b->out) sizes.push_back((uint32_t)o.size());
        ms = b->ms; conv = b->conv; empty = b->empty; bad = b->bad;
        bt3_anm_of[key] = {real, rank};
    }
    for (auto& q : c.parts) {
        if (q.type || q.fpath.size() < 9 || lower(q.fpath.substr(0, 8)) != "2_anims/" || !isdigit((unsigned char)q.fpath[8])) continue;
        int num = atoi(q.fpath.c_str() + 8);
        if (num < 0 || num >= (int)bt3::ANM_COUNT) continue;
        if (mod_rank(mod_path(folder + "/" + q.fpath)) < rank) { kept++; continue; }
        q.type = 7; q.id = num; q.sz = sizes[num]; n++;
    }
    static int logs = 0;
    if (logs++ < 40) printf("disc: %s: animaciones BT3 %s, %d servidas, %d convertidas de canm, %d vacias, en %.0f ms%s%s\n", key.c_str(), name.c_str(), n, conv, empty, ms,
                            bad ? (", " + std::to_string(bad) + " ilegibles").c_str() : "", kept ? (", " + std::to_string(kept) + " de un mod con mas prioridad").c_str() : "");
}

struct Bt3Eff { uint64_t sz = 0; int64_t date = 0; bool ok = false; std::vector<uint8_t> out[7]; bt3::EffStats st; double ms = 0; };
static std::map<std::string, Bt3Eff> bt3_eff_cache;
static std::map<std::string, Bt3Src> bt3_eff_of;

static Bt3Eff* bt3_eff_data(const std::string& real) {   // bt3_mx held
    std::error_code ec;
    auto p = std::filesystem::u8path(real);
    uint64_t sz = (uint64_t)std::filesystem::file_size(p, ec);
    int64_t date = ec ? 0 : (int64_t)std::filesystem::last_write_time(p, ec).time_since_epoch().count();
    Bt3Eff& b = bt3_eff_cache[real];
    if (b.ok && b.sz == sz && b.date == date) return &b;
    b = Bt3Eff(); b.sz = sz; b.date = date;
    std::vector<uint8_t> d;
    auto t0 = std::chrono::steady_clock::now();
    if (!read_file(real, d, true) || !bt3::eff_entries(d, b.out, b.st)) return nullptr;
    b.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    b.ok = true;
    return &b;
}

static void bt3_eff_scan(Container& c, const std::string& key, const std::string& folder) {
    DScope ds_(13);
    { Bt3Lock g; bt3_eff_of.erase(key); }
    std::string real, name;
    if (!patch_bt3_chars.val || !bt3_closed(c, folder, bt3::is_eff, real, name)) return;
    int rank = mod_rank(real), n = 0, kept = 0;
    uint32_t sizes[7];
    bt3::EffStats st; double ms;
    {
        Bt3Lock g;
        Bt3Eff* b = bt3_eff_data(real);
        if (!b) { printf("disc: %s: %s no es un pak de efectos BT3 valido\n", key.c_str(), name.c_str()); return; }
        for (int k = 0; k < 7; k++) sizes[k] = (uint32_t)b->out[k].size();
        st = b->st; ms = b->ms;
        bt3_eff_of[key] = {real, rank};
    }
    for (auto& q : c.parts) {
        if (q.fpath.size() < 11 || lower(q.fpath.substr(0, 10)) != "3_effects/" || !isdigit((unsigned char)q.fpath[10]) || q.fpath.find('/', 10) != std::string::npos) continue;
        int num = atoi(q.fpath.c_str() + 10);
        if (num < 0 || num > 6) continue;
        if (mod_rank(mod_path(folder + "/" + q.fpath)) < rank) { kept++; continue; }
        q.type = 8; q.id = num; q.sz = sizes[num]; n++;
    }
    static int logs = 0;
    if (logs++ < 40) printf("disc: %s: efectos BT3 %s, %d entradas servidas, %d paks de VFX, %d texturas dbt a atex%s%s%s en %.0f ms\n", key.c_str(), name.c_str(), n, st.paks, st.tex,
                            st.models ? (", " + std::to_string(st.models) + " modelos de VFX traducidos").c_str() : "", st.bad ? (", " + std::to_string(st.bad) + " con error").c_str() : "",
                            kept ? (", " + std::to_string(kept) + " de un mod con mas prioridad").c_str() : "", ms);
}

// opened BT3 files, one by one (any mix with TTT files): .canm -> .tanm; a VFX pak of 3_effects in little endian -> TTT
// (BE, .dbt -> .atex); a closed *_effect_common.pak in little endian -> TTT; an empty VFX pak -> TTT's pak of one empty entry
struct Bt3Conv { uint64_t sz = 0; int64_t date = 0; int kind = 0; bool ok = false; std::vector<uint8_t> out; bt3::EffStats st; };
static std::map<std::string, Bt3Conv> bt3_conv_cache;

static Bt3Conv* bt3_conv(const std::string& real, int kind) {   // bt3_mx held for the cache map only by the callers
    std::error_code ec;
    auto p = std::filesystem::u8path(real);
    uint64_t sz = (uint64_t)std::filesystem::file_size(p, ec);
    int64_t date = ec ? 0 : (int64_t)std::filesystem::last_write_time(p, ec).time_since_epoch().count();
    Bt3Conv& b = bt3_conv_cache[real];
    if (b.ok && b.sz == sz && b.date == date && b.kind == kind) return &b;
    return nullptr;
}
static void bt3_conv_make(const std::string& real, int kind, Bt3Conv& b) {
    std::error_code ec;
    auto p = std::filesystem::u8path(real);
    b = Bt3Conv(); b.kind = kind;
    b.sz = (uint64_t)std::filesystem::file_size(p, ec);
    b.date = ec ? 0 : (int64_t)std::filesystem::last_write_time(p, ec).time_since_epoch().count();
    std::vector<uint8_t> d;
    if (!read_file(real, d, true)) return;
    if (kind == 1) b.ok = bt3::anim_tanm(d.data(), (uint32_t)d.size(), false, b.out) >= 0;
    else if (kind == 2) b.ok = bt3::vfx_pak(d.data(), (uint32_t)d.size(), b.out, b.st);
    else if (kind == 3) b.ok = bt3::common_pak(d.data(), (uint32_t)d.size(), b.out, b.st);
    static int logs = 0;
    if (b.st.models && logs++ < 40) printf("disc: %s: %d modelos de VFX traducidos%s\n", real.c_str(), b.st.models, b.st.bad ? ", con errores" : "");
}

static int bt3_file_kind(const std::string& key, const Part& q, const std::string& real) {
    std::string l = lower(q.fpath), k = lower(key) + "/" + l;
    if (ends_with(l, ".canm")) return 1;
    if (!ends_with(l, ".pak") || k.find("/3_effects/") == std::string::npos) return 0;
    uint8_t h[8];
    if (!q.sz) return 2;
    if (q.sz < 8) return 0;
    bool rd = head_bytes(real, h, 8) == 8;
    uint32_t le = bt3::u32(h), be = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    if (!rd || le >= be) return 0;
    return ends_with(l, "_effect_common.pak") ? 3 : 2;
}

// a container inside a new character's colour that takes what it lacks from its colour 1 (link): the mod files are colour 1's
static bool bt3_linked(const std::string& key) {
    std::string k = key;
    for (size_t p; (p = k.find_last_of('/')) != std::string::npos;) {
        k.resize(p);
        if (link_to.count(link_key(disc_dir + "/Packfile/" + k)) || link_to.count(link_key(disc_dir + "/Packfile/" + k + ".pak"))) return true;
    }
    return false;
}
static void bt3_file_scan(Container& c, const std::string& key, const std::string& folder) {
    DScope ds_(14);
    if (!patch_bt3_chars.val || (!mod_touches(key) && !bt3_linked(key))) return;
    std::vector<size_t> idx; std::vector<std::string> reals; std::vector<int> kinds;
    for (size_t i = 0; i < c.parts.size(); i++) {
        const Part& q = c.parts[i];
        if (q.type) continue;
        std::string l = lower(q.fpath);
        if (!ends_with(l, ".canm") && !ends_with(l, ".pak")) continue;
        std::string r = bt3_real(folder, q.fpath);
        int kd = bt3_file_kind(key, q, r);
        if (kd) { idx.push_back(i); reals.push_back(r); kinds.push_back(kd); }
    }
    if (idx.empty()) return;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<Bt3Conv> made(idx.size());
    std::vector<char> have(idx.size(), 0);
    int cached = 0;
    for (size_t j = 0; j < idx.size(); j++) {   // sizes already known: converted later, when read
        bt3_warm_c.push_back({reals[j], kinds[j]});
        Bt3Size* z = bt3_size_of("C" + std::to_string(kinds[j]), reals[j], false);
        if (!z || !z->num.count(0)) continue;
        made[j].ok = true; made[j].kind = kinds[j]; made[j].out.clear(); have[j] = 2; cached++;
        Part& q = c.parts[idx[j]];
        q.type = 9; q.id = kinds[j]; q.sz = z->num[0];
    }
    {
        Bt3Lock g;
        for (size_t j = 0; j < idx.size(); j++) { if (have[j]) continue; Bt3Conv* b = bt3_conv(reals[j], kinds[j]); if (b) { made[j] = *b; have[j] = 1; } }
    }
    bt3_parallel(idx.size(), [&](size_t j) { if (!have[j]) bt3_conv_make(reals[j], kinds[j], made[j]); });
    int anm = 0, vfx = 0, tex = 0, bad = 0;
    {
        Bt3Lock g;
        for (size_t j = 0; j < idx.size(); j++) {
            if (have[j] == 2) continue;
            if (!have[j]) bt3_conv_cache[reals[j]] = made[j];
            Part& q = c.parts[idx[j]];
            if (!made[j].ok) { bad++; continue; }
            q.type = 9; q.id = kinds[j]; q.sz = (uint32_t)made[j].out.size();
            if (Bt3Size* z = bt3_size_of("C" + std::to_string(kinds[j]), reals[j], true)) { z->num[0] = q.sz; z->hud = 0; bt3_sizes_dirty = true; }
            if (kinds[j] == 1) anm++; else { vfx += std::max(1, made[j].st.paks); tex += made[j].st.tex; }
        }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static int logs = 0;
    if (cached == (int)idx.size()) return;
    if (logs++ < 60) printf("disc: %s: archivos BT3, %d animaciones canm, %d paks de VFX, %d texturas dbt a atex%s en %.0f ms\n", key.c_str(), anm, vfx, tex,
                            bad ? (", " + std::to_string(bad) + " con error").c_str() : "", ms);
}

// 1_p_dmg of a colour: TTT files (002_model.pmdl...) replace the same numbers of 1_p; a BT3 P (closed file or opened
// with tools/bt3_open.py) gives 2..16 converted, the main textures by material key as in bt3_scan
static void dmg_scan(Container& c, const std::string& key, const std::string& folder) {
    std::vector<Part> dp;
    list_tree(folder, "1_p_dmg", dp);
    std::string real, name = "TTT";
    for (auto& q : dp) {
        std::string b = q.fpath.substr(8);
        if (q.type || q.sz < 0x1000 || b.empty() || isdigit((unsigned char)b[0]) || b.find('/') != std::string::npos) continue;
        std::string r = bt3_real(folder, q.fpath);
        uint8_t h[16];
        if (head_bytes(r, h, 16) == 16 && bt3::is_p(h, 16)) { real = r; name = b; break; }
    }
    if (real.empty())
        for (auto& q : dp) {
            if (q.type || q.fpath.size() < 12 || q.fpath.compare(8, 4, "002_") || q.fpath.find('/', 8) != std::string::npos) continue;
            std::string r = bt3_real(folder, q.fpath);
            uint8_t h[4] = {0, 0, 0, 0};
            if (head_bytes(r, h, 4) == 4 && memcmp(h, "pMdl", 4)) { real = r.substr(0, r.find_last_of('/')); name = "1_p_dmg"; }
            break;
        }
    int n = 0;
    auto num_of = [](const std::string& f, size_t s) { return f.size() > s + 3 && isdigit((unsigned char)f[s]) && isdigit((unsigned char)f[s + 1]) && isdigit((unsigned char)f[s + 2]) && f.find('/', s) == std::string::npos ? atoi(f.c_str() + s) : -1; };
    if (real.empty()) {
        std::map<int, const Part*> by;
        for (auto& q : dp) { int k = num_of(q.fpath, 8); if (k >= 2 && k <= 16) by[k] = &q; }
        for (auto& q : c.parts) {
            if (q.fpath.size() < 5 || lower(q.fpath.substr(0, 4)) != "1_p/") continue;
            auto it = by.find(num_of(q.fpath, 4));
            if (it == by.end()) continue;
            q.fpath = it->second->fpath; q.sz = it->second->sz; q.type = it->second->type; q.id = 0; q.idx = 0; n++;
        }
    } else {
        int rank = mod_rank(real);
        Bt3Size* sc = bt3_size_of("P", real, true);
        for (auto& q : c.parts) {
            if (q.fpath.size() < 5 || lower(q.fpath.substr(0, 4)) != "1_p/") continue;
            int num = num_of(q.fpath, 4);
            if (num < 2 || num > 16) continue;
            if (num == 11 && q.type == 4) { Bt3Lock g; bt3_tex_of[key + "/" + q.fpath] = {real, rank}; n++; continue; }
            if ((q.type && q.type != 6 && q.type != 10) || !bt3::provides(num, patch_bt3_chars.val)) continue;
            auto cs = sc ? sc->num.find(num) : std::map<int, uint32_t>::iterator();
            uint32_t size = 0;
            if (sc && cs != sc->num.end()) size = cs->second;
            else {
                std::vector<uint8_t>* o;
                if (!bt3_bytes(real, num, &o)) { printf("disc: %s: %s no se pudo leer la entrada %d\n", key.c_str(), name.c_str(), num); continue; }
                size = (uint32_t)o->size();
                if (sc) { sc->num[num] = size; bt3_sizes_dirty = true; }
            }
            q.type = 6; q.id = num; q.idx = 1; q.sz = size; n++;
            bt3_warm_p.push_back({real, num});
        }
        Bt3Lock g;
        bt3_dmg_of[key] = {real, rank};
    }
    if (!n) printf("disc: %s: 1_p_dmg sin entradas utiles\n", key.c_str());
}

static bool list_pak(Container& c, const std::string& key, const std::string& folder) {
    DScope ds_(15);
    std::error_code ec;
    std::filesystem::path base = std::filesystem::u8path(link_real(folder));
    int64_t date = date_of(base) + link_dates(folder);
    c.type = 1; c.date = date; c.sz = 0; c.parts.clear();
    auto it = disc_cache_dir.find(key);
    bool touches = mod_touches(key);
    if (!disc_rescan && !touches && it != disc_cache_dir.end() && it->second.type == 1 && it->second.date == date && date) {
        c.parts = it->second.parts;
    } else {
        DScope ds_t(21);
        if (touches) dprof[25].n++;
        list_tree(folder, "", c.parts);
        sort_parts(c.parts);
        if (!touches) { disc_cache_dir[key] = c; disc_cache_dirty = true; }
    }
    bt3_scan(c, key, folder);
    bt3_anm_scan(c, key, folder);
    bt3_eff_scan(c, key, folder);
    bt3_file_scan(c, key, folder);
    if (dmg_key(key)) dmg_scan(c, key, folder);
    for (auto& q : c.parts) {
        if (!q.type || q.type >= 6) continue;
        Container child;
        if (!list_container(child, key + "/" + q.fpath, folder + "/" + q.fpath, q.type)) return false;
    }
    return true;
}

static bool list_container(Container& c, const std::string& key, const std::string& folder, int type) {
    bool ok = type == 4 ? list_tex(c, key, folder) : type == 3 ? list_vagp(c, key, folder) : type == 2 ? list_txt1(c, key, folder) : list_pak(c, key, folder);
    if (ok) disc_containers[key] = c;
    return ok;
}

// decodes the .txt of uncached txt1 across threads; only happens the first time
static void resolve_txt1(const std::string& root_d) {
    if (txt1_pending.empty()) return;
    std::atomic<size_t> next_one(0);
    unsigned threads = std::thread::hardware_concurrency();
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    auto job = [&]() {
        std::vector<uint8_t> d, u;
        for (;;) {
            size_t k = next_one++;
            if (k >= txt1_pending.size()) return;
            auto it = disc_containers.find(txt1_pending[k]);
            if (it == disc_containers.end()) continue;
            Container& c = it->second;
            std::string folder = root_d + txt1_pending[k];
            for (auto& q : c.parts) {
                if (!read_file(folder + "/" + q.fpath, d)) { printf("disc: no puedo leer %s/%s\n", txt1_pending[k].c_str(), q.fpath.c_str()); continue; }
                txt_decode(d, u);
                q.sz = (uint32_t)u.size();
            }
        }
    };
    std::vector<std::thread> group;
    for (unsigned h = 1; h < threads; h++) group.emplace_back(job);
    job();
    for (auto& t : group) t.join();
    for (auto& key : txt1_pending) { if (mod_touches(key)) continue; if (const Container* c = container_of(key)) { disc_cache_dir[key] = *c; disc_cache_dirty = true; } }
    txt1_pending.clear();
}

static uint32_t compute_size(const std::string& key) {
    Container& c = disc_containers[key];
    if (c.sz) return c.sz;
    if (c.type == 3 || c.type == 4 || c.type == 5) return c.sz;
    if (c.type == 2) {
        c.sz = TXT1_HDR + 4 * (uint32_t)c.parts.size();
        for (auto& q : c.parts) c.sz += q.sz;
        return c.sz;
    }
    uint32_t n = (uint32_t)c.parts.size();
    uint32_t pos = (4 + 4 * (n + 1) + ALIN_PAK - 1) / ALIN_PAK * ALIN_PAK;
    for (auto& q : c.parts) {
        if (q.type && q.type < 6) q.sz = compute_size(key + "/" + q.fpath);
        pos = (pos + q.sz + ALIGN_ENTRY - 1) / ALIGN_ENTRY * ALIGN_ENTRY;
    }
    c.sz = pos;
    return c.sz;
}

static bool pak_folder(PkEntry& x, const std::string& folder, const std::string& key, int type) {
    DScope ds_(16);
    if (!is_dir(folder) && !(type == 4 && tex_loose(folder))) return false;
    Container c;
    if (!list_container(c, key, folder, type)) return false;
    x.key = key;
    x.folder = true;
    return true;
}

// builds the container in memory (canonical form, same as tools/open_paks.py); nested ones are built in place
// an ATRAC3 .unk entry is served from the .wav with the same name; only music is stereo
static std::map<std::string, std::pair<int, int>> mod_audio;   // folder -> voices from mods, voices
static std::map<std::string, int> mod_audio_empty;
// the voice packs' wavs of an entry: their ids, and the entry as long as its longest language (a shorter one ends in silence)
static bool voice_alts(const std::string& key, uint32_t id, uint32_t* sz) {
    if (voice_packs.empty()) return false;
    std::string rel = lower(key.substr(0, key.find_last_of('.'))) + ".wav";
    std::vector<uint32_t> alt(voice_packs.size(), 0);
    bool any = false;
    for (size_t i = 0; i < voice_packs.size(); i++) {
        auto it = voice_packs[i].wav.find(rel);
        if (it == voice_packs[i].wav.end()) continue;
        int aid = 0;
        uint32_t asz = at3_register(key, it->second, key.compare(0, 6, "Music/") == 0, &aid);
        if (!asz) { printf("mods: %s: %s no es un wav valido\n", voice_packs[i].id.c_str(), it->second.c_str()); continue; }
        alt[i] = (uint32_t)aid; any = true;
        *sz = std::max(*sz, asz);
    }
    if (any) at3_alt[id] = alt;
    return any;
}
// an entry the game leaves empty but a voice pack fills: silent with the game's own voices (at3_only)
static bool voice_only(PkEntry& x, const std::string& key) {
    if (voice_packs.empty()) return false;
    std::string rel = lower(key.substr(0, key.find_last_of('.'))) + ".wav";
    size_t k = 0;
    while (k < voice_packs.size() && !voice_packs[k].wav.count(rel)) k++;
    if (k == voice_packs.size()) return false;
    int id = 0;
    uint32_t sz = at3_register(key, voice_packs[k].wav[rel], key.compare(0, 6, "Music/") == 0, &id);
    if (!sz) return false;
    voice_alts(key, (uint32_t)id, &sz);
    at3_only.insert((uint32_t)id);
    Container c;
    c.type = 5; c.sz = sz;
    Part q; q.type = 5; q.id = id; q.sz = sz; q.fpath = voice_packs[k].wav[rel];
    c.parts.push_back(q);
    disc_containers[key] = c;
    x.key = key; x.folder = true;
    return true;
}
static bool at3_entry(PkEntry& x, const std::string& fpath, const std::string& key, bool* empty) {
    DScope ds_(17);
    size_t p = fpath.find_last_of("./\\");
    if (p == std::string::npos || fpath[p] != '.') return false;
    std::string wav = fpath.substr(0, p) + ".wav";
    if (!is_file(wav)) return voice_only(x, key);
    int id = 0;
    uint32_t sz = at3_register(key, wav, key.compare(0, 6, "Music/") == 0, &id, empty);
    if (mod_enabled) { auto& m = mod_audio[key.substr(0, key.find_last_of('/'))]; m.second++; if (mod_path(wav) != wav) m.first++; }
    if (*empty) { if (mod_enabled && mod_path(wav) != wav) mod_audio_empty[key.substr(0, key.find_last_of('/'))]++; if (voice_only(x, key)) { *empty = false; return true; } return false; }
    if (!sz && mod_path(wav) != wav) {   // a broken mod file never takes the whole disc down: the entry falls back to assets
        printf("mods: voz %s: el wav del mod no es valido, uso el de assets\n", key.c_str());
        std::string rel;
        if (mod_rel(wav, rel)) mod_files.erase(rel);
        mod_audio[key.substr(0, key.find_last_of('/'))].first--;
        if (is_file(wav)) sz = at3_register(key, wav, key.compare(0, 6, "Music/") == 0, &id);
    }
    if (!sz) return false;
    voice_alts(key, (uint32_t)id, &sz);
    Container c;
    c.type = 5; c.sz = sz;
    Part q; q.type = 5; q.id = id; q.sz = sz; q.fpath = wav;
    c.parts.push_back(q);
    disc_containers[key] = c;
    x.key = key; x.folder = true;
    return true;
}

static std::atomic<uint64_t> disc_built(0), disc_ms(0);
static std::atomic<int64_t> disc_last_read(0);
#ifndef DISC_READING_EXTERN
static std::atomic<int> disc_reading(0);
#endif
static int disc_trace = 0;

static void build_container(const Container& c, const std::string& key, const std::string& folder, uint8_t* d, bool paralelo);
static void pphd_patch(const Container& pak, const std::string& key, uint8_t* d, uint32_t sz);

// the built txt1 is saved in assets/cache/txt1 with its date: next time it is a single read
static std::string txt1_cache_path(const std::string& key) {
    std::string nm = key;
    for (auto& ch : nm) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
    return disc_dir + "/cache/txt1/" + nm + ".bin";
}

static bool txt1_cache_read(const Container& c, const std::string& key, uint8_t* d) {
    FILE* f = fopen(txt1_cache_path(key).c_str(), "rb");
    if (!f) return false;
    int64_t date = 0; uint32_t sz = 0;
    bool ok = fread(&date, 8, 1, f) == 1 && fread(&sz, 4, 1, f) == 1 && date == c.date && sz == c.sz && fread(d, 1, sz, f) == sz;
    fclose(f);
    return ok;
}

static bool cache_replace(const std::string& tmp, const std::string& dst) {   // rename over an existing file; Windows refuses while a reader holds it open
    std::error_code ec;
    for (int k = 0; k < 3; k++) {
        std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(dst), ec);
        if (!ec) return true;
        std::error_code e2;
        std::filesystem::remove(std::filesystem::u8path(dst), e2);
        if (k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    static std::atomic<int> logs{0};
    if (logs++ < 20) { printf("disc: no se pudo reemplazar la cache %s: %s\n", dst.c_str(), ec.message().c_str()); fflush(stdout); }
    remove(tmp.c_str());
    return false;
}
static void txt1_cache_write(const Container& c, const std::string& key, const uint8_t* d) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache/txt1"), ec);
    std::string fpath = txt1_cache_path(key), tmp = fpath + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(&c.date, 8, 1, f) == 1 && fwrite(&c.sz, 4, 1, f) == 1 && fwrite(d, 1, c.sz, f) == c.sz;
    fclose(f);
    if (ok) cache_replace(tmp, fpath);
    else remove(tmp.c_str());
}

static void build_txt1(const Container& c, const std::string& key, const std::string& folder, uint8_t* d) {
    bool touches = mod_touches(key);
    if (!touches && txt1_cache_read(c, key, d)) return;
    uint32_t n = (uint32_t)c.parts.size();
    std::vector<uint8_t> hdr;
    bool own = read_file(folder + "/_txt1.bin", hdr);
    hdr.resize(TXT1_HDR, 0);
    if (own || !txt1_header_make(key, n, hdr.data())) {
        uint16_t base = hdr[2] | (hdr[3] << 8), last_one = hdr[4] | (hdr[5] << 8), old = (uint16_t)(last_one - base + 1);
        for (uint32_t pos = 4; pos + 1 < TXT1_HDR; pos += 2) {
            uint16_t v = hdr[pos] | (hdr[pos + 1] << 8), nv = v;
            if (pos == 4 || v == last_one) nv = (uint16_t)(base + n - 1);
            else if (v == old) nv = (uint16_t)n;
            hdr[pos] = nv & 255; hdr[pos + 1] = nv >> 8;
        }
    }
    memcpy(d, hdr.data(), TXT1_HDR);
    std::vector<uint32_t> offs(n + 1);
    offs[0] = TXT1_HDR + 4 * n;
    for (uint32_t k = 0; k < n; k++) { memcpy(d + TXT1_HDR + 4 * k, &offs[k], 4); offs[k + 1] = offs[k] + c.parts[k].sz; }
    if (n && (c.parts[0].fpath == "texto.txt" || c.parts[0].fpath == txt1_unique_name(folder))) {
        std::vector<uint8_t> f, u; std::vector<std::vector<uint8_t>> texts; std::string err;
        if (!read_file(folder + "/" + c.parts[0].fpath, f) || !txt1_split(f, texts, &err)) printf("disc: no puedo leer %s de %s: %s\n", c.parts[0].fpath.c_str(), key.c_str(), err.c_str());
        for (uint32_t k = 0; k < n; k++) {
            if (k < texts.size()) txt_decode(texts[k], u); else u.assign(2, 0);
            uint32_t gap = c.parts[k].sz;
            if (u.size() != gap) {
                if (u.size() > gap) printf("disc: %s [%u] crecio desde el arranque, se recorta (usa TTT_DISC_RESCAN=1)\n", key.c_str(), k + 1);
                u.resize(gap, 0);
                if (gap >= 2) { u[gap - 2] = 0; u[gap - 1] = 0; }
            }
            if (offs[k] + gap <= c.sz) memcpy(d + offs[k], u.data(), gap);
        }
        if (!touches) txt1_cache_write(c, key, d);
        return;
    }
    // every string has its slot reserved from startup, so they are read in parallel
    std::atomic<uint32_t> next_one(0);
    unsigned threads = std::thread::hardware_concurrency();
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    if (threads > n) threads = n ? n : 1;
    auto job = [&]() {
        std::vector<uint8_t> f, u;
        for (;;) {
            uint32_t k = next_one++;
            if (k >= n) return;
            if (!read_file(folder + "/" + c.parts[k].fpath, f)) { printf("disc: no puedo abrir %s\n", c.parts[k].fpath.c_str()); u.assign(2, 0); }
            else txt_decode(f, u);
            uint32_t gap = c.parts[k].sz;
            if (u.size() != gap) {
                if (u.size() > gap) printf("disc: %s crecio desde el arranque, se recorta (usa TTT_DISC_RESCAN=1)\n", c.parts[k].fpath.c_str());
                u.resize(gap, 0);
                if (gap >= 2) { u[gap - 2] = 0; u[gap - 1] = 0; }
            }
            if (offs[k] + gap <= c.sz) memcpy(d + offs[k], u.data(), gap);
        }
    };
    std::vector<std::thread> group;
    for (unsigned h = 1; h < threads; h++) group.emplace_back(job);
    job();
    for (auto& t : group) t.join();
    if (!touches) txt1_cache_write(c, key, d);
}

static void build_pak(const Container& c, const std::string& key, const std::string& folder, uint8_t* d, bool paralelo) {
    uint32_t n = (uint32_t)c.parts.size();
    uint32_t start_at = (4 + 4 * (n + 1) + ALIN_PAK - 1) / ALIN_PAK * ALIN_PAK;
    uint32_t be = (n >> 24) | ((n >> 8) & 0xFF00) | ((n << 8) & 0xFF0000) | (n << 24);
    memcpy(d, &be, 4);
    uint32_t pos = start_at;
    std::vector<uint32_t> offs(n + 1);
    for (uint32_t k = 0; k <= n; k++) {
        offs[k] = pos;
        uint32_t o = (pos >> 24) | ((pos >> 8) & 0xFF00) | ((pos << 8) & 0xFF0000) | (pos << 24);
        memcpy(d + 4 + 4 * k, &o, 4);
        if (k < n) pos = (pos + c.parts[k].sz + ALIGN_ENTRY - 1) / ALIGN_ENTRY * ALIGN_ENTRY;
    }
    unsigned threads = paralelo ? std::thread::hardware_concurrency() : 1;
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    if (threads > n) threads = n ? n : 1;
    auto job = [&](unsigned h) {
        for (uint32_t k = h; k < n; k += threads) {
            const Part& q = c.parts[k];
            if (!q.sz) continue;
            if (q.type == 6) {
                Bt3Src src;
                { Bt3Lock g; auto& m = q.idx == 1 ? bt3_dmg_of : bt3_of; auto it = m.find(key); if (it != m.end()) src = it->second; }
                std::vector<uint8_t>* o;
                if (!src.real.empty() && bt3_bytes(src.real, q.id, &o) && o->size() == q.sz) memcpy(d + offs[k], o->data(), q.sz);
                else printf("disc: %s: entrada BT3 %d no disponible\n", key.c_str(), q.id);
                continue;
            }
            if (q.type == 11) {
                auto st = bt3_stage_get(key);
                if (st && (size_t)q.id < st->size() && (*st)[q.id].size() == q.sz) memcpy(d + offs[k], (*st)[q.id].data(), q.sz);
                else printf("mapas: %s: entrada %d del escenario BT3 no disponible\n", key.c_str(), q.id);
                continue;
            }
            if (q.type == 10) {
                auto it = dmg_raw.find(key);
                FILE* f = it != dmg_raw.end() ? open_file(it->second, "rb") : nullptr;
                if (!f || fseek(f, (long)q.off, SEEK_SET) || fread(d + offs[k], 1, q.sz, f) != q.sz) printf("disc: %s: entrada %d del pak no disponible\n", key.c_str(), (int)k);
                if (f) fclose(f);
                continue;
            }
            if (q.type == 9) {
                std::string r = bt3_real(folder, q.fpath);
                Bt3Lock g;
                Bt3Conv* b = bt3_conv(r, q.id);
                if (!b) { bt3_conv_make(r, q.id, bt3_conv_cache[r]); b = bt3_conv(r, q.id); }
                if (b && b->out.size() == q.sz) memcpy(d + offs[k], b->out.data(), q.sz);
                else printf("disc: %s: archivo BT3 %s no disponible\n", key.c_str(), q.fpath.c_str());
                continue;
            }
            if (q.type == 8) {
                Bt3Lock g;
                auto it = bt3_eff_of.find(key);
                Bt3Eff* b = it != bt3_eff_of.end() ? bt3_eff_data(it->second.real) : nullptr;
                if (b && q.id >= 0 && q.id < 7 && b->out[q.id].size() == q.sz) memcpy(d + offs[k], b->out[q.id].data(), q.sz);
                else printf("disc: %s: efecto BT3 %d no disponible\n", key.c_str(), q.id);
                continue;
            }
            if (q.type == 7) {
                Bt3Lock g;
                auto it = bt3_anm_of.find(key);
                Bt3Anm* b = it != bt3_anm_of.end() ? bt3_anm_data(it->second.real) : nullptr;
                if (b && q.id < (int)b->out.size() && b->out[q.id].size() == q.sz) memcpy(d + offs[k], b->out[q.id].data(), q.sz);
                else printf("disc: %s: animacion BT3 %d no disponible\n", key.c_str(), q.id);
                continue;
            }
            if (q.type) {
                const Container* h = container_of(key + "/" + q.fpath);
                if (h) build_container(*h, key + "/" + q.fpath, folder + "/" + q.fpath, d + offs[k], false);
                continue;
            }
            FILE* f = open_file(folder + "/" + q.fpath, "rb");
            if (f) {
                size_t r = fread(d + offs[k], 1, q.sz, f);
                if (r != q.sz || fgetc(f) != EOF) printf("disc: %s cambio de tamano (usa TTT_DISC_RESCAN=1)\n", q.fpath.c_str());
                fclose(f);
                if (ends_with(q.fpath, ".pphd")) pphd_patch(c, key, d + offs[k], q.sz);
            } else printf("disc: no puedo abrir %s\n", q.fpath.c_str());
        }
    };
    std::vector<std::thread> group;
    for (unsigned h = 1; h < threads; h++) group.emplace_back(job, h);
    job(0);
    for (auto& t : group) t.join();
}

// the game decides whether a sound loops by looking for an ADPCM block with flag 3 (0x0881A964)
static bool sfx_loop_markers(int id) {
    if (id < 1 || id > (int)sfx_reg.size()) return false;
    SfxWav& w = sfx_reg[id - 1];
    if (w.markers >= 0) return w.markers > 0;
    if (w.state > 0) { w.markers = w.loop_end > w.loop_start ? 1 : 0; return w.markers > 0; }
    w.markers = 0;
    FILE* f = open_file(w.fpath, "rb");
    if (!f) return false;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
        uint8_t ch[8];
        while (fread(ch, 1, 8, f) == 8) {
            uint32_t n; memcpy(&n, ch + 4, 4);
            if (!memcmp(ch, "smpl", 4) && n >= 0x3C) {
                uint8_t sm[0x3C];
                if (fread(sm, 1, 0x3C, f) == 0x3C) {
                    uint32_t nb, a, b;
                    memcpy(&nb, sm + 0x1C, 4); memcpy(&a, sm + 0x2C, 4); memcpy(&b, sm + 0x30, 4);
                    w.markers = nb > 0 ? 1 : 0;
                }
                break;
            }
            if (fseek(f, (long)(n + (n & 1)), SEEK_CUR)) break;
        }
    }
    fclose(f);
    return w.markers > 0;
}

// rewrites offset, rate and length of every sound of the sibling .vagp in the .pphd PPVA table
static void pphd_patch(const Container& pak, const std::string& key, uint8_t* d, uint32_t sz) {
    if (sz < 0x20 || memcmp(d, "PPHD", 4)) return;
    const Container* v = nullptr;
    for (auto& q : pak.parts) if (q.type == 3 && (v = container_of(key + "/" + q.fpath))) break;
    if (!v) return;
    std::vector<uint32_t> bloques;
    for (uint32_t pos = 0x10; pos + 4 <= sz; pos += 4) {
        uint32_t b; memcpy(&b, d + pos, 4);
        if (b == 0xFFFFFFFFu || b >= sz) break;
        bloques.push_back(b);
    }
    bloques.push_back(sz);
    for (size_t k = 0; k + 1 < bloques.size(); k++) {
        uint32_t b = bloques[k], end_pos = bloques[k + 1];
        if (end_pos < b + 0x20 || memcmp(d + b, "PPVA", 4)) continue;
        uint32_t declared; memcpy(&declared, d + b + 4, 4);
        uint32_t n = (8 + declared - 0x20) / 16, fits = (end_pos - b - 0x20) / 16;
        if (declared < 0x18 || n > fits) n = fits;
        for (auto& q : v->parts) {
            if (q.idx < 1 || (uint32_t)q.idx > n) continue;
            uint8_t* e = d + b + 0x20 + (uint32_t)(q.idx - 1) * 16;
            memcpy(e, &q.off, 4); memcpy(e + 4, &q.freq, 4); memcpy(e + 8, &q.sz, 4);
        }
        return;
    }
}

static void build_vagp(const Container& c, uint8_t* d) {
    for (auto& q : c.parts) {
        if (q.off + 16 > c.sz) continue;
        memcpy(d + q.off, SFX_MARKER, 8);
        uint32_t id = (uint32_t)q.id, zero = 0;
        memcpy(d + q.off + 8, &id, 4);
        memcpy(d + q.off + 12, &zero, 4);
        if (q.sz >= 48 && q.off + q.sz <= c.sz && sfx_loop_markers(q.id)) {
            d[q.off + 16 + 1] = 6;
            d[q.off + q.sz - 16 + 1] = 3;
        }
    }
}

// only the requested chunk: a long track is not materialized whole on every read
static void at3_range(const Container& c, uint32_t from, uint8_t* d, uint32_t n) {
    if (c.parts.empty()) return;
    uint32_t id = (uint32_t)c.parts[0].id;
    for (uint32_t off = from / AT3_STEP * AT3_STEP; off < from + n && off + 16 <= c.sz; off += AT3_STEP) {
        uint8_t m[16];
        memcpy(m, AT3_MARKER, 8);
        memcpy(m + 8, &id, 4);
        memcpy(m + 12, &off, 4);
        for (uint32_t k = 0; k < 16; k++)
            if (off + k >= from && off + k < from + n) d[off + k - from] = m[k];
    }
}

static void build_at3(const Container& c, uint8_t* d) { at3_range(c, 0, d, c.sz); }

static void build_container(const Container& c, const std::string& key, const std::string& folder, uint8_t* d, bool paralelo) {
    if (c.type == 5) build_at3(c, d);
    else if (c.type == 4) build_tex(c, key, d);
    else if (c.type == 3) build_vagp(c, d);
    else if (c.type == 2) build_txt1(c, key, folder, d);
    else build_pak(c, key, folder, d, paralelo);
}

// assets/cache/armado: already built container (version in built_signature); the signature covers the structure (ids included) and each file's size and date
static int built_cache = -1;
static void signature_mix(uint64_t& h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t k = 0; k < n; k++) { h ^= b[k]; h *= 0x100000001B3ull; }
}
static thread_local std::string* sig_txt = nullptr;   // TTT_SIG_TRACE: readable copy of what the signature covers
static void container_signature(uint64_t& h, const Container& c, const std::string& key) {
    signature_mix(h, &c.type, 4); signature_mix(h, &c.sz, 4);
    if (sig_txt) *sig_txt += "C " + key + " " + std::to_string(c.type) + " " + std::to_string(c.sz) + "\n";
    for (auto& q : c.parts) {
        signature_mix(h, q.fpath.data(), q.fpath.size());
        uint32_t v[7] = {q.sz, q.off, q.freq, (uint32_t)q.type, c.type == 3 || c.type == 4 ? 0u : (uint32_t)q.id, (uint32_t)q.idx, q.type >= 6 ? 0xB7300000u + bt3::VERSION : 0xA5A5A5A5u};   // runtime ids (tex, sfx) are stamped at read time
        signature_mix(h, v, sizeof(v));
        if (sig_txt) { char b[160]; snprintf(b, sizeof(b), " sz %u off %u freq %u type %u id %u idx %u\n", v[0], v[1], v[2], v[3], v[4], v[5]); *sig_txt += "P " + q.fpath + b; }
        if (q.type >= 1 && q.type <= 4) if (const Container* hc = container_of(key + "/" + q.fpath)) container_signature(h, *hc, key + "/" + q.fpath);
    }
}
static void tree_signature(uint64_t& h, const std::string& folder, const std::string& rel) {
    std::vector<DirEntry> inp;
    list_dir_fs(folder, inp);
    std::sort(inp.begin(), inp.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    for (auto& e : inp) {
        std::string r = rel + "/" + e.name;
        signature_mix(h, r.data(), r.size());
        if (e.dir) { if (sig_txt) *sig_txt += "D " + r + "\n"; tree_signature(h, folder + "/" + e.name, r); continue; }
        signature_mix(h, &e.sz, 8); signature_mix(h, &e.date, 8);
        if (sig_txt) *sig_txt += "F " + r + " " + std::to_string((unsigned long long)e.sz) + " " + std::to_string((long long)e.date) + "\n";
    }
}
static uint64_t built_signature(const Container& c, const std::string& key, const std::string& folder) {
    uint64_t h = 0xCBF29CE484222325ull;
    const char* ver = icons_slot_page(0) >= 0 ? "armado 2 slots" : "armado 1";   // bump when the way any container is built changes
    signature_mix(h, ver, strlen(ver));
    int fl[2] = {tex_enabled, at3_enabled};
    signature_mix(h, fl, sizeof(fl));
    if (sig_txt) *sig_txt += std::string("V ") + ver + " " + std::to_string(fl[0]) + " " + std::to_string(fl[1]) + "\n";
    if (patch_mod_cache.val) {   // only the texture sizes of this container, not the whole file
        std::string k = lower(key);
        for (auto it = tex_orig.lower_bound(k); it != tex_orig.end() && it->first.compare(0, k.size(), k) == 0; ++it) {
            char c = it->first.size() > k.size() ? it->first[k.size()] : 0;
            if (c != '/' && c != '#') continue;
            signature_mix(h, it->first.data(), it->first.size());
            int v[2] = {it->second.first, it->second.second};
            signature_mix(h, v, sizeof(v));
            if (sig_txt) *sig_txt += "T " + it->first + " " + std::to_string(v[0]) + " " + std::to_string(v[1]) + "\n";
        }
    } else {
        std::vector<DirEntry> root_d;
        list_dir_fs(disc_dir, root_d);
        for (auto& e : root_d) if (e.name == "tamanos_texturas.txt") { signature_mix(h, &e.sz, 8); signature_mix(h, &e.date, 8); }
    }
    container_signature(h, c, key);
    tree_signature(h, folder, "");
    if (mod_touches(key)) {   // mod files over this container: names, sizes, dates and which mod serves each
        std::string k = lower(dmg_strip(key)), kp = k + "/";
        for (auto it = mod_dirs.lower_bound(k); it != mod_dirs.end() && (it->first == k || it->first.compare(0, kp.size(), kp) == 0); ++it) {
            signature_mix(h, it->first.data(), it->first.size());
            for (auto& e : it->second) {
                signature_mix(h, e.name.data(), e.name.size());
                signature_mix(h, &e.sz, 8); signature_mix(h, &e.date, 8);
                if (sig_txt) *sig_txt += "M " + it->first + "/" + e.name + " " + std::to_string((unsigned long long)e.sz) + " " + std::to_string((long long)e.date) + "\n";
                auto f = mod_files.find(it->first.empty() ? lower(e.name) : it->first + "/" + lower(e.name));
                if (f != mod_files.end()) signature_mix(h, f->second.data(), f->second.size());
            }
        }
    }
    return h;
}
// with the container from the cache, pngs decode in the background instead of on first draw
static void tex_ids(const Container& c, const std::string& key, std::vector<uint32_t>& ids) {
    {
        std::lock_guard<std::mutex> g(bt3_stage_mx);
        auto it = bt3_stage_of.find(key);
        if (it != bt3_stage_of.end()) { for (int k = 0; k < it->second.count(); k++) ids.push_back(it->second.id0 + (uint32_t)k); return; }
    }
    for (auto& q : c.parts) {
        if (c.type == 4) { if (q.id > 0) { ids.push_back((uint32_t)q.id); for (auto& m : tex_reg[q.id - 1].mats) ids.push_back(m.second); } continue; }
        if (q.type >= 1 && q.type <= 4) if (const Container* hc = container_of(key + "/" + q.fpath)) tex_ids(*hc, key + "/" + q.fpath, ids);
    }
}
// png decode ahead on a small pool: every container the game reads, newest first, so the first draw finds it decoded
static std::mutex& tp_mx = *(new std::mutex());
static std::condition_variable& tp_cv = *(new std::condition_variable());
static std::deque<std::vector<uint32_t>>& tp_q = *(new std::deque<std::vector<uint32_t>>());
static void tex_preload(std::vector<uint32_t> ids, bool urgent = true) {
    if (ids.empty()) return;
    if (!patch_tex_async.val) { std::thread([](std::vector<uint32_t> v) { for (uint32_t id : v) tex_load(id); }, std::move(ids)).detach(); return; }
    static bool started = false;
    {
        std::lock_guard<std::mutex> g(tp_mx);
        if (!started) {
            started = true;
            int n = (int)std::max(1u, std::thread::hardware_concurrency());
            n = std::min(4, std::max(1, n - 2));
            for (int i = 0; i < n; i++) std::thread([] {
                for (;;) {
                    uint32_t id = 0;
                    {
                        std::unique_lock<std::mutex> lk(tp_mx);
                        tp_cv.wait(lk, [] { return !tp_q.empty(); });
                        auto& v = tp_q.front();
                        id = v.back(); v.pop_back();
                        if (v.empty()) tp_q.pop_front();
                    }
                    if (!__atomic_load_n(&tex_reg[id - 1].state, __ATOMIC_ACQUIRE)) tex_load(id);
                }
            }).detach();
        }
        std::reverse(ids.begin(), ids.end());
        if (urgent) tp_q.push_front(std::move(ids)); else tp_q.push_back(std::move(ids));
    }
    tp_cv.notify_all();
}
static void at3_ids(const Container& c, const std::string& key, std::vector<int>& ids) {
    for (auto& q : c.parts) {
        if (c.type == 5) { if (q.id > 0) ids.push_back(q.id); continue; }
        if (q.type >= 1 && q.type <= 5) if (const Container* hc = container_of(key + "/" + q.fpath)) at3_ids(*hc, key + "/" + q.fpath, ids);
    }
}
// voices and music exposed as wav: read once in the background so the first decode finds them in the system cache
static void at3_warm(const Container& c, const std::string& key) {
    std::vector<int> ids;
    at3_ids(c, key, ids);
    if (ids.empty()) return;
    std::vector<std::string> files;
    std::vector<std::pair<uint32_t, At3Wav>> lips;   // voices: mouth levels computed here, not at their first play on the game thread
    for (int id : ids) if (id <= (int)at3_reg.size()) {
        const At3Wav& r = at3_reg[id - 1];
        files.push_back(r.fpath);
        if (lip_wants(r.key) == std::string::npos || r.frames == 0) continue;
        At3Wav w;
        w.fpath = r.fpath; w.key = r.key; w.rate = r.rate; w.channels = r.channels; w.bits = r.bits; w.is_float = r.is_float; w.data_ = r.data_; w.frames = r.frames;
        lips.push_back({(uint32_t)id, std::move(w)});
    }
    std::thread([](std::vector<std::string> v, std::vector<std::pair<uint32_t, At3Wav>> lv) {
        static std::vector<char> buf(1 << 20);
        static std::mutex m;
        std::lock_guard<std::mutex> g(m);
        uint64_t total = 0;
        for (auto& f : v) {
            FILE* h = open_file(f, "rb");
            if (!h) continue;
            size_t r;
            while ((r = fread(buf.data(), 1, buf.size(), h)) > 0 && total < (256ull << 20)) total += r;
            fclose(h);
        }
        uint64_t t0 = disc_us();
        int done = 0;
        for (auto& e : lv) {
            { std::lock_guard<std::mutex> l(lip_mx); if (lip_env.count(e.first)) continue; }
            if (!patch_lip_early.val) break;
            e.second.f = open_file(e.second.fpath, "rb");
            if (!e.second.f) continue;
            std::vector<uint8_t> env;
            lip_analyze(e.second, env);
            fclose(e.second.f); e.second.f = nullptr;
            std::lock_guard<std::mutex> l(lip_mx);
            if (!lip_env.count(e.first)) { lip_env[e.first] = std::move(env); done++; }
        }
        static std::atomic<int> logs{0};
        if (done && logs.fetch_add(1) < 20) printf("lip: %d voces analizadas en segundo plano en %llu ms\n", done, (unsigned long long)((disc_us() - t0) / 1000));
    }, std::move(files), std::move(lips)).detach();
}
static void sfx_ids(const Container& c, const std::string& key, std::vector<int>& ids) {
    for (auto& q : c.parts) {
        if (c.type == 3) { if (q.id > 0) ids.push_back(q.id); continue; }
        if (q.type >= 1 && q.type <= 3) if (const Container* hc = container_of(key + "/" + q.fpath)) sfx_ids(*hc, key + "/" + q.fpath, ids);
    }
}
// sounds of a container the game just read are decoded ahead, so their first play does not read the disk
static void sfx_prefetch(const Container& c, const std::string& key) {
    std::vector<int> ids;
    sfx_ids(c, key, ids);
    size_t n = 0;
    for (int id : ids) if (!__atomic_load_n(&sfx_reg[id - 1].state, __ATOMIC_ACQUIRE)) ids[n++] = id;
    ids.resize(n);
    if (ids.empty()) return;
    std::thread([](std::vector<int> v, std::string k) {
        auto t0 = std::chrono::steady_clock::now();
        for (int id : v) sfx_wav(id);
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 40) printf("sound: %s %d sonidos leidos por adelantado en %.0f ms\n", k.c_str(), (int)v.size(),
                                           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }, std::move(ids), key).detach();
}
static std::string built_path(const std::string& key) {
    std::string nm = key;
    for (auto& ch : nm) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
    return disc_dir + "/cache/armado/" + nm + ".bin";
}
static bool sig_trace_on() { const char* e = getenv("TTT_SIG_TRACE"); return e && *e == '1'; }
static uint64_t built_signature(const Container& c, const std::string& key, const std::string& folder);
static void sig_compare(const std::string& key, const std::string& folder) {   // first line that differs from the copy written with the cache
    std::vector<uint8_t> old;
    const Container* c = container_of(key);
    if (!c || !read_file(built_path(key) + ".sig.txt", old)) { printf("disc: firma de %s: sin copia previa\n", key.c_str()); return; }
    std::string now; sig_txt = &now;
    built_signature(*c, key, folder);
    sig_txt = nullptr;
    std::string o(old.begin(), old.end());
    size_t a = 0, b = 0;
    for (int n = 0; a < o.size() || b < now.size(); n++) {
        size_t ea = o.find('\n', a), eb = now.find('\n', b);
        std::string la = a < o.size() ? o.substr(a, ea - a) : "(fin)", lb = b < now.size() ? now.substr(b, eb - b) : "(fin)";
        if (la != lb) { printf("disc: firma de %s, linea %d: antes [%s] ahora [%s]\n", key.c_str(), n + 1, la.c_str(), lb.c_str()); return; }
        a = ea == std::string::npos ? o.size() : ea + 1; b = eb == std::string::npos ? now.size() : eb + 1;
    }
    printf("disc: firma de %s: mismas lineas\n", key.c_str());
}
static void built_miss(const std::string& key, const char* why, const std::string& folder) {   // why the built cache did not serve it, with the files newer than the cache
    static int logs = 0;
    if (logs >= 30) return;
    logs++;
    std::error_code ec;
    auto cdate = std::filesystem::last_write_time(std::filesystem::u8path(built_path(key)), ec);
    std::string newer;
    int n = 0;
    if (!ec && !folder.empty())
        for (auto it = std::filesystem::recursive_directory_iterator(std::filesystem::u8path(folder), ec); !ec && it != std::filesystem::recursive_directory_iterator() && n < 4; it.increment(ec)) {
            std::error_code e2;
            if (it->last_write_time(e2) > cdate && !e2) { newer += " " + std::filesystem::relative(it->path(), std::filesystem::u8path(folder), e2).generic_u8string() + (it->is_directory(e2) ? "/" : ""); n++; }
        }
    printf("disc: cache de %s no sirve: %s%s%s\n", key.c_str(), why, n ? ", mas nuevos que la cache:" : "", newer.c_str());
    if (sig_trace_on() && !folder.empty()) sig_compare(key, folder);
}
// texture and sfx ids are given in listing order, which changes between sessions: a cached build gets the current ids stamped in
static void built_restamp(const Container& c, const std::string& key, uint8_t* d, uint32_t sz) {
    if (c.type == 4) {
        for (auto& q : c.parts) if (q.off + 16 <= sz && !memcmp(d + q.off, TEX_MARKER, 8)) { uint32_t id = (uint32_t)q.id, chk = ~id; memcpy(d + q.off + 8, &id, 4); memcpy(d + q.off + 12, &chk, 4); }
        return;
    }
    if (c.type == 3) {
        for (auto& q : c.parts) if (q.off + 16 <= sz && !memcmp(d + q.off, SFX_MARKER, 8)) { uint32_t id = (uint32_t)q.id; memcpy(d + q.off + 8, &id, 4); }
        return;
    }
    if (c.type != 1 || sz < 4) return;
    auto be = [&](uint32_t o) { return o + 4 <= sz ? __builtin_bswap32(*(const uint32_t*)(d + o)) : 0u; };
    uint32_t n = be(0);
    if (n != c.parts.size()) return;
    for (uint32_t k = 0; k < n; k++) {
        const Part& q = c.parts[k];
        if (q.type < 1 || q.type > 4 || q.type == 2) continue;
        uint32_t a = be(4 + 4 * k), b = be(8 + 4 * k);
        if (a >= b || b > sz) continue;
        const Container* hc = container_of(key + "/" + q.fpath);
        if (hc) built_restamp(*hc, key + "/" + q.fpath, d + a, b - a);
    }
}
static bool built_read(const std::string& key, uint64_t signature, uint8_t* d, uint32_t sz, const std::string& folder = std::string(), bool game = false) {
    FILE* f = fopen(built_path(key).c_str(), "rb");
    if (!f) { if (game) built_miss(key, "no existe", ""); return false; }
    char m[8]; uint64_t fi = 0; uint32_t t = 0;
    bool head = fread(m, 8, 1, f) == 1 && !memcmp(m, "TTTARM1", 8) && fread(&fi, 8, 1, f) == 1 && fread(&t, 4, 1, f) == 1;
    bool ok = head && fi == signature && t == sz && fread(d, 1, sz, f) == sz;
    fclose(f);
    if (ok) { const Container* c = container_of(key); if (c) built_restamp(*c, key, d, sz); }
    if (!ok) built_miss(key, !head ? "cabecera rota" : fi != signature ? "firma distinta" : t != sz ? "tamano distinto" : "lectura corta", fi != signature ? folder : "");
    return ok;
}
static void built_write(std::string key, uint64_t signature, std::vector<uint8_t> v) {
    static std::atomic<unsigned> serial(0);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache/armado"), ec);
    std::string fpath = built_path(key), tmp = fpath + "." + std::to_string(serial++) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    uint32_t t = (uint32_t)v.size();
    bool ok = fwrite("TTTARM1", 8, 1, f) == 1 && fwrite(&signature, 8, 1, f) == 1 && fwrite(&t, 4, 1, f) == 1 && fwrite(v.data(), 1, t, f) == t;
    fclose(f);
    if (ok) ok = cache_replace(tmp, fpath);
    else remove(tmp.c_str());
    if (ok && sig_trace_on()) {
        const Container* c = container_of(key);
        if (c) {
            std::string t; sig_txt = &t;
            built_signature(*c, key, disc_dir + "/Packfile/" + dmg_strip(key));
            sig_txt = nullptr;
            if (FILE* g = fopen((fpath + ".sig.txt").c_str(), "wb")) { fwrite(t.data(), 1, t.size(), g); fclose(g); }
        }
    }
}

static bool built_valid(const std::string& key, uint64_t signature, uint32_t sz) {
    FILE* f = fopen(built_path(key).c_str(), "rb");
    if (!f) return false;
    char m[8]; uint64_t fi = 0; uint32_t t = 0;
    bool ok = fread(m, 8, 1, f) == 1 && !memcmp(m, "TTTARM1", 8) && fread(&fi, 8, 1, f) == 1 && fread(&t, 4, 1, f) == 1 && fi == signature && t == sz;
    fclose(f);
    return ok;
}

static bool built_candidate(const PkEntry& x) {
    if (!x.folder) return false;
    { std::lock_guard<std::mutex> g(bt3_stage_mx); if (bt3_stage_of.count(x.key)) return false; }
    const Container* c = container_of(x.key);
    return c && c->type != 2 && c->type != 5 && (patch_mod_cache.val || !mod_touches(x.key));
}

// Characters/<character>/<form>/color_N.pak: reading one form decodes the other forms of the same colour
static void sibling_forms(const PkEntry& x) {
    static std::set<std::string> done_n;
    std::vector<std::string> p;
    for (size_t a = 0, b; ; a = b + 1) { b = x.fpath.find('/', a); p.push_back(x.fpath.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; }
    if (p.size() != 4 || p[0] != "Characters") return;
    std::string group = p[1] + "/" + p[3];
    if (!done_n.insert(group).second) return;
    std::string pre = "Characters/" + p[1] + "/", suffix = "/" + p[3];
    std::vector<uint32_t> ids;
    for (auto& y : disc_pk) {
        if (!y.folder || y.fpath.size() <= pre.size() + suffix.size() || y.fpath.compare(0, pre.size(), pre) || !ends_with(y.fpath, suffix.c_str())) continue;
        if (std::count(y.fpath.begin() + pre.size(), y.fpath.end(), '/') != 1) continue;
        if (const Container* c = container_of(y.key)) tex_ids(*c, y.key, ids);
    }
    tex_preload(std::move(ids), false);
}

static uint64_t built_signature(const Container& c, const std::string& key, const std::string& folder);
static void built_write(std::string key, uint64_t signature, std::vector<uint8_t> v);

// once at startup: builds in the background whatever is not cached, yielding the disc while the game reads
static void preload() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    tex_no_retain = true;
    auto prio = [](const std::string& r) { return r.compare(0, 11, "Characters/") == 0 ? 0 : r.compare(0, 5, "Maps/") == 0 ? 1 : r.compare(0, 6, "Menus/") == 0 ? 2 : 3; };
    std::vector<int> order;
    for (size_t k = 0; k < disc_pk.size(); k++) if (built_candidate(disc_pk[k])) order.push_back((int)k);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return prio(disc_pk[a].fpath) < prio(disc_pk[b].fpath); });
    auto t0 = std::chrono::steady_clock::now();
    unsigned done_n = 0;
    std::vector<uint8_t> d;
    for (int k : order) {
        for (;;) {
            int64_t last = disc_last_read, ahora = std::chrono::steady_clock::now().time_since_epoch().count();
            if (disc_reading == 0 && std::chrono::steady_clock::duration(ahora - last) > std::chrono::milliseconds(1500)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        const PkEntry& x = disc_pk[k];
        const Container* c = container_of(x.key);
        std::string folder = disc_dir + "/Packfile/" + dmg_strip(x.key);
        uint64_t signature = built_signature(*c, x.key, folder);
        if (built_valid(x.key, signature, x.sz)) continue;
        d.assign(x.sz, 0);
        build_container(*c, x.key, folder, d.data(), false);
        built_write(x.key, signature, d);
        done_n++;
    }
    if (done_n) printf("disc: precarga terminada, %u contenedores nuevos en la cache en %.0f s\n", done_n,
                       std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

// ---------------- technique names per character (item G 61) ----------------
// a colour's 1_p/043_move_list_<en|es|fr>.txt (tools/move_lists.py) gives the name and command of each record of its 043; when the pak
// is served, the record ids become port ids (7000..9999 and 30000..32767, no txt1 uses them) and the text hook 0x0883EE74 answers them
struct MvText { uint32_t orig; std::string s[3]; bool has[3]; };
static std::mutex mv_mx;
static std::vector<MvText> mv_tab;
static std::map<std::string, uint32_t> mv_dedupe;
static uint32_t mv_gen_n = 1;
static const uint32_t MV_A = 30000, MV_AN = 2768, MV_B = 7000, MV_BN = 2900;   // 9900.. names of new characters
static uint32_t mv_id(size_t i) { return i < MV_AN ? MV_A + (uint32_t)i : MV_B + (uint32_t)(i - MV_AN); }
static int mv_index(uint32_t id) { return id >= MV_A && id < MV_A + MV_AN ? (int)(id - MV_A) : id >= MV_B && id < MV_B + MV_BN ? (int)(MV_AN + id - MV_B) : -1; }

static void mv_parse(const std::vector<uint8_t>& f, std::map<std::pair<int, int>, std::string>& out) {
    std::string s(f.begin(), f.end());
    if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    int rec = -1, kind = 0; std::string acc;
    auto flush = [&]() { if (rec > 0) { if (!acc.empty() && acc.back() == '\n') acc.pop_back(); out[{rec, kind}] = acc; } acc.clear(); };
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        int n = 0; char w[16] = {0};
        if (line.size() > 4 && line[0] == '[' && line.back() == ']' && sscanf(line.c_str(), "[%d %15[a-z]]", &n, w) == 2 && n > 0 && (!strcmp(w, "name") || !strcmp(w, "command"))) {
            flush(); rec = n; kind = strcmp(w, "name") ? 1 : 0;
        } else if (rec > 0 && !(nl == std::string::npos && line.empty())) { acc += line; acc += '\n'; }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    flush();
}

static uint32_t mv_alloc(uint32_t orig, const std::string* s, const bool* has) {   // mv_mx held; same texts share one id
    std::string k = std::to_string(orig);
    for (int l = 0; l < 3; l++) { k += has[l] ? '\x01' : '\x02'; k += s[l]; }
    auto it = mv_dedupe.find(k);
    if (it != mv_dedupe.end()) return it->second;
    if (mv_tab.size() >= MV_AN + MV_BN) { mv_tab.clear(); mv_dedupe.clear(); mv_gen_n++; printf("mv: tabla de textos llena, se reinicia\n"); }
    MvText t; t.orig = orig;
    for (int l = 0; l < 3; l++) { t.s[l] = s[l]; t.has[l] = has[l]; }
    mv_tab.push_back(t);
    uint32_t id = mv_id(mv_tab.size() - 1);
    mv_dedupe[k] = id;
    return id;
}

// BT3 move lists per P file, kept in RAM and on disk: serving a pak never reads a P file nor waits for BT3 work
struct MvB3 { bool ok = false; std::vector<bt3::MoveRec> m[4]; };
static std::mutex mvb_mx;
static std::map<std::string, MvB3> mvb_tab;
static std::string mvb_path(const std::string& real) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : real) { h ^= ch; h *= 1099511628211ull; }
    char b[40]; snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return disc_dir + "/cache/armado/mv_" + b + ".bin";
}
static void mvb_put_s(std::vector<uint8_t>& o, const std::string& t) { uint32_t n = (uint32_t)t.size(); o.insert(o.end(), (uint8_t*)&n, (uint8_t*)&n + 4); o.insert(o.end(), t.begin(), t.end()); }
static bool mvb_load_disk(const std::string& real, uint64_t sz, int64_t date, MvB3& r, const std::string& newest) {
    std::vector<uint8_t> f;
    if (!read_file(mvb_path(real), f, true) || f.size() < 24) return false;
    size_t q = 0;
    auto u32 = [&](uint32_t& v) { if (q + 4 > f.size()) return false; memcpy(&v, &f[q], 4); q += 4; return true; };
    auto str = [&](std::string& t) { uint32_t n; if (!u32(n) || q + n > f.size()) return false; t.assign((const char*)&f[q], n); q += n; return true; };
    uint64_t s0; int64_t d0; memcpy(&s0, &f[0], 8); memcpy(&d0, &f[8], 8); q = 16;
    if (s0 != sz || d0 != date) {
        static int logs = 0;
        if (logs++ < 20) printf("mv: cache de %s no sirve: tamano %llu -> %llu, fecha %s, mas nuevo: %s\n", real.c_str(), (unsigned long long)s0, (unsigned long long)sz, d0 == date ? "igual" : d0 < date ? "mas nueva" : "mas vieja", newest.empty() ? "-" : newest.c_str());
        return false;
    }
    uint32_t ok; if (!u32(ok)) return false;
    r = MvB3(); r.ok = ok != 0;
    for (int l = 0; l < 4; l++) {
        uint32_t n; if (!u32(n) || n > 4096) return false;
        r.m[l].resize(n);
        for (auto& x : r.m[l]) {
            uint32_t v[4]; for (auto& w : v) if (!u32(w)) return false;
            x.cat = (int)v[0]; x.a = (int)v[1]; x.b = (int)v[2]; x.c = (int)v[3];
            if (!str(x.name) || !str(x.cmd)) return false;
        }
    }
    return true;
}
static void mvb_save_disk(const std::string& real, uint64_t sz, int64_t date, const MvB3& r) {
    std::vector<uint8_t> o(16);
    memcpy(&o[0], &sz, 8); memcpy(&o[8], &date, 8);
    auto u32 = [&](uint32_t v) { o.insert(o.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
    u32(r.ok ? 1 : 0);
    for (int l = 0; l < 4; l++) {
        u32((uint32_t)r.m[l].size());
        for (auto& x : r.m[l]) { u32((uint32_t)x.cat); u32((uint32_t)x.a); u32((uint32_t)x.b); u32((uint32_t)x.c); mvb_put_s(o, x.name); mvb_put_s(o, x.cmd); }
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(disc_dir + "/cache/armado"), ec);
    std::string pth = mvb_path(real), tmp = pth + ".tmp";
    FILE* fp = open_file(tmp, "wb", true);
    if (!fp) return;
    bool good = fwrite(o.data(), 1, o.size(), fp) == o.size();
    fclose(fp);
    if (good) cache_replace(tmp, pth);
    else remove(tmp.c_str());
}
// load = may read the P file (or its disk entry); false = only what is already in RAM
static bool mvb_get(const std::string& real, std::vector<bt3::MoveRec> (&m)[4], bool load, bool* found) {
    *found = false;
    {
        std::lock_guard<std::mutex> g(mvb_mx);
        auto it = mvb_tab.find(real);
        if (it != mvb_tab.end()) { *found = true; for (int l = 0; l < 4; l++) m[l] = it->second.m[l]; return it->second.ok; }
    }
    if (!load) return false;
    auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    auto pp = std::filesystem::u8path(real);
    bool dir = std::filesystem::is_directory(pp, ec);
    uint64_t sz = 0; int64_t date = 0;
    std::string newest;
    if (dir) bt3_folder(real, &sz, &date, nullptr, &newest);
    else { sz = (uint64_t)std::filesystem::file_size(pp, ec); date = ec ? 0 : (int64_t)std::filesystem::last_write_time(pp, ec).time_since_epoch().count(); }
    MvB3 r;
    bool disk = mvb_load_disk(real, sz, date, r, newest);
    if (!disk) {
        bt3::File f;
        if ((dir ? bt3_folder(real, &sz, &date, &f.d) : read_file(real, f.d, true)) && bt3::is_p(f.d.data(), f.d.size())) r.ok = bt3::move_lists(f, r.m);
        mvb_save_disk(real, sz, date, r);
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    { static int logs = 0; if (logs++ < 40) printf("mv: listas BT3 de %s %s en %.0f ms\n", real.c_str(), disk ? "desde la cache" : "leidas del archivo", ms); }
    std::lock_guard<std::mutex> g(mvb_mx);
    MvB3& e = mvb_tab[real];
    e = r;
    *found = true;
    for (int l = 0; l < 4; l++) m[l] = e.m[l];
    return e.ok;
}
static int mvb_on() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_MV_BT3_CACHE"); v = e && *e ? atoi(e) : 1; } return v; }

static int psp_mv_lang = -1;   // game language as the port's menus see it (0 en, 1 es, 2 fr), set by the hooks
void psp_mv_set_lang(int li) { psp_mv_lang = li; }
// bg: from the hold worker; never waits for BT3 work nor reads a P file (false = not done, the game thread patches it)
static bool mv_patch(const std::string& key, const std::string& folder, uint8_t* d, uint32_t sz, bool bg = false) {
    if (lower(key).compare(0, 11, "characters/") || sz < 0x100) return true;
    auto be = [&](uint32_t o) { return o + 4 <= sz ? __builtin_bswap32(*(const uint32_t*)(d + o)) : 0u; };
    uint32_t n = be(0);
    if (n < 45 || n > 4096) return true;
    uint32_t a = be(4 + 4 * 43), b = be(4 + 4 * 44);
    if (a >= b || b > sz) return true;
    static const char* L[2] = {"en", "es"};
    std::map<std::pair<int, int>, std::string> tx[3];
    bool any = false, lang[3] = {false, false, false}, bt3s = false;
    std::shared_ptr<bt3::File> bf;
    std::string breal;
    std::vector<bt3::MoveRec> bm[4];
    bool bm_ok = false, bm_done = false;
    { std::lock_guard<std::mutex> g(bt3_of_mx); auto it = bt3_of.find(key); if (it != bt3_of.end()) breal = it->second.real; }
    if (!breal.empty() && mvb_on()) {
        bool found = false;
        bm_ok = mvb_get(breal, bm, false, &found);
        if (!found) {
            { static int logs = 0; if (!bg && logs++ < 20) printf("mv: %s servido sin listas preparadas (hilo del juego)\n", key.c_str()); }
            bm_ok = mvb_get(breal, bm, true, &found);
        }
        bm_done = true;
    } else if (!breal.empty() && bg) {
        std::unique_lock<std::mutex> g(bt3_mx, std::try_to_lock);
        if (!g.owns_lock()) return false;
        auto it = bt3_cache.find(breal);
        if (it == bt3_cache.end() || !it->second.f) return false;
        bf = it->second.f;
    } else if (!breal.empty()) { Bt3Lock g; Bt3Data* bd = bt3_data(breal); if (bd) bf = bd->f; }
    if (!bm_done && bf) bm_ok = bt3::move_lists(*bf, bm);
    if (bm_ok) {   // BT3 character: the records and texts of the game's language (its files can differ per language)
        int gl = psp_mv_lang >= 0 && psp_mv_lang < 3 && !bm[psp_mv_lang].empty() ? psp_mv_lang : 0;
        std::vector<uint8_t> dat;
        bt3::move_dat(bm[gl], dat);
        if (dat.size() <= b - a) {
            memset(d + a, 0, b - a);
            memcpy(d + a, dat.data(), dat.size());
            for (size_t k = 0; k < bm[gl].size(); k++)
                for (int l = 0; l < 3; l++) { tx[l][{(int)k + 1, 0}] = bm[gl][k].name; tx[l][{(int)k + 1, 1}] = bm[gl][k].cmd; }
            lang[gl] = any = bt3s = true;
        }
    }
    for (int l = 0; l < 2 && !bt3s; l++) {
        std::vector<uint8_t> f;
        if (read_file(folder + "/1_p/043_move_list_" + L[l] + ".txt", f) && !f.empty()) { mv_parse(f, tx[l]); lang[l] = any = true; }
    }
    if (!any) return true;
    std::lock_guard<std::mutex> g(mv_mx);
    int recs = 0, own = 0;
    for (uint32_t o = a; o + 32 <= b; o += 32, recs++) {
        int32_t cat; memcpy(&cat, d + o, 4);
        if (cat == -1) break;
        for (int kind = 0; kind < 2; kind++) {
            uint32_t orig; memcpy(&orig, d + o + 4 + 4 * kind, 4);
            std::string s[3]; bool has[3] = {false, false, false};
            for (int l = 0; l < 3; l++) { auto it = tx[l].find({recs + 1, kind}); if (it != tx[l].end()) { s[l] = it->second; has[l] = true; } }
            if (!has[0] && !has[1] && !has[2]) continue;
            uint32_t id = mv_alloc(orig, s, has);
            memcpy(d + o + 4 + 4 * kind, &id, 4);
            own++;
        }
    }
    static int logs = 0;
    if (logs++ < 40) printf("mv: %s: %d tecnicas, %d textos propios%s (%s%s)\n", key.c_str(), recs, own, bt3s ? " de BT3" : "", lang[0] ? "en " : "", lang[1] ? "es" : "");
    return true;
}

// text hook: 0 not a port id, 1 text in out (UTF-8), 2 no text in that language: use the game's id in orig
int psp_mv_text(uint32_t id, int lang, std::string& out, uint32_t* orig) {
    if (id >= roster::NAME_OLD && id < roster::NAME_OLD + (uint32_t)roster::K_OLD) {   // an original character renamed by a mod
        int k = (int)(id - roster::NAME_OLD);
        *orig = roster::name_orig()[k];
        const roster::Rename* r = roster::renamed(roster::sys_of_group(k));
        int l = lang >= 0 && lang < 3 ? lang : 0;
        if (!r || r->name[l].empty()) return 2;
        out = r->name[l];
        return 1;
    }
    if (roster::rank_of_name(id) >= 0) {   // name of a new character (name tables of group k)
        for (auto& c : roster::defs().chars)
            if (roster::rank(c.sysid) == roster::rank_of_name(id)) {
                int l = lang >= 0 && lang < 3 ? lang : 0;
                if (c.name[l].empty() && c.parent >= 0) {   // transformation of an original: its base's name
                    const roster::Rename* r = roster::renamed(c.parent);
                    if (r && !r->name[l].empty()) { out = r->name[l]; return 1; }
                    int k = roster::group_vanilla(c.parent);
                    if (k >= 0 && roster::name_orig()[k]) { *orig = roster::name_orig()[k]; return 2; }
                    out = c.id.empty() ? c.folder.substr(c.folder.find_last_of('/') + 1) : c.id;
                    return 1;
                }
                out = c.name[l];
                return 1;
            }
        return 0;
    }
    int i = mv_index(id);
    if (i < 0 || lang < 0 || lang > 2) return 0;
    std::lock_guard<std::mutex> g(mv_mx);
    if ((size_t)i >= mv_tab.size()) return 0;
    const MvText& t = mv_tab[(size_t)i];
    *orig = t.orig;
    if (t.has[lang]) { out = t.s[lang]; return 1; }
    if (t.has[0]) { out = t.s[0]; return 1; }
    return 2;
}
uint32_t psp_mv_gen() { std::lock_guard<std::mutex> g(mv_mx); return mv_gen_n; }

// dynamic colours: each character gets as many colours as its folders really have (assets + mods, colour 1 up to the first missing)
// served in system_<lang> 2_general/037_color_count.dat (BT3 id records of 60 bytes after 4, count at +6)
static const size_t COL_MODEL = 1428;
static size_t ro_first = (size_t)-1;
static std::vector<uint8_t> col_found;
static uint8_t col_n[roster::ALL];
static bool col_ready = false;
// colours 5..10 (Extra 8/10 colours): folders color_5.. next to color_1, appended as entries after the others
static const int COL_MAX = 10;
static bool ro_appended = false;
static std::map<uint32_t, uint32_t> col_x;   // colour 1 entry * 16 + colour (4..9) -> entry
static std::map<uint32_t, uint32_t> col_x_base;   // entry -> colour 1 entry
static std::map<uint32_t, std::vector<uint32_t>> col_list;   // colour 1 entry -> entries of the colours shown in the menu
static void col_extra(uint8_t* header, uint32_t* distinct) {
    std::vector<size_t> firsts;
    for (int k = 0; k < roster::K_OLD; k++) if (COL_MODEL - 1 + 5 * (size_t)k < disc_pk.size()) firsts.push_back(COL_MODEL - 1 + 5 * (size_t)k);
    if (ro_first != (size_t)-1)
        for (auto& c : roster::defs().chars) { int r = roster::rank(c.sysid); size_t f = ro_first + (roster::model_entry(r) - roster::MODEL_BASE); if (r >= 0 && f < disc_pk.size()) firsts.push_back(f); }
    int chars = 0, n0 = 0;
    for (size_t f : firsts) {
        const PkEntry x = disc_pk[f];
        std::string base = x.folder ? x.key : x.fpath;
        if (ends_with(lower(base), ".pak")) base.resize(base.size() - 4);
        if (lower(base).size() < 7 || lower(base).compare(base.size() - 7, 7, "color_1")) continue;
        std::string parent = base.substr(0, base.size() - 1);
        std::error_code ec;
        std::string c1 = std::filesystem::is_directory(std::filesystem::u8path(disc_dir + "/Packfile/" + base), ec) ? disc_dir + "/Packfile/" + base : "";
        for (size_t m = 0; c1.empty() && m < mod_active.size(); m++) {
            std::string d = mods_root_global + "/" + mod_active[m].first + "/Packfile/" + base;
            if (std::filesystem::is_directory(std::filesystem::u8path(d), ec)) c1 = d;
        }
        bool any = false;
        for (int j = 5; j <= COL_MAX; j++) {
            std::string rel = parent + std::to_string(j), real = disc_dir + "/Packfile/" + rel;
            if (!c1.empty() && !std::filesystem::exists(std::filesystem::u8path(real), ec) && is_dir(real)) link_to[link_key(real)] = c1;   // only 1_p in the mod: the rest from colour 1
            PkEntry y = x;
            uint64_t t = 0;
            if (pak_folder(y, real, rel, 1)) y.fpath = rel + ".pak";
            else if (is_file(real + ".pak", &t)) { y.fpath = rel + ".pak"; y.key.clear(); y.folder = false; y.sz = (uint32_t)t; }
            else break;
            if (disc_pk.size() >= 32767) break;
            col_x[(uint32_t)f * 16 + (uint32_t)(j - 1)] = (uint32_t)disc_pk.size();
            col_x_base[(uint32_t)disc_pk.size()] = (uint32_t)f;
            disc_pk.push_back(y);
            (*distinct)++; n0++; any = true;
        }
        chars += any;
    }
    if (!n0) return;
    uint32_t n = (uint32_t)disc_pk.size();
    memcpy(header + 4, &n, 4);
    disc_pk_data = std::max(disc_pk_data, sectors_of((n + 1) * 16));
    ro_appended = true;
    printf("disc: colores extra: %d colores 5..%d en %d personajes, PACKFILE con %u entradas\n", n0, COL_MAX, chars, n);
}
// menu colour k of a character (its colour 1 entry) -> entry of its k-th present colour
bool psp_col_extra(uint32_t first, uint32_t colour, uint32_t* out) {
    auto it = col_list.find(first);
    if (it == col_list.end() || colour >= it->second.size()) return false;
    *out = it->second[colour];
    return true;
}
uint32_t psp_col_extra_base(uint32_t idx) { auto it = col_x_base.find(idx); return it == col_x_base.end() ? idx : it->second; }
static void col_counts() {
    const char* e = getenv("TTT_DYN_COLORS");
    if (e && *e && !atoi(e)) { printf("disc: colores dinamicos apagados\n"); return; }
    memset(col_n, 0, sizeof(col_n));
    auto run = [&](size_t first) {
        std::vector<uint32_t>& l = col_list[(uint32_t)first];
        l.clear();
        for (int j = 0; j < COL_MAX; j++) {   // colour 1 up to the first missing
            if (j < 4) { if (first + j < col_found.size() && col_found[first + j]) l.push_back((uint32_t)(first + j)); else break; }
            else { auto it = col_x.find((uint32_t)first * 16 + (uint32_t)j); if (it != col_x.end()) l.push_back(it->second); else break; }
        }
        if (l.empty()) l.push_back((uint32_t)first);
        return (int)l.size();
    };
    int less = 0, news = 0;
    for (int s = 0; s < roster::SYS; s++) {
        int k = roster::group_vanilla(s);
        if (k < 0 || COL_MODEL - 1 + 5 * k + 3 >= col_found.size()) continue;
        col_n[s] = (uint8_t)run(COL_MODEL - 1 + 5 * k);
        less += col_n[s] < 4;
    }
    if (ro_first != (size_t)-1)
        for (auto& c : roster::defs().chars) { int r = roster::rank(c.sysid); if (r >= 0) { col_n[c.sysid] = (uint8_t)run(ro_first + (roster::model_entry(r) - roster::MODEL_BASE)); news++; } }
    for (auto& r : roster::defs().renames) if (r.newid >= 0 && r.newid < roster::SYS && r.newid != r.sysid) col_n[r.newid] = col_n[r.sysid];
    col_ready = true;
    printf("disc: colores dinamicos: %d originales con menos de 4 colores, %d personajes nuevos\n", less, news);
}
static void col_patch(const PkEntry& x, const Container* c, uint8_t* d) {
    if (!col_ready || !c || c->type != 1 || lower(x.key).find("system_") == std::string::npos) return;
    auto be = [&](uint32_t o) { return o + 4 <= x.sz ? __builtin_bswap32(*(const uint32_t*)(d + o)) : 0u; };
    for (size_t i = 0; i < c->parts.size(); i++) {
        std::string f = lower(c->parts[i].fpath);
        size_t sl = f.find_last_of('/');
        if (f.compare(sl == std::string::npos ? 0 : sl + 1, 3, "037")) continue;
        uint32_t a = be(4 + 4 * (uint32_t)i), b = be(8 + 4 * (uint32_t)i);
        if (a >= b || b > x.sz || b - a < 4 + 60u * roster::SYS) return;
        int changed = 0;
        for (int s = 0; s < roster::SYS; s++) {
            uint8_t* p = d + a + 4 + 60 * s + 6;
            if (col_n[s] && *p != col_n[s]) { *p = col_n[s]; changed++; }
        }
        static int logs = 0;
        if (logs++ < 6) printf("disc: %s: cantidad de colores servida, %d personajes cambiados\n", x.key.c_str(), changed);
        return;
    }
}

// item 72: the two characters flying behind the main menu (menu_main 07_bg_chars_anim.rhg textures 01 and 02) drawn from a pool:
// the two originals plus every png in Packfile/Menus/bg_chars of the enabled mods; two different ones on every read of the menu
static std::vector<std::string> fly_pool;
static bool fly_scanned = false;
static std::map<std::pair<std::string, uint32_t>, uint32_t> fly_ids;

static uint32_t fly_reg(const std::string& f, uint32_t slot) {
    auto k = std::make_pair(f, slot);
    auto it = fly_ids.find(k);
    if (it != fly_ids.end()) return it->second;
    const TexPng& o = tex_reg[slot - 1];
    int w = 0, h = 0, cols = 0;
    uint32_t id = 0;
    if (png_cabecera(f, &w, &h, &cols) && w > 0 && h > 0) {
        TexPng t; t.fpath = f; t.name = f.substr(f.find_last_of("/\\") + 1); t.pak = o.pak; t.psp = o.psp; t.colors = cols;
        t.width0 = o.width0; t.height0 = o.height0; t.dw = o.dw; t.dh = o.dh; t.bpp = o.bpp; t.pal_size = o.pal_size; t.sz = o.sz;
        t.scale = std::max(1, w / std::max(1, o.width0));
        tex_reg.push_back(t);
        id = (uint32_t)tex_reg.size();
    } else printf("menu: %s no es un png valido\n", f.c_str());
    fly_ids[k] = id;
    return id;
}

static void fly_patch(const PkEntry& x, const Container* c, uint8_t* d) {
    if (!tex_enabled || !c || c->type != 1 || lower(x.key).find("menu_main") == std::string::npos) return;
    if (!fly_scanned) {
        fly_scanned = true;
        std::error_code ec;
        for (auto& m : mod_active) {
            std::vector<std::string> fs;
            for (auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(mods_root_global + "/" + m.first + "/Packfile/Menus/bg_chars"), ec))
                if (e.is_regular_file(ec) && ends_with(lower(e.path().filename().u8string()), ".png")) fs.push_back(e.path().generic_u8string());
            std::sort(fs.begin(), fs.end());
            fly_pool.insert(fly_pool.end(), fs.begin(), fs.end());
        }
        printf("menu: personajes del fondo: %zu extra desde mods\n", fly_pool.size());
    }
    if (fly_pool.empty()) return;
    auto be = [&](uint32_t o) { return o + 4 <= x.sz ? __builtin_bswap32(*(const uint32_t*)(d + o)) : 0u; };
    for (size_t i = 0; i < c->parts.size(); i++) {
        if (lower(c->parts[i].fpath).find("bg_chars_anim.rhg") == std::string::npos) continue;
        uint32_t a = be(4 + 4 * (uint32_t)i), b = be(8 + 4 * (uint32_t)i), off[2] = {0, 0}, id[2] = {0, 0};
        std::string seen;
        if (a >= b || b > x.sz) return;
        for (uint32_t o = a; o + 16 <= b; o += 4) {
            if (memcmp(d + o, TEX_MARKER, 8)) continue;
            uint32_t t, chk; memcpy(&t, d + o + 8, 4); memcpy(&chk, d + o + 12, 4);
            if (chk != ~t || !t || t > tex_reg.size()) continue;
            int n = atoi(tex_reg[t - 1].name.c_str());
            if (seen.size() < 200) seen += " " + tex_reg[t - 1].name;
            if ((n == 1 || n == 2) && !id[n - 1]) { off[n - 1] = o; id[n - 1] = t; }
        }
        if (!id[0] || !id[1]) { static int logs = 0; if (logs++ < 3) printf("menu: %s sin las texturas 01 y 02 (texturas:%s)\n", c->parts[i].fpath.c_str(), seen.c_str()); return; }
        static std::mt19937 rng((uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());
        uint32_t n = (uint32_t)fly_pool.size() + 2, p0 = rng() % n, p1 = rng() % (n - 1);
        if (p1 >= p0) p1++;
        uint32_t pick[2] = {p0, p1};
        std::string shown[2];
        for (int s = 0; s < 2; s++) {
            uint32_t k = pick[s], nid = k == (uint32_t)s ? id[s] : fly_reg(k < 2 ? tex_reg[id[k] - 1].fpath : fly_pool[k - 2], id[s]);
            if (!nid) nid = id[s];
            uint32_t chk = ~nid;
            memcpy(d + off[s] + 8, &nid, 4); memcpy(d + off[s] + 12, &chk, 4);
            shown[s] = tex_reg[nid - 1].name;
        }
        static int logs = 0;
        if (logs++ < 20) printf("menu: personajes del fondo: %s y %s (de %u)\n", shown[0].c_str(), shown[1].c_str(), n);
        return;
    }
}

// item 33: containers built (or read from the built cache) by one background worker and kept in RAM; the game's read is one memcpy
struct HoldEntry { std::vector<uint8_t> d; uint64_t used = 0; std::vector<uint8_t> mv; uint32_t mv_at = 0, mv_gen = 0; int mv_lang = -2; bool mv_done = false; };
static int hold_mv() { static int v = -1; if (v < 0) { const char* e = getenv("TTT_HOLD_MV"); v = e && *e ? atoi(e) : 1; } return v; }
static std::mutex& hold_mx = *(new std::mutex());
static std::condition_variable& hold_cv = *(new std::condition_variable());
static std::map<int, HoldEntry>& hold_map = *(new std::map<int, HoldEntry>());
static std::deque<int>& hold_q = *(new std::deque<int>());
static uint64_t hold_bytes = 0, hold_use = 0;
static int hold_busy = -1;
static bool hold_busy_fast = false;   // a slow background build is not waited for: the game builds it with all cores
static std::set<int>& hold_front = *(new std::set<int>());   // near the cursor: built with all cores
static int hold_late = -1;   // TTT_HOLD_LATE=0: the built cache is written before the entry is released (old)
static const uint64_t HOLD_CAP = 400ull << 20;

static bool hold_take(int i, uint8_t* d, uint32_t sz, bool* mv_ok = nullptr) {
    std::unique_lock<std::mutex> g(hold_mx);
    static int wait_slow = -1;
    if (wait_slow < 0) { const char* e = getenv("TTT_HOLD_WAIT_SLOW"); wait_slow = e && *e ? atoi(e) : 0; }
    if (hold_busy == i && !hold_busy_fast && !wait_slow) { static int logs = 0; if (logs++ < 20) printf("disc: %s se arma ahora, sin esperar al segundo plano\n", disc_pk[i].key.c_str()); return false; }
    if (hold_busy == i) hold_cv.wait_for(g, std::chrono::seconds(2), [i] { return hold_busy != i; });
    auto it = hold_map.find(i);
    if (it == hold_map.end() || it->second.d.size() != sz) return false;
    memcpy(d, it->second.d.data(), sz);
    it->second.used = ++hold_use;
    HoldEntry& e = it->second;
    if (mv_ok && e.mv_done && e.mv_lang == psp_mv_lang && e.mv_gen == psp_mv_gen() && (uint64_t)e.mv_at + e.mv.size() <= sz) {
        if (!e.mv.empty()) memcpy(d + e.mv_at, e.mv.data(), e.mv.size());
        *mv_ok = true;
    }
    return true;
}
static void hold_worker() {
    for (;;) {
        int i;
        bool fast = false, fresh = false;
        {
            std::unique_lock<std::mutex> g(hold_mx);
            hold_cv.wait(g, [] { return !hold_q.empty(); });
            i = hold_q.front(); hold_q.pop_front();
            if (hold_map.count(i)) continue;
            hold_busy = i;
            fresh = rl_fresh.count(i) > 0;
            fast = hold_front.erase(i) > 0;
            hold_busy_fast = fast;
        }
        auto t0 = std::chrono::steady_clock::now();
        PkEntry x = disc_pk[i];
        const Container* c = container_of(x.key);
        std::vector<uint8_t> d(x.sz, 0), w;
        bool ok = c != nullptr, built = false;
        uint64_t signature = 0;
        if (ok) {
            std::string folder = disc_dir + "/Packfile/" + dmg_strip(x.key);
            bool usa = built_cache && built_candidate(x) && !fresh;
            signature = usa ? built_signature(*c, x.key, folder) : 0;
            if (!(usa && built_read(x.key, signature, d.data(), x.sz, folder))) {
                build_container(*c, x.key, folder, d.data(), fast);
                built = true;
                if (usa) { if (hold_late) w = d; else built_write(x.key, signature, d); }
            }
        }
        uint64_t ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        { static int slow = 0; if (ms >= 100 && slow < 30) { slow++; printf("disc: %s preparado en memoria en %llu ms (%s%s)\n", x.key.c_str(), (unsigned long long)ms, built ? "armado" : "cache", fast ? ", rapido" : ""); } }
        if (!w.empty()) { std::thread(built_write, x.key, signature, std::move(w)).detach(); }
        std::vector<uint8_t> mvs; uint32_t mv_at = 0, mv_gen = 0; int mv_lang = -2; bool mv_done = false;
        if (ok && hold_mv() && !lower(x.key).compare(0, 11, "characters/")) {
            int l0 = psp_mv_lang; uint32_t g0 = psp_mv_gen();
            std::vector<uint8_t> p = d;
            bool done = mv_patch(x.key, disc_dir + "/Packfile/" + dmg_strip(x.key), p.data(), x.sz, true);
            if (done && g0 == psp_mv_gen() && l0 == psp_mv_lang) {
                size_t lo = 0, hi = p.size();
                while (lo < hi && p[lo] == d[lo]) lo++;
                while (hi > lo && p[hi - 1] == d[hi - 1]) hi--;
                mvs.assign(p.begin() + lo, p.begin() + hi);
                mv_at = (uint32_t)lo; mv_gen = g0; mv_lang = l0; mv_done = true;
            }
        }
        std::lock_guard<std::mutex> g(hold_mx);
        hold_busy = -1;
        if (!ok) { hold_cv.notify_all(); continue; }
        while (hold_bytes + x.sz > HOLD_CAP && !hold_map.empty()) {
            auto v = hold_map.begin();
            for (auto e = hold_map.begin(); e != hold_map.end(); ++e) if (e->second.used < v->second.used) v = e;
            hold_bytes -= v->second.d.size(); hold_map.erase(v);
        }
        hold_bytes += x.sz;
        HoldEntry& e = hold_map[i];
        e.d = std::move(d); e.used = ++hold_use;
        e.mv = std::move(mvs); e.mv_at = mv_at; e.mv_gen = mv_gen; e.mv_lang = mv_lang; e.mv_done = mv_done;
        static int logs = 0;
        if (logs++ < 8) { printf("disc: %s en memoria en %llu ms\n", x.key.c_str(), (unsigned long long)ms); fflush(stdout); }
        hold_cv.notify_all();
    }
}
// front = the cursor is near: first in the queue, textures decoded too
void psp_disc_hold(uint32_t idx, bool front, bool fast) {
    if (!disc_ready || idx >= disc_pk.size()) return;
    static bool started = false;
    if (built_cache < 0) { const char* e = getenv("TTT_BUILT_CACHE"); built_cache = e ? atoi(e) : 1; }
    if (hold_late < 0) { const char* e = getenv("TTT_HOLD_LATE"); hold_late = e && *e ? atoi(e) : 1; }
    {
        std::lock_guard<std::mutex> g(hold_mx);
        if (!started) { started = true; std::thread(hold_worker).detach(); }
        if (hold_map.count((int)idx) || hold_busy == (int)idx) { if (!front) return; }
        else {
            auto it = std::find(hold_q.begin(), hold_q.end(), (int)idx);
            if (it != hold_q.end()) { if (!front) return; hold_q.erase(it); }
            if (front) { hold_q.push_front((int)idx); if (fast) hold_front.insert((int)idx); } else hold_q.push_back((int)idx);
        }
    }
    hold_cv.notify_all();
    if (front && tex_enabled) {
        static std::set<uint32_t> tex_done;
        const Container* c = container_of(disc_pk[idx].key);
        if (c && tex_done.insert(idx).second) { std::vector<uint32_t> ids; tex_ids(*c, disc_pk[idx].key, ids); tex_preload(std::move(ids), false); }
    }
}
// the map's sound pak (Maps/SFX/NN_...) held as soon as the loading screen starts reading the map, TTT_MAP_SFX_AHEAD=0 off
static void map_sfx_ahead(int idx) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("TTT_MAP_SFX_AHEAD"); on = !(e && *e == '0'); }
    if (!on || !patch_hold_battle.val || idx < 0 || (size_t)idx >= disc_pk.size()) return;
    static bool bt3w = false;   // BT3 water sounds of the mod decoded once, off the game thread, before the first splash reads them
    if (!bt3w) {
        bt3w = true;
        std::vector<int> ids;
        for (auto& kv : mod_files)
            if (!kv.first.compare(0, 17, "sounds/bt3_water/") && ends_with(kv.first, ".wav") && isdigit((unsigned char)kv.first[17])) ids.push_back(atoi(kv.first.c_str() + 17));
        if (!ids.empty()) std::thread([ids]() {
            uint64_t t0 = disc_us(); const int16_t* p; int n, r;
            for (int id : ids) psp_bt3_snd_pcm(id, &p, &n, &r);
            printf("disc: %d sonidos de agua BT3 decodificados por adelantado en %.0f ms\n", (int)ids.size(), (disc_us() - t0) / 1000.0); fflush(stdout);
        }).detach();
    }
    static std::map<int, int> of;
    auto it = of.find(idx);
    if (it == of.end()) {
        std::string f = lower(disc_pk[idx].fpath), b = f.substr(f.find_last_of('/') + 1);
        size_t u = b.find('_');
        std::string num = u != std::string::npos && u > 0 && isdigit((unsigned char)b[0]) ? b.substr(0, u + 1) : "";
        std::string stem = b.substr(0, b.find('.'));
        int found = -1;
        for (size_t i = 0; i < disc_pk.size() && found < 0; i++) {
            std::string y = lower(disc_pk[i].fpath);
            if (y.compare(0, 9, "maps/sfx/")) continue;
            std::string yb = y.substr(9);
            if ((!num.empty() && !yb.compare(0, num.size(), num)) || yb.substr(0, yb.find('.')) == stem) found = (int)i;
        }
        it = of.emplace(idx, found).first;
        if (found >= 0) { printf("disc: sonidos del mapa %s a memoria al empezar la carga (%s)\n", disc_pk[idx].fpath.c_str(), disc_pk[found].fpath.c_str()); fflush(stdout); }
    }
    if (it->second >= 0) psp_disc_hold((uint32_t)it->second, true, false);
}
// the battle's fixed containers (HUD, battle sounds) built into RAM ahead: the loading screen does not sign and read their cache
void psp_disc_hold_battle() {
    if (!patch_hold_battle.val || !disc_ready) return;
    static std::vector<uint32_t> ids;
    static bool found = false;
    if (!found) {
        found = true;
        for (size_t i = 0; i < disc_pk.size(); i++) {
            std::string k = lower(disc_pk[i].key);
            if (!k.compare(0, 20, "battle_ui/battle_ui_") || k == "battle_ui/sfx_battle") ids.push_back((uint32_t)i);
        }
        printf("disc: %d contenedores fijos de batalla a memoria antes de la carga\n", (int)ids.size()); fflush(stdout);
    }
    for (uint32_t i : ids) psp_disc_hold(i, true, false);
}

// a transformation's target: its colour pak and the other entries of the same form folder (sfx...) held, and a BT3 P read ahead
static std::mutex& prep_mx = *(new std::mutex());
static std::map<uint32_t, std::vector<int>>& prep_group = *(new std::map<uint32_t, std::vector<int>>());
static std::set<uint32_t>& prep_bt3_done = *(new std::set<uint32_t>());
void psp_disc_prepare(uint32_t idx) {
    if (!disc_ready || idx >= disc_pk.size()) return;
    std::vector<int> g{(int)idx};
    std::string k = disc_pk[idx].key, dir = k.substr(0, k.find_last_of('/') + 1);
    if (!dir.empty() && lower(k).compare(0, 11, "characters/") == 0)
        for (size_t i = 0; i < disc_pk.size(); i++) {
            const std::string& y = disc_pk[i].key;
            if (i == idx || y.size() <= dir.size() || y.compare(0, dir.size(), dir) || y.find('/', dir.size()) != std::string::npos || y.find('#') != std::string::npos) continue;
            if (lower(y).compare(dir.size(), 6, "color_") == 0) continue;
            g.push_back((int)i);
        }
    for (int i : g) psp_disc_hold((uint32_t)i, true, true);
    std::string real;
    { std::lock_guard<std::mutex> l(bt3_of_mx); auto it = bt3_of.find(k); if (it != bt3_of.end()) real = it->second.real; }
    {
        std::lock_guard<std::mutex> l(prep_mx);
        prep_group[idx] = g;
        if (real.empty()) prep_bt3_done.insert(idx);
        else if (!prep_bt3_done.count(idx)) {
            std::thread([idx, real] {
                { Bt3Lock b; bt3_data(real); }
                std::lock_guard<std::mutex> l(prep_mx); prep_bt3_done.insert(idx);
            }).detach();
        }
    }
    static int logs = 0;
    if (logs++ < 20) printf("transform: preparando %s, %d entradas%s\n", k.c_str(), (int)g.size(), real.empty() ? "" : ", P de BT3");
}
bool psp_disc_held(uint32_t idx) {
    if (!disc_ready || idx >= disc_pk.size()) return true;
    std::vector<int> g{(int)idx};
    {
        std::lock_guard<std::mutex> l(prep_mx);
        auto it = prep_group.find(idx);
        if (it != prep_group.end()) g = it->second;
        if (it != prep_group.end() && !prep_bt3_done.count(idx)) return false;
    }
    std::lock_guard<std::mutex> l(hold_mx);
    for (int i : g) if (container_of(disc_pk[i].key) && !hold_map.count(i)) return false;
    return true;
}

static bool disc_build_into(int i, uint8_t* d) {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t n0 = open_n, us0 = open_us;
    auto tl = t0;
    double lp[6] = {0, 0, 0, 0, 0, 0};
    auto lap = [&](int k) { auto n = std::chrono::steady_clock::now(); lp[k] = std::chrono::duration<double, std::milli>(n - tl).count(); tl = n; };
    PkEntry& x = disc_pk[i];
    const Container* c = container_of(x.key);
    std::string folder = disc_dir + "/Packfile/" + dmg_strip(x.key);
    if (built_cache < 0) { const char* e = getenv("TTT_BUILT_CACHE"); built_cache = e ? atoi(e) : 1; }
    bool mv_held = false;
    bool held = hold_take(i, d, x.sz, &mv_held);
    lap(0);
    { static int logs = 0; if (held && logs++ < 30) printf("disc: %s servido desde memoria\n", x.key.c_str()); }
    bool usa = !held && built_cache && built_candidate(x) && !rl_fresh.count(i);
    uint64_t signature = usa ? built_signature(*c, x.key, folder) : 0;
    lap(1);
    bool from_cache = held || (usa && built_read(x.key, signature, d, x.sz, folder, true));
    lap(2);
    if (tex_enabled && c && (from_cache || patch_tex_async.val)) {
        std::vector<uint32_t> ids;
        tex_ids(*c, x.key, ids);
        tex_preload(std::move(ids));
    }
    if (!from_cache) {
        memset(d, 0, x.sz);
        if (c) build_container(*c, x.key, folder, d, true);
        if (usa) std::thread(built_write, x.key, signature, std::vector<uint8_t>(d, d + x.sz)).detach();
    }
    lap(3);
    bool rl_read = false;
    if (rl_gen) { std::lock_guard<std::mutex> g(rl_watch_mx); rl_read = rl_watch_idx.erase(i) > 0; }
    if (rl_read) {
        uint32_t n = 0, bad = 0, lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t o = 0; o + 16 <= x.sz; o += 4) {
            if (memcmp(d + o, TEX_MARKER, 8)) continue;
            uint32_t id, chk; memcpy(&id, d + o + 8, 4); memcpy(&chk, d + o + 12, 4);
            if (chk != ~id) continue;
            n++; lo = std::min(lo, id); hi = std::max(hi, id);
            if (!id || id > tex_reg.size()) bad++;
        }
        printf("recarga: el juego leyo %s, %u bytes%s, texturas en el pak %u (%u..%u), invalidas %u\n", x.key.c_str(), x.sz, held ? " (en memoria)" : from_cache ? " (desde cache)" : "", n, n ? lo : 0, hi, bad);
    }
    if (!mv_held) mv_patch(x.key, folder, d, x.sz);
    lap(4);
    col_patch(x, c, d);
    fly_patch(x, c, d);
    if (tex_enabled && c) sibling_forms(x);
    if (c && patch_sfx_prefetch.val) { sfx_prefetch(*c, x.key); at3_warm(*c, x.key); }
    if (psp_stall_on) psp_stall_file(x.key.c_str());
    uint64_t ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    lap(5);
    disc_built++; disc_ms += ms;
    { static int tl_logs = 0; if (ms >= 8 && tl_logs < 150) { tl_logs++; bool b3; { std::lock_guard<std::mutex> g(bt3_of_mx); b3 = bt3_of.count(x.key) > 0; }
        printf("disc: tiempos %s [%s]: %llu ms, espera %.1f, firma %.1f, cache %.1f, armado %.1f, mv %.1f%s, resto %.1f%s\n", x.key.c_str(), b3 ? "BT3" : "TTT", (unsigned long long)ms, lp[0], lp[1], lp[2], lp[3], lp[4], mv_held ? " (hecho antes)" : "", lp[5], held ? " (en memoria)" : from_cache ? " (desde cache)" : ""); } }
    { static int logs = 0; if (!from_cache && ms >= 50 && logs < 40) { logs++; printf("disc: %s armado sin cache en %llu ms%s\n", x.key.c_str(), (unsigned long long)ms, usa ? "" : " (no se guarda)"); } }
    if (psp_stall_on) psp_stall_note(5, (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(), x.sz);
    if (disc_trace) printf("disc: %s armado%s, %u partes, %u bytes, %llu ms, %llu archivos abiertos en %llu ms\n", x.fpath.c_str(), from_cache ? " desde cache" : "",
                            (unsigned)(c ? c->parts.size() : 0), x.sz, (unsigned long long)ms, (unsigned long long)(open_n - n0), (unsigned long long)((open_us - us0) / 1000));
    return from_cache;
}

// an entry the game will likely read soon: its textures decoded and its container in the cache, in the background
void psp_disc_prebuild(uint32_t idx) {
    static std::set<uint32_t> done;
    if (!disc_ready || idx >= disc_pk.size() || !done.insert(idx).second) return;
    PkEntry x = disc_pk[idx];
    const Container* c = container_of(x.key);
    if (!c) return;
    if (tex_enabled) { std::vector<uint32_t> ids; tex_ids(*c, x.key, ids); tex_preload(std::move(ids), false); }
    if (!built_candidate(x)) return;
    std::thread([x, c] {
        std::string folder = disc_dir + "/Packfile/" + dmg_strip(x.key);
        uint64_t signature = built_signature(*c, x.key, folder);
        if (built_valid(x.key, signature, x.sz)) return;
        std::vector<uint8_t> d(x.sz, 0);
        build_container(*c, x.key, folder, d.data(), false);
        built_write(x.key, signature, std::move(d));
        printf("disc: %s preparado en segundo plano\n", x.key.c_str()); fflush(stdout);
    }).detach();
}

static std::vector<uint8_t>* disc_build(int i) {
    static int cache_i[16];
    static std::vector<uint8_t> cache_d[16];
    static bool start = false;
    static int next_one = 0;
    static int gen = 0;
    if (!start || gen != rl_gen) { start = true; gen = rl_gen; for (int k = 0; k < 16; k++) cache_i[k] = -1; }
    for (int k = 0; k < 16; k++) if (cache_i[k] == i) return &cache_d[k];
    std::vector<uint8_t>& d = cache_d[next_one];
    cache_i[next_one] = i;
    next_one = (next_one + 1) % 16;
    d.resize(disc_pk[i].sz);
    disc_build_into(i, d.data());
    return &d;
}

static FILE* disc_open_entry(int i) {
    for (int k = 0; k < disc_cache_n; k++) if (disc_cache_idx[k] == i) return disc_cache[k];
    std::string fpath = disc_dir + "/Packfile/" + disc_pk[i].fpath;
    FILE* f = open_file(fpath, "rb");
    if (!f) return nullptr;
    if (disc_cache_n == 8) { fclose(disc_cache[7]); disc_cache_n = 7; }
    for (int k = disc_cache_n; k > 0; k--) { disc_cache[k] = disc_cache[k - 1]; disc_cache_idx[k] = disc_cache_idx[k - 1]; }
    disc_cache[0] = f; disc_cache_idx[0] = i; disc_cache_n++;
    return f;
}

static void disc_index_rebuild() {
    disc_index.assign((disc_pk.size() + 1) * 16, 0);
    memcpy(disc_index.data(), disc_index.data(), 0);
    for (size_t k = 0; k < disc_pk.size(); k++) {
        uint32_t i = (uint32_t)k + 1;
        // the game decodes the size with the 0-based index; an unchanged entry keeps its original size
        uint32_t t = disc_pk[k].sz == disc_pk[k].size0 ? disc_pk[k].real : disc_pk[k].sz;
        uint32_t v[4] = {disc_pk[k].sector ^ KEY_SECTOR, t ^ (i - 1),
                         disc_pk[k].alt == 0xFFFFFFFFu ? (KEY_SECTOR ^ KEY_ALT) : (disc_pk[k].alt ^ KEY_ALT), disc_pk[k].cola};
        memcpy(disc_index.data() + i * 16, v, 16);
    }
}

extern bool (*psp_tex_png)(const uint8_t*, uint32_t, uint32_t, int, int, int, int, int, int*, int*, const uint32_t**);
extern uint32_t (*psp_tex_variant)(const uint8_t*);
extern uint32_t (*psp_tex_matid)(const uint8_t*, int);
extern uint32_t (*psp_tex_dmg1)(const uint8_t*, uint32_t);

static void disc_iso_mark(uint32_t* distinct);

// new characters of the roster (item 29.3): 91 model groups (4 colours + sfx) and 91 voice groups appended after the
// original entries, as Los ijue30s' PACKFILE layout; only the groups of declared characters point to their folder
static size_t st_first = (size_t)-1;   // PACKFILE index of the first new map
static std::vector<std::string> ro_donor;   // per appended entry: the voice donor's file, used when the mod has none
static void roster_entries(uint8_t* header, uint32_t empty_sz, uint32_t* distinct) {
    std::string root_d = mods_root_of(disc_dir);
    std::vector<std::string> ids;
    const char* e = getenv("TTT_MODS");
    if (!(e && !strcmp(e, "0"))) for (auto& m : mods_list(root_d)) if (m.enabled) ids.push_back(m.id);
    roster::load(root_d, ids);
    stages::load(root_d, ids);
    auto& d = roster::defs();
    auto& sd = stages::defs();
    if ((d.chars.empty() && sd.maps.empty()) || disc_pk.size() != roster::MODEL_BASE) return;
    std::vector<std::string> vnames;
    for (int j = 0; j < 126; j++) {
        const std::string& f = disc_pk[0x1159 + j].fpath;
        vnames.push_back(f.substr(f.find_last_of('/') + 1));
    }
    ro_first = disc_pk.size();
    bool keys = false;
    for (auto& ch : d.chars) keys = keys || roster::keyed(ch.sysid);
    for (uint32_t i = roster::MODEL_BASE; i < (keys ? roster::pf_all() : roster::PF_COUNT); i++) {   // keyed groups only when a mod declares one
        PkEntry x;
        x.folder = false; x.sector = x.sector0 = 0; x.cola = 0; x.alt = disc_pk[0x593].alt;   // the original entries' 'no alternate' value
        bool voice = false;
        int j = 0, k = roster::entry_rank(i, &j, &voice);
        x.sz = x.size0 = x.real = voice ? empty_sz : 0;
        const roster::Char* c = nullptr;
        for (auto& ch : d.chars) if (roster::rank(ch.sysid) == k) c = &ch;
        std::string folder = c ? c->folder : "Roster/Unused_" + std::to_string(roster::K_OLD + k);
        std::string tail = voice ? "/Voices/" + vnames[j] : j == 4 ? std::string("/sfx.pak") : "/color_" + std::to_string(j + 1) + ".pak";
        x.fpath = folder + tail;
        // colours never come from another character: a missing colour is the character's own colour 1; only SFX and voices
        // fall back to the base character
        int dk = c ? roster::group_vanilla(c->donor) : -1;
        std::string df;
        if (!voice && j < 4) { if (c && j > 0) df = folder + "/color_1.pak"; }
        else if (dk >= 0) { const std::string& m = disc_pk[0x593 + dk * 5].fpath; df = m.substr(0, m.find_last_of('/')) + tail; }
        ro_donor.push_back(df);
        disc_pk.push_back(x);
    }
    // new maps: map and SFX per map after the roster groups (the SFX fall back to an original map's)
    st_first = disc_pk.size();
    for (auto& m : sd.maps)
        for (int j = 0; j < 2; j++) {
            PkEntry x;
            x.folder = false; x.sector = x.sector0 = 0; x.cola = 0; x.alt = disc_pk[0x495].alt; x.sz = x.size0 = x.real = 0;
            x.fpath = j ? "Maps/SFX/" + m.id + ".pak" : "Maps/" + m.id + ".pak";
            ro_donor.push_back(j ? disc_pk[0x496 + 2 * m.sfx].fpath : std::string());
            disc_pk.push_back(x);
        }
    uint32_t n = (uint32_t)disc_pk.size();
    memcpy(header + 4, &n, 4);
    disc_pk_data = std::max(disc_pk_data, sectors_of((n + 1) * 16));
    ro_appended = true;
    (*distinct)++;
    for (size_t j = 0; j < sd.maps.size(); j++) printf("disc: mapa nuevo %d (entrada %u) desde %s: Maps/%s\n", stages::OLD + (int)j, (unsigned)(st_first + 2 * j), sd.maps[j].mod.c_str(), sd.maps[j].id.c_str());
    for (auto& ch : d.chars) printf("disc: personaje nuevo %d (grupo %d, numero 0x%02X) desde %s: %s\n", ch.sysid, roster::group_of(ch.sysid), roster::menu_of(ch.sysid), ch.mod.c_str(), ch.folder.c_str());
    printf("disc: PACKFILE ampliado a %u entradas, datos en el sector %u\n", n, disc_pk_data);
    if (n >= 32000) printf("disc: AVISO: %u entradas, el juego usa indices de 16 bits (maximo 32767)\n", n);
}

// a new character without SFX or voices of its own uses its base character's folder for them (never for colours)
// a blank PNG made by the port (the size a TTT HUD portrait has; the image itself comes from the BT3 file)
static bool roster_png(const std::string& path, int w, int h, const std::vector<uint32_t>& px);
static bool roster_blank_png(const std::string& path, int w, int h) {
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::u8path(path), ec)) return true;
    return roster_png(path, w, h, std::vector<uint32_t>((size_t)w * h, 0));
}
static bool roster_png(const std::string& path, int w, int h, const std::vector<uint32_t>& px) {   // RGBA, 8 bits
    std::vector<uint8_t> raw, z;
    for (int y = 0; y < h; y++) { raw.push_back(0); const uint8_t* r = (const uint8_t*)&px[(size_t)y * w]; raw.insert(raw.end(), r, r + (size_t)w * 4); }
    bt3::zlib_compress(raw.data(), (uint32_t)raw.size(), z);
    static uint32_t ct[256];
    if (!ct[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; ct[i] = c; }
    std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
        for (int i = 3; i >= 0; i--) f.push_back((uint8_t)(d.size() >> (i * 8)));
        size_t st = f.size(); f.insert(f.end(), t, t + 4); f.insert(f.end(), d.begin(), d.end());
        uint32_t c = 0xFFFFFFFFu; for (size_t i = st; i < f.size(); i++) c = ct[(c ^ f[i]) & 255] ^ (c >> 8); c ^= 0xFFFFFFFFu;
        for (int i = 3; i >= 0; i--) f.push_back((uint8_t)(c >> (i * 8)));
    };
    std::vector<uint8_t> hd(13, 0);
    for (int i = 0; i < 4; i++) { hd[3 - i] = (uint8_t)(w >> (i * 8)); hd[7 - i] = (uint8_t)(h >> (i * 8)); }
    hd[8] = 8; hd[9] = 6;
    chunk("IHDR", hd); chunk("IDAT", z); chunk("IEND", {});
    std::vector<uint8_t> old;
    if (read_file(path, old, true) && old == f) return true;   // same bytes: the date stays and the built caches over it stay valid
    FILE* o = fopen(path.c_str(), "wb");
    if (!o) return false;
    bool ok = fwrite(f.data(), 1, f.size(), o) == f.size();
    fclose(o);
    return ok;
}

// new characters (Packfile/Characters/<id> only in a mod): SFX and voices missing in the mod come from the base character;
// colours 2 to 4 given as a folder take the entries they lack from colour 1; a colour opened from BT3 files gets its TTT entries
// adapted: 000/011/012 textures from its .dbt folders, 013..016 empty, the HUD portraits 045/047/049 (BT3 image, TTT size)
// and the HUD layouts 044/046/048, which are the same in every TTT character (read from the game, the port carries no game data)
static void roster_links() {
    auto& d = roster::defs();
    std::string pk = disc_dir + "/Packfile/", root_d = mods_root_of(disc_dir), cache = disc_dir + "/cache/roster";
    int n = 0, nc = 0, nb = 0;
    std::error_code ec;
    auto isdir = [&](const std::string& p) { return std::filesystem::is_directory(std::filesystem::u8path(p), ec); };
    std::map<int, std::pair<std::string, DirEntry>> layout;   // 44, 46, 48: the game's HUD layouts
    std::string empty = cache + "/empty.dat", p64 = cache + "/hud_64x32.png", p128 = cache + "/hud_128x64.png";
    if (!d.chars.empty()) {
        std::filesystem::create_directories(std::filesystem::u8path(cache), ec);
        if (FILE* f = fopen(empty.c_str(), "ab")) fclose(f);
        roster_blank_png(p64, 64, 32); roster_blank_png(p128, 128, 64);
        for (size_t g = 0; g < roster::K_OLD && layout.size() < 3 && disc_pk.size() > 0x593 + g * 5; g++) {
            std::string t = disc_pk[0x593 + g * 5].fpath;
            t = pk + t.substr(0, t.size() - 4) + "/1_p";
            std::vector<DirEntry> all;
            list_dir_fs(t, all);
            for (auto& e : all) {
                int num = isdigit((unsigned char)e.name[0]) ? atoi(e.name.c_str()) : -1;
                if ((num == 44 || num == 46 || num == 48) && !e.dir && !layout.count(num)) layout[num] = {link_real(t + "/" + e.name), e};
            }
        }
        if (layout.size() != 3) printf("roster: no encuentro los AGS del HUD del juego (%d de 3)\n", (int)layout.size());
    }
    for (auto& c : d.chars) {
        if (std::filesystem::exists(std::filesystem::u8path(pk + c.folder), ec)) continue;
        std::string own = root_d + "/" + c.mod + "/Packfile/" + c.folder + "/";
        std::string c1 = isdir(own + "color_1") ? own + "color_1" : isdir(own + "color_1.pak") ? own + "color_1.pak" : "";
        for (int j = 1; j <= 4; j++) {
            std::string part = "color_" + std::to_string(j), mine = isdir(own + part) ? own + part : isdir(own + part + ".pak") ? own + part + ".pak" : "";
            if (mine.empty()) continue;
            if (j > 1 && !c1.empty()) { link_to[link_key(pk + c.folder + "/" + (mine.size() > 4 && ends_with(mine, ".pak") ? part + ".pak" : part))] = c1; nc++; }
            std::set<int> nums; std::map<int, std::string> dbt;
            for (const std::string& src : {mine, c1}) {
                std::vector<DirEntry> in;
                if (!src.empty()) list_dir_os(src + "/1_p", in);
                for (auto& e : in) {
                    if (!isdigit((unsigned char)e.name[0])) continue;
                    int num = atoi(e.name.c_str());
                    if (e.dir && lower(e.name).compare(0, 17, "011_textures_dmg.") == 0) continue;
                    if (e.dir && ends_with(lower(e.name), ".dbt")) { if (!dbt.count(num)) dbt[num] = src + "/1_p/" + e.name; }
                    else nums.insert(num);
                }
                if (src == mine && !in.empty()) break;
            }
            if (dbt.empty()) continue;   // not a BT3 colour
            std::string v = pk + c.folder + "/" + (ends_with(mine, ".pak") ? part + ".pak" : part) + "/1_p";
            auto add = [&](int num, const std::string& name, const std::string& real, bool dir) {
                if (nums.count(num) || real.empty()) return;
                link_to[link_key(v + "/" + name)] = real;
                DirEntry e; e.name = name; e.dir = dir; e.date = date_of(std::filesystem::u8path(real));
                e.sz = dir ? 0 : (uint64_t)std::filesystem::file_size(std::filesystem::u8path(real), ec);
                link_dirs[link_key(v)].push_back(e); link_gen++;
                nb++;
            };
            add(0, "000_hud.atex", dbt.count(0) ? dbt[0] : "", true);
            add(11, "011_textures.atex", dbt.count(11) ? dbt[11] : "", true);
            add(12, "012_face_blink.atex", dbt.count(12) ? dbt[12] : "", true);
            for (int k = 13; k <= 16; k++) add(k, "0" + std::to_string(k) + "_unused.dat", empty, false);
            for (auto& l : layout) add(l.first, l.second.second.name, l.second.first, false);
            add(45, "045_hud_1.rhg.png", p64, false);
            add(47, "047_hud_2.rhg.png", p64, false);
            add(49, "049_hud_clash.rhg.png", p128, false);
        }
        int dk = roster::group_vanilla(c.donor);
        if (dk < 0 || disc_pk.size() <= (size_t)(0x593 + dk * 5)) continue;
        const std::string& m = disc_pk[0x593 + dk * 5].fpath;
        std::string base = m.substr(0, m.find_last_of('/'));
        for (std::string part : {"sfx", "Voices"}) {
            if (std::filesystem::exists(std::filesystem::u8path(own + part), ec) || std::filesystem::exists(std::filesystem::u8path(own + part + ".pak"), ec)) continue;
            std::string real = link_real(pk + base + "/" + part);
            if (!isdir(real)) { part += ".pak"; real = link_real(pk + base + "/" + part); }
            if (!isdir(real)) continue;
            std::string full = pk + c.folder + "/" + part;
            link_to[link_key(full)] = real;
            DirEntry e; e.name = part; e.dir = true; e.sz = 0; e.date = date_of(std::filesystem::u8path(real));
            link_dirs[link_key(pk + c.folder)].push_back(e); link_gen++;
            n++;
        }
    }
    if (n) printf("roster: %d carpetas de SFX o voces tomadas del personaje base\n", n);
    if (nc) printf("roster: %d colores completan lo que les falta con su color 1\n", nc);
    if (nb) printf("roster: %d entradas TTT adaptadas de archivos BT3\n", nb);
}

std::string psp_assets_dir() { return disc_dir; }

// text of a system id in one language (0 en, 1 es, 2 fr) from assets: Main/system_<lang>.pak, its txt1 ([N] = id N - 1)
// the label a number shows in the game's select screen (kind 0 name, 1 form right, 2 form left), cropped from its RHG
bool psp_roster_label(int lang, int kind, int menu, std::vector<uint32_t>& px, int* w, int* h) {
    static const char* PK[3] = {"char_select_en.pak", "char_select_es.pak", "char_select.pak"};
    static const int NUM[3] = {25, 29, 31};
    static std::map<std::string, std::vector<uint8_t>> agss;
    static std::map<std::string, std::tuple<int, int, std::vector<uint32_t>>> pngs;
    if (lang < 0 || lang > 2 || kind < 0 || kind > 2 || menu < 0) return false;
    static std::set<int> said;
    auto fail = [&](const char* why, const std::string& what) {
        if (said.insert(lang * 16 + kind).second) printf("roster: etiqueta %d de %s (numero %d): %s %s\n", kind, PK[lang], menu, why, what.c_str());
        return false;
    };
    std::string dir = disc_dir + "/Shared/" + psp_shared_rel(std::string("Menus/char_select/") + PK[lang]) + "/1_interface", af, rf;
    std::vector<DirEntry> inp;
    list_dir_fs(dir, inp);
    for (auto& e : inp) {
        if (!isdigit((unsigned char)e.name[0])) continue;
        int v = atoi(e.name.c_str());
        if (v == NUM[kind] && ends_with(lower(e.name), ".ags")) af = dir + "/" + e.name;
        if (v == NUM[kind] + 1 && (ends_with(lower(e.name), ".rhg") || ends_with(lower(e.name), ".rhg.png"))) rf = dir + "/" + e.name;
    }
    if (af.empty() || rf.empty()) return fail("sin AGS o RHG en", dir);
    auto ia = agss.find(af);
    if (ia == agss.end()) { std::vector<uint8_t> d; read_file(link_real(af), d, true); ia = agss.emplace(af, std::move(d)).first; }
    std::vector<ags::State> S;
    if (!ags::parse(ia->second, S)) return fail("AGS no legible", af);
    if (menu >= (int)S.size()) return false;
    int t, tw, th; float r[4];
    if (!ags::rect(ia->second, S[menu], &t, &tw, &th, r) || tw <= 0 || th <= 0) {
        if (ags::on_state(ia->second, S) < 0) return fail("ningun estado con textura recortada en", af);
        return false;
    }
    std::string real = link_real(rf), pf;
    if (ends_with(lower(rf), ".rhg.png")) { if (t == 0) pf = real; }
    else {
        std::vector<DirEntry> ps;
        list_dir_fs(real, ps);
        for (auto& e : ps) if (!e.dir && isdigit((unsigned char)e.name[0]) && atoi(e.name.c_str()) == t + 1 && ends_with(lower(e.name), ".png")) pf = real + "/" + e.name;
    }
    if (pf.empty()) return fail(("sin png de la textura " + std::to_string(t) + " en").c_str(), real);
    auto ip = pngs.find(pf);
    if (ip == pngs.end()) {
        std::vector<uint8_t> f; std::string err; int pw = 0, ph = 0; std::vector<uint32_t> all;
        if (!read_file(link_real(pf), f, true) || !png_decode(f, &pw, &ph, all, &err)) return fail("png no legible", pf);
        ip = pngs.emplace(pf, std::make_tuple(pw, ph, std::move(all))).first;
    }
    int pw = std::get<0>(ip->second), ph = std::get<1>(ip->second);
    const std::vector<uint32_t>& all = std::get<2>(ip->second);
    double sx = (double)pw / tw, sy = (double)ph / th;
    int x0 = std::max(0, (int)(r[0] * sx)), y0 = std::max(0, (int)(r[1] * sy)), x1 = std::min(pw, (int)(r[2] * sx + 0.5)), y1 = std::min(ph, (int)(r[3] * sy + 0.5));
    if (x1 <= x0 || y1 <= y0) return false;
    *w = x1 - x0; *h = y1 - y0;
    px.assign((size_t)*w * *h, 0);
    for (int y = 0; y < *h; y++) memcpy(&px[(size_t)y * *w], &all[(size_t)(y0 + y) * pw + x0], 4 * (size_t)*w);
    return true;
}

std::string psp_system_text(int lang, uint32_t id) {
    static std::vector<std::string> T[3];
    static bool done[3] = {false, false, false};
    if (lang < 0 || lang > 1) return "";
    if (!done[lang]) {
        done[lang] = true;
        static const char* L[2] = {"en", "es"};
        std::string pak;
        for (auto& x : disc_pk) {   // the system pak of that language as the manifest names it
            std::string f = lower(x.fpath), want = std::string("system_") + L[lang];
            size_t sl = f.find_last_of('/');
            std::string b = sl == std::string::npos ? f : f.substr(sl + 1);
            if (b.size() > 4 && ends_with(b, ".pak") && b.find(want) != std::string::npos && b.find(want) + want.size() == b.size() - 4) { pak = disc_dir + "/Packfile/" + x.fpath; break; }
        }
        if (pak.empty()) { printf("roster: el manifiesto no tiene system_%s.pak\n", L[lang]); return ""; }
        if (!is_dir(pak)) pak = pak.substr(0, pak.size() - 4);
        std::vector<DirEntry> inp;
        list_dir(pak, inp);
        std::string tf;
        for (int depth = 0; depth < 2 && tf.empty(); depth++) {
            std::vector<std::string> subs;
            for (auto& e : inp) {
                if (!e.dir) continue;
                if (ends_with(lower(e.name), "_system.txt1") || (ends_with(e.name, ".txt1") && tf.empty())) tf = pak + "/" + e.name;
                else if (!container_type(e.name)) subs.push_back(e.name);
            }
            if (!tf.empty() || subs.empty()) break;
            std::string sub = pak + "/" + subs[0];
            inp.clear();
            for (auto& x : subs) { std::vector<DirEntry> more; list_dir(pak + "/" + x, more); for (auto& e : more) { e.name = x + "/" + e.name; inp.push_back(e); } }
            (void)sub;
        }
        printf("roster: system_%s: %s, txt1 %s (%d entradas en el pak)\n", L[lang], pak.c_str() + disc_dir.size() + 1, tf.empty() ? "no encontrado" : tf.c_str() + pak.size() + 1, (int)inp.size());
        std::vector<DirEntry> in2;
        if (!tf.empty()) list_dir(tf, in2);
        std::string single = tf.empty() ? "" : txt1_unique_name(tf);
        { std::string ls; for (size_t q = 0; q < in2.size() && q < 4; q++) ls += " " + in2[q].name; printf("roster: system_%s: archivos del txt1:%s (%d)\n", L[lang], ls.c_str(), (int)in2.size()); }
        std::vector<std::pair<int, std::string>> nums;
        for (auto& e : in2) {
            if (e.dir) continue;
            int num = 0; char hex[32];
            if (e.name == single || e.name == "texto.txt") {
                std::vector<uint8_t> d; std::vector<std::vector<uint8_t>> texts; std::string err;
                if (read_file(tf + "/" + e.name, d) && txt1_split(d, texts, &err)) { T[lang].clear(); for (auto& t : texts) T[lang].push_back(std::string(t.begin(), t.end())); }
                nums.clear();
                break;
            }
            if (ends_with(e.name, ".txt") && sscanf(e.name.c_str(), "%d-%31[0-9A-Fa-f].", &num, hex) == 2 && num > 0) nums.push_back({num, e.name});
        }
        if (!nums.empty()) {
            std::sort(nums.begin(), nums.end());
            T[lang].assign(nums.back().first, std::string());
            for (auto& q : nums) { std::vector<uint8_t> d; if (read_file(tf + "/" + q.second, d)) T[lang][q.first - 1] = std::string(d.begin(), d.end()); }
        }
        for (auto& t : T[lang]) {
            if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == 0)) t.pop_back();
        }
        printf("roster: textos de %s: %u cadenas\n", pak.c_str() + disc_dir.size() + 1, (unsigned)T[lang].size());
    }
    return id < T[lang].size() ? T[lang][id] : "";
}

// BT3 conversions whose sizes came from the cache: done in the background once the window is open
void psp_disc_warm() {
    std::vector<std::pair<std::string, int>> p, c;
    { std::set<std::pair<std::string, int>> seen; for (auto& x : bt3_warm_p) if (seen.insert(x).second) p.push_back(x); }
    { std::set<std::pair<std::string, int>> seen; for (auto& x : bt3_warm_c) if (seen.insert(x).second) c.push_back(x); }
    bt3_warm_p.clear(); bt3_warm_c.clear();
    if (p.empty() && c.empty()) return;
    std::thread([p, c] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        bt3_bg = true;
        auto t0 = std::chrono::steady_clock::now();
        int yields = 0;
        auto yield = [&] { bool y = false; while (bt3_fg_n.load() > 0 || bt3_now_ms() - bt3_fg_ms.load() < 400) { y = true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } yields += y; };
        for (auto& x : p) { yield(); std::vector<uint8_t>* o; bt3_bytes(x.first, x.second, &o); }
        for (auto& x : c) { yield(); Bt3Lock g; if (!bt3_conv(x.first, x.second)) bt3_conv_make(x.first, x.second, bt3_conv_cache[x.first]); }
        printf("disc: conversion BT3 en segundo plano: %u entradas y %u archivos en %.0f ms, %d pausas por lecturas del juego\n", (unsigned)p.size(), (unsigned)c.size(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), yields);
    }).detach();
}

int psp_stage_base() { return disc_ready && st_first != (size_t)-1 ? (int)st_first : -1; }
// image rect of the map select's big render (state 5 of map_renders_big.ags), 480x272 units
bool psp_map_big_rect(int* r) {
    static int ok = -1, R[4] = {0, 0, 0, 0};
    if (ok < 0) {
        ok = 0;
        std::string f = disc_dir + "/Shared/" + psp_shared_rel("Menus/map_select") + "/map_renders_big.ags";
        std::vector<uint8_t> d; std::vector<ags::State> S;
        int t = 0, w = 0, h = 0; float q[4];
        int step = !read_file(f, d, true) ? 1 : !ags::parse(d, S) ? 2 : S.size() <= 5 ? 3 : !ags::rect(d, S[5], &t, &w, &h, q) ? 4 : 0;
        printf("map3d: %s, %u bytes, %u estados, paso %d\n", f.c_str(), (unsigned)d.size(), (unsigned)S.size(), step); fflush(stdout);
        if (!step) {
            R[0] = (int)floorf(q[0] + 0.5f); R[1] = (int)floorf(q[1] + 0.5f); R[2] = (int)floorf(q[2] + 0.5f); R[3] = (int)floorf(q[3] + 0.5f);
            ok = 1;
        } else if (step == 4) {   // a whole-texture element: its static pose ($info row: position, center, scale)
            const ags::State& st = S[5];
            uint32_t hd = 0x20 + 128 * 5, ng = ags::rd(d, hd + 12), pinfo = st.p0 - 32 * ng, p1 = ags::al16(st.p0 + 8 * st.n0), n1 = ags::rd(d, hd + 28);
            int best = 0;
            for (auto& f : st.refs) {
                if (ags::rd(d, f.pos + 4) == 0xFFFFFFFFu) continue;
                uint32_t sz = ags::rd(d, f.pos + 8), g = ags::rd(d, f.body);
                int w = (int)(sz & 0xFFFF), h = (int)(sz >> 16);
                if (g >= ng || w * h <= best) continue;
                uint32_t row = pinfo + 32 * g, pi = ags::rd(d, row), ci = ags::rd(d, row + 4), si = ags::rd(d, row + 16);
                if (pi >= st.n0 || ci >= st.n0 || si >= n1) continue;
                float pos[2], cen[2], sc[2];
                memcpy(pos, d.data() + st.p0 + 8 * pi, 8); memcpy(cen, d.data() + st.p0 + 8 * ci, 8); memcpy(sc, d.data() + p1 + 8 * si, 8);
                printf("map3d: elemento del render: textura %u %dx%d, posicion (%.1f, %.1f), centro (%.2f, %.2f), escala (%.3f, %.3f), animado %u\n", ags::rd(d, f.pos + 4), w, h, pos[0], pos[1], cen[0], cen[1], sc[0], sc[1], ags::rd(d, f.body + 8));
                bool unit = fabsf(cen[0]) <= 1.0f && fabsf(cen[1]) <= 1.0f;
                float cx = unit ? cen[0] * w : cen[0], cy = unit ? cen[1] * h : cen[1];
                float x0 = pos[0] - cx * sc[0], y0 = pos[1] - cy * sc[1];
                R[0] = (int)floorf(x0 + 0.5f); R[1] = (int)floorf(y0 + 0.5f); R[2] = (int)floorf(x0 + w * sc[0] + 0.5f); R[3] = (int)floorf(y0 + h * sc[1] + 0.5f);
                best = w * h; ok = 1;
            }
            t = 0; w = h = 0;
        }
        printf("map3d: render grande en el AGS: %s (%d, %d)-(%d, %d), textura %d %dx%d\n", ok ? "si" : "no", R[0], R[1], R[2], R[3], t, w, h); fflush(stdout);
    }
    for (int i = 0; i < 4; i++) r[i] = R[i];
    return ok > 0;
}

// HUD portraits (045, 047, 049) inside a closed colour pak of a mod: decoded once to PNG (cache/closed_hud), registered as
// textures of <colour>/1_p/<name>.rhg and marked in the bytes read from the pak, so they are served like any opened texture
struct ClosedTex { uint32_t off, id; };
static std::map<int, std::vector<ClosedTex>> closed_tex;
static std::map<int, std::vector<std::pair<uint32_t, std::vector<uint8_t>>>> closed_patch;   // entry -> bytes written over a read (base HUD AGS, standard RHG sizes)   // entry -> texture data offsets in the file and their ids
static bool rhg_decode(const uint8_t* d, size_t n, std::vector<std::pair<uint32_t, std::vector<uint32_t>>>& out, std::vector<std::pair<int, int>>& sz) {
    auto u32 = [&](size_t o) { return o + 4 <= n ? (uint32_t)d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24 : 0u; };
    auto al = [](uint32_t x, uint32_t a) { return (x + a - 1) / a * a; };
    if (n < 0x30 || memcmp(d, "RHG\0", 4)) return false;
    uint32_t nt = u32(0x14), nd = u32(0x1C), np = u32(0x24);
    if (!nt || nt + nd + np > 4096) return false;
    uint32_t bd = 0x30 + al(nt * 40, 16), bp = bd + al(nd * 36, 16), cur = bp + np * 32;
    struct D { uint32_t fmt, w, h, off, tam; }; struct P { uint32_t off, tam, cols; };
    std::vector<D> dat; std::vector<P> pal;
    for (uint32_t k = 0; k < nd; k++) { size_t o = bd + k * 36; cur = al(cur, 128); dat.push_back({u32(o + 4), u32(o + 8), u32(o + 12), cur, u32(o + 20)}); cur += u32(o + 20); }
    for (uint32_t k = 0; k < np; k++) { size_t o = bp + k * 32; cur = al(cur, 128); pal.push_back({cur, u32(o + 8), u32(o + 12)}); cur += u32(o + 8); }
    if (cur > n) return false;
    for (uint32_t k = 0; k < nt; k++) {
        uint32_t di = u32(0x30 + k * 40 + 4), pi = u32(0x30 + k * 40 + 8), tw = u32(0x30 + k * 40 + 20), th = u32(0x30 + k * 40 + 24);
        if (di >= nd || pi >= np) return false;
        const D& a = dat[di]; const P& b = pal[pi];
        if (!a.tam || !b.tam || !a.w || !a.h || a.w > 1024 || a.h > 1024) return false;
        int bpp = a.fmt == 0x12 || (a.fmt != 0x11 && b.cols <= 16) ? 4 : 8;
        tw = a.w; th = a.h;   // the whole data: a mod's AGS may draw more than the table's size (a 64x64 BT3-style portrait)
        uint32_t step = bpp == 4 ? a.w / 2 : a.w;
        std::vector<uint8_t> lin((size_t)step * a.h, 0);
        bool sw = step % 16 == 0 && a.h % 8 == 0;
        for (uint32_t y = 0; y < a.h; y++)
            for (uint32_t x = 0; x < step; x++) {
                size_t src = sw ? ((size_t)(y / 8) * (step / 16) + x / 16) * 128 + (y % 8) * 16 + x % 16 : (size_t)y * step + x;
                if (src < a.tam && a.off + src < n) lin[(size_t)y * step + x] = d[a.off + src];
            }
        std::vector<uint32_t> px((size_t)tw * th);
        for (uint32_t y = 0; y < th; y++)
            for (uint32_t x = 0; x < tw; x++) {
                uint8_t v = lin[(size_t)y * step + (bpp == 4 ? x / 2 : x)];
                uint32_t i = bpp == 4 ? (x & 1 ? v >> 4 : v & 15) : v, po = b.off + 4 * i;
                px[(size_t)y * tw + x] = 4 * i + 4 <= b.tam && po + 4 <= n ? u32(po) : 0;
            }
        out.push_back({a.off, std::move(px)});
        sz.push_back({(int)tw, (int)th});
    }
    return !out.empty();
}
static void closed_hud_entries() {
    static const char* NAMES[3] = {"045_hud_1.rhg", "047_hud_2.rhg", "049_hud_clash.rhg"};
    static const int NUM[3] = {45, 47, 49};
    int paks = 0, texs = 0, seen = 0, logs = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t k = 0; k < disc_pk.size(); k++) {
        const PkEntry& x = disc_pk[k];
        if (x.folder || !ends_with(lower(x.fpath), ".pak")) continue;
        std::string base = x.fpath.substr(0, x.fpath.size() - 4), l = lower(base);
        size_t sl = l.find_last_of('/');
        if (sl == std::string::npos || l.compare(sl + 1, 6, "color_")) continue;
        std::vector<uint8_t> d;
        seen++;
        if (!read_file(disc_dir + "/Packfile/" + x.fpath, d) || d.size() < 16) { if (logs++ < 6) printf("disc: %s: pak de color cerrado no leido\n", x.fpath.c_str()); continue; }
        auto be = [&](size_t o) { return o + 4 <= d.size() ? (uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3] : 0u; };
        auto entry = [&](uint32_t start, uint32_t size, int i, uint32_t* o, uint32_t* z) {   // entry i of the pak at start
            uint32_t cnt = be(start);
            if (!cnt || cnt > 4096 || (uint32_t)i >= cnt) return false;
            uint32_t a = be(start + 4 + 4 * i), b = (uint32_t)i + 1 < cnt ? be(start + 8 + 4 * i) : be(start + 4 + 4 * cnt);
            if (!b || b > size) b = size;
            if (a >= b || a > size) return false;
            *o = start + a; *z = b - a;
            return true;
        };
        uint32_t ps = 0, pz = (uint32_t)d.size(), o, z;
        bool found = entry(0, pz, 45, &o, &z) && z >= 4 && !memcmp(&d[o], "RHG\0", 4);
        for (uint32_t i = 0, cnt = be(0); !found && i < cnt && i < 64; i++) {   // the HUD files inside a sub-pak (1_p)
            uint32_t so, sz;
            if (entry(0, (uint32_t)d.size(), (int)i, &so, &sz) && entry(so, sz, 45, &o, &z) && z >= 4 && !memcmp(&d[o], "RHG\0", 4)) { ps = so; pz = sz; found = true; }
        }
        if (!found) { if (logs++ < 6) printf("disc: %s: pak de color cerrado sin retratos RHG en la entrada 45\n", x.fpath.c_str()); continue; }
        std::string cdir = disc_dir + "/cache/closed_hud/" + std::to_string(std::hash<std::string>()(l + "|" + std::to_string(d.size())));
        std::vector<ClosedTex> marks;
        for (int h = 0; h < 3; h++) {
            uint32_t eo, ez;
            if (!entry(ps, pz, NUM[h], &eo, &ez)) continue;
            std::vector<std::pair<uint32_t, std::vector<uint32_t>>> tx; std::vector<std::pair<int, int>> sz;
            if (!rhg_decode(&d[eo], ez, tx, sz)) continue;
            auto le = [&](size_t o) { return (uint32_t)d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
            if (h == 1 && tx.size() == 1 && le(eo + 0x1C) == 1 && sz[0].first == 64 && sz[0].second > 32) {   // HUD 2 of another shape (a mod with its own AGS): the TTT shape, an original's AGS and a 64x32 texture
                uint32_t ao, az;
                std::vector<uint8_t> ags;
                if (!read_file(disc_dir + "/Packfile/" + base + "/1_p/046_hud_2.ags", ags, true) || ags.size() < 4 || memcmp(ags.data(), "AGS\0", 4)) {
                    ags.clear();
                    for (size_t j = 0; j < disc_pk.size() && ags.empty(); j++)
                        if (disc_pk[j].folder && ends_with(lower(disc_pk[j].key), "/color_1") && (!read_file(disc_dir + "/Packfile/" + disc_pk[j].key + "/1_p/046_hud_2.ags", ags, true) || ags.size() < 4 || memcmp(ags.data(), "AGS\0", 4))) ags.clear();
                }
                bool slot = entry(ps, pz, 46, &ao, &az);
                if (ags.empty() || !slot || ags.size() > az) printf("disc: %s: HUD 2 de %dx%d con AGS propio, sin AGS base aplicable (base %u B, hueco %u B)\n", x.fpath.c_str(), sz[0].first, sz[0].second, (unsigned)ags.size(), slot ? az : 0);
                else {
                    ags.resize(az, 0);
                    auto& pt = closed_patch[(int)k];
                    pt.push_back({ao, ags});
                    pt.push_back({eo + 0x30 + 20, {64, 0, 0, 0, 32, 0, 0, 0}});
                    pt.push_back({eo + 0x60 + 12, {32, 0, 0, 0}});
                    tx[0].second.resize((size_t)64 * 32);
                    sz[0].second = 32;
                    cdir += "b";
                    printf("disc: %s: HUD 2 con AGS propio, se usa el AGS base de TTT y el retrato a 64x32\n", x.fpath.c_str());
                }
            }
            for (size_t t = 0; t < tx.size(); t++) {
                char nm[16]; snprintf(nm, sizeof nm, "%02d.png", (int)t + 1);
                std::string f = cdir + "/" + NAMES[h] + "/" + nm;
                if (!is_file(f)) { std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(cdir + "/" + NAMES[h]), ec); roster_png(f, sz[t].first, sz[t].second, tx[t].second); }
                TexPng r; r.fpath = f; r.name = nm; r.pak = base + "/1_p/" + NAMES[h]; r.scale = 1; r.psp = false; r.colors = 0;
                r.width0 = r.dw = sz[t].first; r.height0 = r.dh = sz[t].second; r.bpp = 8; r.sz = (uint32_t)r.dw * r.dh;
                tex_reg.push_back(r);
                marks.push_back({eo + tx[t].first, (uint32_t)tex_reg.size()});
                texs++;
                if (logs++ < 12) printf("disc: %s: retrato %s %dx%d reconocible\n", x.fpath.c_str(), NAMES[h], sz[t].first, sz[t].second);
            }
        }
        if (!marks.empty()) { closed_tex[(int)k] = marks; paks++; }
    }
    printf("disc: paks de color cerrados: %d revisados, %d con retratos del HUD, %d texturas reconocibles, %d ms\n", seen, paks, texs, (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}
static void closed_hud_mark(int idx, uint32_t from, uint8_t* d, uint32_t amount) {   // the marks that fall inside a read of the entry
    auto pt = closed_patch.find(idx);
    if (pt != closed_patch.end())
        for (auto& b : pt->second)
            for (uint32_t i = 0; i < b.second.size(); i++) if (b.first + i >= from && b.first + i < from + amount) d[b.first + i - from] = b.second[i];
    auto it = closed_tex.find(idx);
    if (it == closed_tex.end()) return;
    for (const ClosedTex& m : it->second) {
        uint8_t b[16];
        uint32_t id = m.id, chk = ~id;
        memcpy(b, TEX_MARKER, 8); memcpy(b + 8, &id, 4); memcpy(b + 12, &chk, 4);
        for (uint32_t i = 0; i < 16; i++) if (m.off + i >= from && m.off + i < from + amount) d[m.off + i - from] = b[i];
    }
}

// damaged colours (item 30): one entry per colour folder with 1_p_dmg, after every other entry
static std::map<uint32_t, uint32_t> dmg_of, dmg_base;   // 0-based colour entry -> damaged entry, and back
static void dmg_entries(uint8_t* header, uint32_t* distinct) {
    size_t n0 = disc_pk.size();
    for (size_t k = 0; k < n0; k++) {
        const PkEntry& x = disc_pk[k];
        bool closed = !x.folder && ends_with(x.fpath, ".pak");
        if (!x.folder && !closed) continue;
        std::string base = x.folder ? x.key : x.fpath.substr(0, x.fpath.size() - 4), l = lower(base);
        size_t s = l.find_last_of('/');
        if (s == std::string::npos || l.compare(s + 1, 6, "color_")) continue;
        std::string folder = disc_dir + "/Packfile/" + base;
        bool has_dmg;
        { DScope ds_d(22); has_dmg = is_dir(folder + "/1_p_dmg"); }
        if (!has_dmg) continue;
        PkEntry y = x;
        y.key = base + "#dmg";
        if (closed) {   // a closed TTT colour pak (new characters): its entries as slices, 1_p_dmg over them
            DScope ds_c(24);
            std::string real = disc_dir + "/Packfile/" + x.fpath;
            FILE* f = open_file(real, "rb");
            uint8_t h[4]; uint32_t n = 0;
            std::vector<uint8_t> ix;
            if (f && fread(h, 1, 4, f) == 4) { n = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3]; if (n && n < 4096) { ix.resize(4 * (n + 1)); if (fread(ix.data(), 1, ix.size(), f) != ix.size()) n = 0; } else n = 0; }
            if (f) fclose(f);
            if (!n) { printf("disc: %s: el pak cerrado no se pudo leer para el traje danado\n", x.fpath.c_str()); continue; }
            Container c; c.type = 1;
            auto be = [&](uint32_t i) { const uint8_t* p = &ix[4 * i]; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
            for (uint32_t i = 0; i < n; i++) {
                char nm[32]; snprintf(nm, sizeof nm, i < 43 ? "1_p/%03u_pak" : "pak/%04u", i);
                Part q; q.fpath = nm; q.type = 10; q.off = be(i); q.sz = be(i + 1) > be(i) ? be(i + 1) - be(i) : 0;
                c.parts.push_back(q);
            }
            dmg_raw[y.key] = real;
            dmg_scan(c, y.key, folder);
            disc_containers[y.key] = c;
            y.folder = true;
        } else {
            Container c;
            DScope ds_c(23);
            if (!list_container(c, y.key, folder, 1)) { printf("disc: %s: no se pudo abrir 1_p_dmg\n", x.key.c_str()); continue; }
        }
        if (disc_pk.size() >= 32767) break;
        dmg_of[(uint32_t)k] = (uint32_t)disc_pk.size();
        dmg_base[(uint32_t)disc_pk.size()] = (uint32_t)k;
        disc_pk.push_back(y);
        (*distinct)++;
    }
    if (dmg_of.empty()) return;
    uint32_t n = (uint32_t)disc_pk.size();
    memcpy(header + 4, &n, 4);
    disc_pk_data = std::max(disc_pk_data, sectors_of((n + 1) * 16));
    ro_appended = true;
    printf("disc: %u colores con traje danado, PACKFILE con %u entradas\n", (unsigned)dmg_of.size(), n);
}

// Character Reference (G 63): menu pak of a mod (Packfile/Menus/menu_chara_reference, pak or folder), appended as one entry
static size_t cr_index = (size_t)-1, cr_map_index = (size_t)-1;
static void cr_entries(uint8_t* header, uint32_t* distinct) {
    const char* e = getenv("TTT_CHAR_REF");
    if ((e && *e == '0') || disc_pk.size() <= 56 || disc_pk.size() >= 32767) return;
    std::string rel = "Menus/menu_chara_reference", real = disc_dir + "/Packfile/" + rel;
    PkEntry y = disc_pk[56];
    uint64_t t = 0;
    if (pak_folder(y, real, rel, 1)) y.fpath = rel + ".pak";
    else if (is_file(real + ".pak", &t)) { y.fpath = rel + ".pak"; y.key.clear(); y.folder = false; y.sz = (uint32_t)t; }
    else return;
    cr_index = disc_pk.size();
    disc_pk.push_back(y);
    uint32_t n = (uint32_t)disc_pk.size();
    memcpy(header + 4, &n, 4);
    disc_pk_data = std::max(disc_pk_data, sectors_of((n + 1) * 16));
    ro_appended = true;
    (*distinct)++;
    printf("disc: referencia de personajes en la entrada %u (%s, %u bytes)\n", (unsigned)cr_index, y.folder ? "carpeta" : "pak", y.folder ? 0u : y.sz);
    // 3D viewer map (TTT map pak, stage entry template)
    std::string mrel = "Menus/chara_reference_map", mreal = disc_dir + "/Packfile/" + mrel;
    PkEntry m = disc_pk[0x495];
    if (pak_folder(m, mreal, mrel, 1)) m.fpath = mrel + ".pak";
    else if (is_file(mreal + ".pak", &t)) { m.fpath = mrel + ".pak"; m.key.clear(); m.folder = false; m.sz = (uint32_t)t; }
    else { printf("disc: referencia de personajes sin mapa del visor (Menus/chara_reference_map)\n"); return; }
    cr_map_index = disc_pk.size();
    disc_pk.push_back(m);
    n = (uint32_t)disc_pk.size();
    memcpy(header + 4, &n, 4);
    disc_pk_data = std::max(disc_pk_data, sectors_of((n + 1) * 16));
    (*distinct)++;
    printf("disc: mapa del visor de la referencia de personajes en la entrada %u (%s, %u bytes)\n", (unsigned)cr_map_index, m.folder ? "carpeta" : "pak", m.folder ? 0u : m.sz);
}
// Character Reference voices: one AT3 entry per wav (Chi-Chi in chara_reference_voices, character samples in chara_reference_samples),
// English in the mod's Packfile, other languages as voice pack alternatives (Lang/<pack>/Packfile)
static std::map<std::string, int> cr_voice_idx;
static void cr_voice_entries(uint8_t* header, uint32_t* distinct) {
    cr_voice_idx.clear();
    if (cr_index == (size_t)-1) return;
    std::vector<std::string> names;
    char b[64];
    for (int k = 0; k < 225; k++) { snprintf(b, sizeof b, "Menus/chara_reference_voices/chichi_%03d", k); names.push_back(b); }
    for (int c = 0; c < 161; c++) for (int l = 0; l < 2; l++) { snprintf(b, sizeof b, "Menus/chara_reference_samples/chara_%03d_%d", c, l); names.push_back(b); }
    int first = -1, n = 0;
    for (auto& rel : names) {
        if (disc_pk.size() >= 32767) break;
        PkEntry y = disc_pk[cr_index];
        y.fpath = rel + ".unk"; y.key.clear(); y.folder = false; y.sz = 0;
        bool empty = false;
        if (!at3_entry(y, disc_dir + "/Packfile/" + y.fpath, y.fpath, &empty)) continue;
        if (first < 0) first = (int)disc_pk.size();
        cr_voice_idx[lower(rel.substr(rel.find_last_of('/') + 1))] = (int)disc_pk.size();
        disc_pk.push_back(y);
        n++;
    }
    if (!n) return;
    uint32_t cnt = (uint32_t)disc_pk.size();
    memcpy(header + 4, &cnt, 4);
    disc_pk_data = std::max(disc_pk_data, sectors_of((cnt + 1) * 16));
    (*distinct)++;
    printf("disc: referencia de personajes: %d voces en las entradas %d..%d\n", n, first, (int)cnt - 1);
}
int psp_cr_voice(const std::string& name) { auto it = cr_voice_idx.find(lower(name)); return disc_ready && it != cr_voice_idx.end() ? it->second : -1; }
// which voice language serves a Character Reference voice entry now (diagnostic)
std::string psp_cr_voice_info(int idx) {
    if (!disc_ready || idx < 0 || idx >= (int)disc_pk.size()) return "";
    auto it = disc_containers.find(disc_pk[idx].key);
    uint32_t id = it != disc_containers.end() && !it->second.parts.empty() ? it->second.parts[0].id : 0;
    std::string alts;
    auto a = at3_alt.find(id);
    if (a != at3_alt.end()) for (size_t i = 0; i < a->second.size() && i < voice_packs.size(); i++) if (a->second[i]) alts += (alts.empty() ? "" : ",") + voice_packs[i].id;
    int k = psp_voice_lang;
    char b[256];
    snprintf(b, sizeof b, "idioma %s, audio %u, en los paquetes [%s]", k > 0 && k <= (int)voice_packs.size() ? voice_packs[k - 1].id.c_str() : "del juego", id, alts.c_str());
    return b;
}
int psp_cr_pak() { return disc_ready && cr_index != (size_t)-1 ? (int)cr_index : -1; }
// a colour entry whose 1_p has level 1 damage textures (011_textures_dmg.*)
std::string psp_disc_key(uint32_t idx) { return disc_ready && idx < disc_pk.size() ? (disc_pk[idx].key.empty() ? disc_pk[idx].fpath : disc_pk[idx].key) : std::string("-"); }
// optional name of a colour (mod creator): <colour folder>/color_name.txt, "en = ..." / "es = ...", a line without a key = every language
std::string psp_colour_name(uint32_t idx, int lang) {
    if (!disc_ready || idx >= disc_pk.size()) return "";
    static std::mutex mx;
    static std::map<uint32_t, std::array<std::string, 2>> memo;
    std::lock_guard<std::mutex> g(mx);
    auto it = memo.find(idx);
    if (it == memo.end()) {
        std::array<std::string, 2> nm;
        std::string k = dmg_strip(disc_pk[idx].key.empty() ? disc_pk[idx].fpath : disc_pk[idx].key);
        if (ends_with(lower(k), ".pak")) k.resize(k.size() - 4);
        std::vector<uint8_t> f;
        if (!lower(k).compare(0, 11, "characters/") && read_file(disc_dir + "/Packfile/" + k + "/color_name.txt", f)) {
            std::string t(f.begin(), f.end()), line;
            if (t.size() >= 3 && !t.compare(0, 3, "\xEF\xBB\xBF")) t.erase(0, 3);
            size_t a = 0;
            while (a < t.size()) {
                size_t b = t.find('\n', a); line = t.substr(a, b == std::string::npos ? std::string::npos : b - a); a = b == std::string::npos ? t.size() : b + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                size_t e = line.find('=');
                std::string key = e == std::string::npos ? "" : lower(line.substr(0, e)), v = e == std::string::npos ? line : line.substr(e + 1);
                while (!key.empty() && key.back() == ' ') key.pop_back();
                while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                if (v.empty()) continue;
                if (key == "en") nm[0] = v; else if (key == "es") nm[1] = v; else if (key.empty()) { if (nm[0].empty()) nm[0] = v; if (nm[1].empty()) nm[1] = v; }
            }
            for (int l = 0; l < 2; l++) if (nm[l].empty()) nm[l] = nm[1 - l];
            if (!nm[0].empty()) printf("colores: nombre del color %s: %s / %s\n", k.c_str(), nm[0].c_str(), nm[1].c_str());
        }
        it = memo.emplace(idx, nm).first;
    }
    return it->second[lang == 1 ? 1 : 0];
}
bool psp_disc_lv1(uint32_t idx) {
    if (!disc_ready || idx >= disc_pk.size()) return false;
    std::string k = lower(dmg_strip(disc_pk[idx].key.empty() ? disc_pk[idx].fpath : disc_pk[idx].key));
    if (ends_with(k, ".pak")) k.resize(k.size() - 4);
    for (auto& s : lv1_keys) if (!k.empty() && (ends_with(s, k.c_str()) || ends_with(k, s.c_str()))) return true;
    // not built yet on this machine: the files decide (the select must answer the same on every machine of a net battle)
    static std::mutex mx;
    static std::map<uint32_t, bool> memo;
    std::lock_guard<std::mutex> g(mx);
    auto it = memo.find(idx);
    if (it != memo.end()) return it->second;
    std::string key = dmg_strip(disc_pk[idx].key.empty() ? disc_pk[idx].fpath : disc_pk[idx].key);
    if (ends_with(lower(key), ".pak")) key.resize(key.size() - 4);
    std::vector<DirEntry> inp;
    list_dir(disc_dir + "/Packfile/" + key + "/1_p", inp);
    bool r = false;
    for (auto& e : inp) if (lower(e.name).compare(0, 17, "011_textures_dmg.") == 0) { r = true; break; }
    memo[idx] = r;
    return r;
}
bool psp_disc_blink(uint32_t idx) {
    if (!disc_ready || idx >= disc_pk.size()) return false;
    std::string k = lower(dmg_strip(disc_pk[idx].key.empty() ? disc_pk[idx].fpath : disc_pk[idx].key));
    if (ends_with(k, ".pak")) k.resize(k.size() - 4);
    for (auto& s : blink_keys) if (!k.empty() && (ends_with(s, k.c_str()) || ends_with(k, s.c_str()))) return true;
    return false;
}
int psp_cr_map() { return disc_ready && cr_map_index != (size_t)-1 ? (int)cr_map_index : -1; }
int psp_col_count(int s) { return col_ready && s >= 0 && s < roster::ALL ? col_n[s] : 0; }
// first enabled mod (priority order) that has the file
std::string psp_cr_file(const std::string& rel) {
    std::error_code ec;
    for (auto& m : mod_active) {
        std::string p = mods_root_global + "/" + m.first + "/" + rel;
        if (std::filesystem::is_regular_file(std::filesystem::u8path(p), ec)) return p;
    }
    return "";
}
// the game's model index is the 0-based entry (index 0x593 = disc_pk[0x593], roster ro_model)
bool psp_disc_dmg(uint32_t idx, uint32_t* out) {
    if (!disc_ready) return false;
    auto it = dmg_of.find(idx);
    if (it == dmg_of.end()) return false;
    *out = it->second;
    return true;
}
// folder of an original character's colour (group k, colour 0..3), Packfile-relative
std::string psp_colour_folder(int k, int j) {
    size_t i = 0x593 + (size_t)k * 5;
    if (k < 0 || j < 0 || j >= COL_MAX || i >= disc_pk.size()) return "";
    std::string f = disc_pk[i].fpath;
    size_t s = f.find_last_of('/');
    return s == std::string::npos ? "" : f.substr(0, s) + "/color_" + std::to_string(j + 1);
}
bool psp_original_exists(int k, int kind);
// an original's colour j (0..9) is served: 1..4 found on disc, 5..10 an extra entry
bool psp_colour_exists(int k, int j) {
    if (!disc_ready || k < 0 || j < 0 || j >= COL_MAX) return false;
    if (j < 4) return psp_original_exists(k, j);
    return col_x.count((uint32_t)(0x593 + 5 * k) * 16 + (uint32_t)j) > 0;
}
// an original character's files (group k): 0..3 colour folder, 4 SFX pak, 5 voices folder; Packfile-relative
std::string psp_original_path(int k, int kind) {
    if (kind < 4) return psp_colour_folder(k, kind);
    size_t i = kind == 4 ? 0x593 + (size_t)k * 5 + 4 : 0x1159 + (size_t)k * 126;
    if (k < 0 || i >= disc_pk.size()) return "";
    std::string f = disc_pk[i].fpath;
    if (kind == 4) return f;
    size_t s = f.find_last_of('/');
    return s == std::string::npos ? "" : f.substr(0, s);
}
// wav of voice line n of character s: lang 0 the game's (or a mod's), k the voice pack k - 1; "" when there is none
std::string psp_voice_wav(int s, int n, int lang) {
    if (!disc_ready || n < 0 || n >= 126) return "";
    int g = roster::group_vanilla(s), r = g < 0 ? roster::rank(s) : -1;
    size_t i = g >= 0 ? 0x1159 + (size_t)g * 126 + (size_t)n : r >= 0 ? (size_t)roster::voice_entry(r) + (size_t)n : (size_t)-1;
    if (i >= disc_pk.size()) return "";
    std::string key = disc_pk[i].key.empty() ? disc_pk[i].fpath : disc_pk[i].key;
    if (lang > 0) {
        if (lang > (int)voice_packs.size()) return "";
        auto& w = voice_packs[lang - 1].wav;
        auto it = w.find(lower(key.substr(0, key.find_last_of('.'))) + ".wav");
        return it == w.end() ? "" : it->second;
    }
    auto c = disc_containers.find(key);
    if (c == disc_containers.end() || c->second.type != 5 || c->second.parts.empty() || at3_only.count((uint32_t)c->second.parts[0].id)) return "";
    std::string p = mod_path(c->second.parts[0].fpath);
    return is_file(p) ? p : "";
}
// character folder (Packfile-relative) of a character pak read at guest address pak: the folder of its group's SFX pak
std::string psp_pak_char_dir(uint32_t pak) {
    int idx = -1;
    { std::lock_guard<std::mutex> g(lip_mx); auto it = lip_paks.find(pak); if (it != lip_paks.end()) idx = it->second; }
    if (idx < 0 || (size_t)idx >= disc_pk.size()) return "";
    size_t i = (size_t)idx;
    if (i >= 0x593 && i < 0x593 + 350) i = 0x593 + (i - 0x593) / 5 * 5 + 4;
    else if (i >= roster::MODEL_BASE && i < roster::VOICE_BASE) i = roster::MODEL_BASE + (i - roster::MODEL_BASE) / 5 * 5 + 4;
    else if (i >= roster::DYN_MODEL && i < roster::dyn_voice()) i = roster::DYN_MODEL + (i - roster::DYN_MODEL) / 5 * 5 + 4;
    if (i >= disc_pk.size()) return "";
    std::string f = disc_pk[i].fpath;
    size_t c = f.find("/color_");
    if (c != std::string::npos) f = f.substr(0, c + 1);
    size_t s = f.find_last_of('/');
    return s == std::string::npos ? "" : f.substr(0, s);
}
// pre-animation camera of a character pak: <colour>/1_p/pre_cman/<name> (or <colour>/pre_cman/<name>), a mod first, then the game's tree; colour 1 as fallback; empty files skipped
std::string psp_pre_cman(uint32_t pak, const std::string& name, std::string* where) {
    int idx = -1;
    { std::lock_guard<std::mutex> g(lip_mx); auto it = lip_paks.find(pak); if (it != lip_paks.end()) idx = it->second; }
    if (!disc_ready || idx < 0 || (size_t)idx >= disc_pk.size()) return "";
    std::string k = dmg_strip(disc_pk[(size_t)idx].key);
    if (where) *where = k;
    std::vector<std::string> ks = {k};
    size_t c = k.find("/color_");
    if (c != std::string::npos) {
        size_t e = k.find('/', c + 1);
        std::string k1 = k.substr(0, c) + "/color_1" + (e == std::string::npos ? "" : k.substr(e));
        if (lower(k1) != lower(k)) ks.push_back(k1);
    }
    for (auto& d : ks)
        for (const char* sub : {"/1_p/pre_cman/", "/pre_cman/"}) {
            std::string rel = d + sub + name, m = psp_cr_file("Packfile/" + rel);
            uint64_t sz = 0;
            if (!m.empty() && is_file(m, &sz) && sz >= 0x20) return m;
            if (is_file(disc_dir + "/Packfile/" + rel, &sz) && sz >= 0x20) return disc_dir + "/Packfile/" + rel;
        }
    return "";
}
// the game has that file of an original character (group k; kinds as psp_original_path)
bool psp_original_exists(int k, int kind) {
    if (!disc_ready || k < 0 || kind < 0 || kind > 5) return false;
    if (kind == 5) { std::string f = psp_original_path(k, 5); return !f.empty() && is_dir(disc_dir + "/Packfile/" + f); }
    size_t i = 0x593 + (size_t)k * 5 + kind;
    return i < col_found.size() && col_found[i];
}
uint32_t psp_disc_size(uint32_t idx) { return disc_ready && idx < disc_pk.size() ? disc_pk[idx].sz : 0; }
uint32_t psp_disc_dmg_base(uint32_t idx) {
    if (!disc_ready) return idx;
    auto it = dmg_base.find(idx);
    return it == dmg_base.end() ? idx : it->second;
}

bool psp_disc_pf(uint32_t* count, uint32_t* sectors) {
    if (!disc_ready || !ro_appended) return false;
    *count = (uint32_t)disc_pk.size();
    *sectors = disc_pk_data;
    return true;
}

// whole entries of a voice pack (SFX paks): read from the pack's file while its language is chosen; the entry takes the longest size
static std::unordered_map<size_t, std::vector<std::string>> voice_raw_of;
static void voice_raw_size(size_t k, uint32_t* distinct) {
    if (voice_packs.empty()) return;
    std::string rel = lower(disc_pk[k].fpath);
    std::vector<std::string> alt(voice_packs.size());
    bool any = false;
    for (size_t i = 0; i < voice_packs.size(); i++) {
        auto it = voice_packs[i].raw.find(rel);
        if (it == voice_packs[i].raw.end()) continue;
        std::error_code ec;
        uint64_t n = std::filesystem::file_size(std::filesystem::u8path(it->second), ec);
        if (ec || !n) continue;
        alt[i] = it->second; any = true;
        if (n > disc_pk[k].sz) { disc_pk[k].sz = (uint32_t)n; (*distinct)++; }
    }
    if (any) voice_raw_of[k] = alt;
}
static const std::string* voice_raw_file(size_t k) {
    int l = psp_voice_lang;
    if (l <= 0) return nullptr;
    auto it = voice_raw_of.find(k);
    return it != voice_raw_of.end() && l <= (int)it->second.size() && !it->second[l - 1].empty() ? &it->second[l - 1] : nullptr;
}

static bool disc_init() {
    const char* e = getenv("TTT_ASSETS");
    if (e && !strcmp(e, "0")) return false;
    disc_dir = e && *e ? e : std::string(GAME_DIR) + "/../assets";
    if (const char* t = getenv("TTT_TEX")) tex_enabled = atoi(t);
    if (tex_log < 0) { const char* t = getenv("TTT_TEX_LOG"); tex_log = t ? atoi(t) : 0; }
    if (tex_fit < 0) { const char* t = getenv("TTT_TEX_FIT"); tex_fit = t ? atoi(t) : 1; }
    if (const char* t = getenv("TTT_AT3")) at3_enabled = atoi(t);
    if (const char* t = getenv("TTT_AT3_LOG")) at3_log = atoi(t);
    if (tex_enabled) { psp_tex_png = tex_png_buscar; psp_tex_variant = tex_variant; psp_tex_matid = tex_mat_id; psp_tex_dmg1 = tex_dmg1_id; }
    std::string manifest = disc_dir + "/manifiesto.txt";
    FILE* f = fopen(manifest.c_str(), "r");
    if (!f) return false;
    const char* v = getenv("TTT_DISC_VERIFY");
    disc_verify_mode = v ? atoi(v) : 0;
    v = getenv("TTT_DISC_TRACE");
    disc_trace = v ? atoi(v) : 0;
    char line[1024];
    uint8_t header[16];
    memset(header, 0, 16);
    unsigned n = 0, sd = 112, sz = 0;
    char hex[64] = "";
    bool key_base0 = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            if (!strncmp(line, "# clave base 0", 14)) key_base0 = true;
            if (sscanf(line, "# packfile entradas %u sector_datos %u tamano %u cabecera %63s", &n, &sd, &sz, hex) >= 3) {
                for (int k = 0; k < 16 && hex[k * 2]; k++) {
                    unsigned b = 0;
                    sscanf(hex + k * 2, "%2x", &b);
                    header[k] = (uint8_t)b;
                }
            }
            continue;
        }
        unsigned i = 0, sec = 0, t = 0, cola = 0;
        char alt[32], fpath[768];
        if (sscanf(line, "%u\t%u\t%u\t%31s\t%x\t%*x\t%767[^\r\n]", &i, &sec, &t, alt, &cola, fpath) != 6) continue;
        if (i != disc_pk.size() + 1) { printf("disc: manifiesto desordenado en la entrada %u\n", i); fclose(f); return false; }
        PkEntry x;
        x.folder = false;
        x.sector = sec; x.sz = t; x.cola = cola;
        x.sector0 = sec; x.size0 = t;
        x.real = key_base0 ? t : t ^ i ^ (i - 1);   // old manifest: size decoded with the 1-based index
        x.alt = alt[0] == '-' ? 0xFFFFFFFFu : (uint32_t)strtoul(alt, nullptr, 10);
        x.fpath = fpath;
        disc_pk.push_back(x);
    }
    fclose(f);
    if (disc_pk.empty() || disc_pk.size() != n) { printf("disc: manifiesto incompleto (%u entradas declaradas, %u leidas)\n", n, (unsigned)disc_pk.size()); return false; }
    disc_pk_data = sd;
    disc_pk_size = sz;

    // real sizes: if any changed, everything behind the index is relocated
    uint32_t distinct = 0, opened = 0, empties = 0, emptied = 0, empty_sz = 0, dropped = 0;
    {
        std::map<uint32_t, int> sizes;
        for (auto& x : disc_pk) if (ends_with(x.fpath, ".unk") && x.size0 < AT3_STEP) sizes[x.size0]++;
        int best = 0; for (auto& kv : sizes) if (kv.second > best) { best = kv.second; empty_sz = kv.first; }
    }
    psp_boot_mark("disco: manifiesto");
    roster_entries(header, empty_sz, &distinct);
    psp_boot_mark("disco: entradas nuevas");
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    v = getenv("TTT_DISC_RESCAN");
    disc_rescan = v && atoi(v);
    links_load();
    roster_links();
    psp_boot_mark("disco: enlaces de Shared");
    txt1_info_load();
    if (!disc_rescan) { disc_cache_read(); tex_hdr_read(); }
    tex_orig_read();
    psp_boot_mark("disco: caches leidas");
    dprof_print("antes de mods");
    mods_init();
    psp_boot_mark("disco: mods");
    dprof_print("mods");
    if (mod_enabled) {
        std::vector<std::string> d;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& p) { if (seen.insert(lower(p)).second) d.push_back(p); };
        for (auto& kv : mod_dirs) add(link_real(disc_dir + "/Packfile" + (kv.first.empty() ? std::string() : "/" + kv.first)));
        for (auto& kv : mod_files) { size_t c = kv.second.find_last_of("/\\"); if (c != std::string::npos) add(kv.second.substr(0, c)); }
        printf("disc: %u carpetas para listar por adelantado\n", (unsigned)d.size());
        ls_prefetch(std::move(d));
    }
    scan_memo = true;
    col_found.assign(disc_pk.size(), 0);
    struct CatT { uint32_t n = 0, mods = 0; uint64_t us = 0; };
    std::map<std::string, CatT> cat_t;
    for (size_t k = 0; k < disc_pk.size(); k++) {
        auto c0 = std::chrono::steady_clock::now();
        struct CatAdd { std::map<std::string, CatT>& m; const PkEntry& x; std::chrono::steady_clock::time_point t0;
            ~CatAdd() {
                std::string f = x.fpath; size_t s = f.find('/');
                CatT& c = m[s == std::string::npos ? std::string("(raiz)") : f.substr(0, s)];
                c.n++; c.us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
                std::string l = lower(f); if (mod_enabled && (mod_dirs.count(l) || (ends_with(l, ".pak") && mod_dirs.count(l.substr(0, l.size() - 4))))) c.mods++;
            } } cat_add_{cat_t, disc_pk[k], c0};
        bool donor_try = false;
      again:
        std::string fpath = disc_dir + "/Packfile/" + disc_pk[k].fpath;
        uint64_t t = 0;
        col_found[k] = !donor_try;
        if (!is_file(fpath, &t)) {
            int type = container_type(disc_pk[k].fpath);
            if (type && pak_folder(disc_pk[k], fpath, disc_pk[k].fpath, type)) { opened++; distinct++; continue; }
            if (type == 1 && pak_folder(disc_pk[k], fpath.substr(0, fpath.size() - 4), disc_pk[k].fpath.substr(0, disc_pk[k].fpath.size() - 4), 1)) { opened++; distinct++; continue; }
            col_found[k] = 0;
            bool empty = false;
            if (at3_entry(disc_pk[k], fpath, disc_pk[k].fpath, &empty)) { opened++; distinct++; continue; }
            if (empty) {   // served as zeros with the size the game gives its own empty slots
                uint32_t ez = disc_pk[k].size0 < AT3_STEP ? disc_pk[k].size0 : empty_sz;
                if (disc_pk[k].sz != ez) { disc_pk[k].sz = ez; distinct++; }
                empties++; emptied++;
                continue;
            }
            // a .unk shorter than one frame is an empty voice slot: its absence is the slot, served
            // as zeros with the manifest size. Dropping a wav with that name enables it
            if (ends_with(disc_pk[k].fpath, ".unk") && disc_pk[k].size0 < AT3_STEP) { empties++; continue; }
            if (lang_dropped(disc_pk[k].fpath)) {   // French entry not extracted or removed: served empty
                if (disc_pk[k].sz) { disc_pk[k].sz = 0; distinct++; }
                dropped++;
                continue;
            }
            if (k >= ro_first) {   // a new character's file the mod does not have: the base character's, else empty
                const std::string& df = ro_donor[k - ro_first];
                if (!donor_try && !df.empty()) {
                    static int logs = 0;
                    if (logs++ < 6) printf("disc: %s: falta en el mod, se usa %s\n", disc_pk[k].fpath.c_str(), df.c_str());
                    disc_pk[k].fpath = df; donor_try = true; distinct++;
                    goto again;
                }
                if (k >= st_first && !((k - st_first) & 1)) printf("mapas: falta %s en el mod (pak o carpeta)\n", disc_pk[k].fpath.c_str());
                disc_pk[k].sz = 0;
                continue;
            }
            size_t ci = k - (COL_MODEL - 1);
            if (!donor_try && k >= COL_MODEL - 1 && ci < (size_t)roster::K_OLD * 5 && ci % 5 && ci % 5 < 4) {   // an original's colour 2..4 that is not there: its colour 1
                static int logs = 0;
                if (logs++ < 6) printf("disc: %s: color no extraido, se usa el color 1\n", disc_pk[k].fpath.c_str());
                disc_pk[k].fpath = disc_pk[k - ci % 5].fpath; donor_try = true; distinct++;
                goto again;
            }
            printf("disc: falta %s\n", disc_pk[k].fpath.c_str());
            scan_memo = false; memo_clear();
            ls_end();
            return false;
        }
        if (bt3_stage_entry(disc_pk[k], fpath, t)) { opened++; distinct++; continue; }
        if ((uint32_t)t != disc_pk[k].sz) { disc_pk[k].sz = (uint32_t)t; distinct++; }
    }
    for (auto& x : disc_pk) if (!bt3_stage_of.count(x.fpath)) ttt_map_fx_entry(x, disc_dir + "/Packfile/" + x.fpath);   // TTT maps (ISO or mods) with a mod's effects
    printf("mapas: efectos del mod en mapas TTT: %d mapas revisados, %d con .fx.txt, %d con burbujas o tormenta, %d registrados\n", ttt_fx_seen[0], ttt_fx_seen[1], ttt_fx_seen[2], (int)ttt_fx_tok.size());
    psp_boot_mark("disco: entradas revisadas");
    if (dropped) printf("disc: %u entradas francesas sin archivo, se sirven vacias\n", dropped);
    dprof_print("revision de entradas");
    {
        std::vector<std::pair<uint64_t, std::string>> o;
        for (auto& kv : cat_t) o.push_back({kv.second.us, kv.first});
        std::sort(o.rbegin(), o.rend());
        for (auto& x : o) { const CatT& c = cat_t[x.second]; printf("arranque: categoria %s: %u entradas (%u con mods), %llu ms\n", x.second.c_str(), c.n, c.mods, (unsigned long long)(c.us / 1000)); }
    }
    scan_memo = false; memo_clear();
    ls_memo_on = false;
    col_extra(header, &distinct);
    col_counts();
    psp_boot_mark("disco: colores extra");
    scan_memo = true;
    if (!ls_threads.empty()) ls_memo_on = true;
    dmg_entries(header, &distinct);
    closed_hud_entries();
    scan_memo = false; memo_clear();
    ls_end();
    psp_boot_mark("disco: entradas de trajes rotos");
    dprof_print("trajes rotos");
    cr_entries(header, &distinct);
    cr_voice_entries(header, &distinct);
    psp_boot_mark("disco: entradas de Character Reference");
    resolve_txt1(disc_dir + "/Packfile/");
    psp_boot_mark("disco: txt1");
    for (auto& x : disc_pk) if (x.folder) x.sz = compute_size(x.key);
    psp_boot_mark("disco: tamanos de carpetas");
    for (size_t k = 0; k < disc_pk.size(); k++) voice_raw_size(k, &distinct);
    psp_boot_mark("disco: tamanos de contenedores");
    disc_cache_write();
    bt3_sizes_write();
    hc_write();
    sig_memo.clear(); dirsig_memo.clear();
    tex_hdr_write();
    disc_iso_mark(&distinct);
    psp_boot_mark("disco: caches escritas y marca del ISO");
    if (distinct) {
        uint32_t cur = 0;
        for (auto& x : disc_pk) { x.sector = cur; cur += sectors_of(x.sz); }
        disc_relocated = true;
    }
    if (const char* q = getenv("TTT_DISC_LIST"))
        for (auto& x : disc_pk)
            if (x.folder && x.fpath == q) {
                const Container& c = *container_of(x.key);
                printf("disc: lista de %s, %u partes\n", q, (unsigned)c.parts.size());
                for (size_t k = 0; k < c.parts.size(); k++) printf("  %3u %8u %s%s\n", (unsigned)k + 1, c.parts[k].sz, c.parts[k].fpath.c_str(), c.parts[k].type ? " (anidado)" : "");
            }
    disc_index_rebuild();
    psp_boot_mark("disco: indice");
    memcpy(disc_index.data(), header, 16);

    // loose disc files and then the PACKFILE, each at its own sector
    uint32_t lbn = 32;
    std::error_code ec;
    std::string root_d = disc_dir + "/Disco";
    for (auto& p : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(root_d), ec)) {
        if (!p.is_regular_file()) continue;
        DiscFile a;
        a.local = p.path().string();
        a.fpath = std::filesystem::relative(p.path(), std::filesystem::u8path(root_d)).generic_string();
        a.sz = (uint32_t)std::filesystem::file_size(p.path());
        a.lbn = lbn;
        lbn += sectors_of(a.sz);
        disc_files.push_back(a);
        disc_spans.push_back({a.lbn, sectors_of(a.sz), a.sz, 0, (int)disc_files.size() - 1});
    }
    disc_pk_lbn = lbn;
    uint32_t end_pos = disc_pk_data;
    for (auto& x : disc_pk) end_pos = std::max(end_pos, disc_pk_data + x.sector + sectors_of(x.sz));
    disc_pk_sectors = end_pos;
    disc_spans.push_back({disc_pk_lbn, disc_pk_data, (uint32_t)disc_index.size(), 1, 0});
    for (size_t k = 0; k < disc_pk.size(); k++)
        disc_spans.push_back({disc_pk_lbn + disc_pk_data + disc_pk[k].sector, sectors_of(disc_pk[k].sz), disc_pk[k].sz, 2, (int)k});
    std::stable_sort(disc_spans.begin(), disc_spans.end(), [](const DiscSpan& a, const DiscSpan& b) { return a.lbn != b.lbn ? a.lbn < b.lbn : a.sectors < b.sectors; });
    if (disc_relocated || !disc_pk_size) disc_pk_size = disc_pk_sectors * DISC_SECTOR;
    disc_ready = true;
    rl_start();
    { int shown = 0; for (auto& m : mod_audio) if (m.second.first && shown++ < 80) printf("mods: voces %s: %d de %d desde mods, %d vaciadas\n", m.first.c_str(), m.second.first, m.second.second, mod_audio_empty.count(m.first) ? mod_audio_empty[m.first] : 0); }
    printf("disc: %u entradas y %u archivos desde %s, PACKFILE en el sector %u%s\n",
           (unsigned)disc_pk.size(), (unsigned)disc_files.size(), disc_dir.c_str(), disc_pk_lbn,
           disc_relocated ? ", recolocado por cambios de tamano" : "");
    if (opened) printf("disc: %u paks abiertos en carpetas, revisados en %.1f s%s\n", opened, (double)clock() / CLOCKS_PER_SEC - t0, disc_rescan ? " (listado completo)" : "");
    if (distinct > opened) printf("disc: %u entradas con tamano distinto al del manifiesto\n", distinct - opened);
    if (empties) printf("disc: %u entradas vacias sin archivo, %u de ellas por un wav sin muestras (tamano %u)\n", empties, emptied, empty_sz);
    if (built_cache < 0) { const char* e = getenv("TTT_BUILT_CACHE"); built_cache = e ? atoi(e) : 1; }
    const char* pc = getenv("TTT_PRELOAD");
    if (built_cache && (!pc || atoi(pc))) std::thread(preload).detach();
    return true;
}

struct DiscBusy { DiscBusy() { disc_reading++; } ~DiscBusy() { disc_reading--; } };

// read by absolute byte position in the virtual disc; gaps read as zero
static bool disc_read_at3 = false;
static int disc_read_entry = -1;
// TTT_DISC_ISO=Menus/,Main/system_es.pak (or *): those entries come from the ISO PACKFILE, for bisection
static std::vector<uint8_t> disc_from_iso;
static FILE* diso = nullptr;
static uint64_t diso_base = 0;

static void disc_iso_mark(uint32_t* distinct) {
    const char* q = getenv("TTT_DISC_ISO");
    if (!q || !*q) return;
    uint32_t lbn = 0, t = 0;
    char fpath[512];
    snprintf(fpath, sizeof(fpath), "%s/game.iso", GAME_DIR);
    if (!iso_buscar("disc0:/PSP_GAME/USRDIR/PACKFILE.BIN", &lbn, &t) || !(diso = fopen(fpath, "rb"))) { printf("disc: TTT_DISC_ISO sin ISO\n"); return; }
    diso_base = ((uint64_t)lbn + disc_pk_data) * DISC_SECTOR;
    std::vector<std::string> pref;
    std::string l = lower(q), p;
    for (char c : l + ",") { if (c == ',') { if (!p.empty()) pref.push_back(p); p.clear(); } else p += c; }
    disc_from_iso.assign(disc_pk.size(), 0);
    unsigned n = 0;
    for (size_t k = 0; k < disc_pk.size(); k++) {
        PkEntry& x = disc_pk[k];
        std::string r = lower(x.fpath);
        for (auto& e : pref)
            if (e == "*" || !r.compare(0, e.size(), e)) {
                disc_from_iso[k] = 1;
                if (x.sz != x.real) (*distinct)++;
                x.sz = x.size0 = x.real;
                x.folder = false;
                n++;
                break;
            }
    }
    printf("disc: %u entradas servidas desde el ISO (%s)\n", n, q);
}

// map pak read: lightning on when PCK1 entry 13 is not empty and MAPD slot 10 (ambient particles) is on (read by gpu_gl.h)
// water: MAPD slot 0 (+4 bit 0x02 water, +8 level, world Y points down) or slot 16 (immersion flag + RGBA, 0x80 = 1.0)
int psp_map_bolts = 0, psp_map_water = 0, psp_map_gfxw = 0, psp_map_wimm = 0;
float psp_map_wlevel = 0.0f;
// BT3 stage post effects kept in MAPD (same layout as BT3 StgDataA): +0x18 light (sun position A, direction B), +0x40 depth tints (air, water), +0x48 sky glare
float psp_map_sun[4] = {0, 0, 0, 0};
static const uint8_t HAZE_DYING_NAMEK[0x18] = {0x01, 0x00, 0x00, 0x00, 0x80, 0x80, 0x80, 0x80, 0x00, 0x14, 0xF9, 0x78, 0xFF, 0x14, 0x10, 0x28, 0xC8, 0xC8, 0x01, 0x00, 0xCD, 0xCC, 0xCC, 0x3D};   // Dying Namek MAPD +0xA0 StgHazeParam
int psp_map_haze[10] = {}; float psp_map_haze_follow = 0.1f, psp_map_top = 0.0f;   // StgHazeParam (MAPD +0xA0): flags, rgba, keys x3, cols, rows, amp x, amp y, period; [9] present
int psp_map_tint[2][6] = {}, psp_map_glare[7] = {}, psp_map_fog[5] = {};   // fog: flags, keys (x, z) x3, present
uint32_t psp_map_wcol = 0x80805C5Cu, psp_map_wsplash = 0;   // wsplash: water colour from the splash sprite (PCK1 entry 10), 0 = none
static void disc_map_check(int idx) {
    static int done = -1;
    if (idx == done) return;
    done = idx;
    psp_map_water = 0; psp_map_wimm = 0; psp_map_fx_tok = 0;
    std::vector<uint8_t> own, *v = &own;
    if (disc_pk[idx].folder) v = disc_build(idx);
    else if (FILE* f = open_file(disc_dir + "/Packfile/" + disc_pk[idx].fpath, "rb")) { own.resize(disc_pk[idx].sz); own.resize(fread(own.data(), 1, own.size(), f)); fclose(f); }
    const std::vector<uint8_t>& d = *v;
    auto be = [&](size_t o) { return o + 4 <= d.size() ? (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3] : 0u; };
    auto le = [&](size_t o) { uint32_t x = 0; if (o + 4 <= d.size()) memcpy(&x, &d[o], 4); return x; };
    uint32_t n = be(0);
    if (n < 14 || n > 64) return;
    psp_map_top = 0.0f;
    for (uint32_t i = 0; i < n; i++) {   // stage model block (magic 7E9A0B60): +0x54 = LIMITS {radius, top, bottom}
        uint32_t o = be(4 + 4 * i), lim = le(o + 0x54);
        if (le(o) != 0x7E9A0B60u || !lim || (size_t)o + lim + 8 > d.size()) continue;
        uint32_t u = le(o + lim + 4); memcpy(&psp_map_top, &u, 4);
        break;
    }
    int on = -1;
    uint32_t wfl = 0, imm = 0, icol = 0x80805C5Cu; float lvl = 0.0f;
    for (uint32_t i = 0; i < n && on < 0; i++) {
        uint32_t o = be(4 + 4 * i);
        if (le(o) != 0x4450414Du) continue;
        uint32_t sl = le(o + 0x10 + 8 * 10 + 4);
        on = (int32_t)sl > 0 ? (int)le(o + sl + 0x10) : 0;
        uint32_t s0 = le(o + 0x10 + 4), s16 = le(o + 0x10 + 8 * 16 + 4);
        if ((int32_t)s0 > 0) { wfl = le(o + s0 + 4); uint32_t u = le(o + s0 + 8); memcpy(&lvl, &u, 4); }
        if ((int32_t)s16 > 0) { imm = le(o + s16) & 0xFF; icol = le(o + s16 + 4); }
        memset(psp_map_sun, 0, sizeof(psp_map_sun)); memset(psp_map_tint, 0, sizeof(psp_map_tint)); memset(psp_map_glare, 0, sizeof(psp_map_glare)); memset(psp_map_fog, 0, sizeof(psp_map_fog)); memset(psp_map_haze, 0, sizeof(psp_map_haze));
        uint32_t sl1 = le(o + 0x18 + 4), n6 = le(o + 0x40), s6 = le(o + 0x40 + 4), n7 = le(o + 0x48), s7 = le(o + 0x48 + 4);
        if ((int32_t)sl1 > 0 && le(o + 0x18) >= 1) { for (int k = 0; k < 3; k++) { uint32_t u = le(o + sl1 + 4 * k); memcpy(&psp_map_sun[k], &u, 4); } psp_map_sun[3] = 1; }
        for (uint32_t k = 0; (int32_t)s6 > 0 && k < n6 && k < 2; k++) {   // flags, rgba, curve keys (x, z) x3
            size_t q = o + s6 + 16 * k;
            if (q + 14 > d.size()) break;
            psp_map_tint[k][0] = (int)le(q); psp_map_tint[k][1] = (int)le(q + 4);
            for (int j = 0; j < 3; j++) psp_map_tint[k][2 + j] = d[q + 8 + 2 * j] | d[q + 9 + 2 * j] << 8;
            psp_map_tint[k][5] = 1;
        }
        if ((int32_t)s7 > 0 && n7 >= 1 && o + s7 + 12 <= d.size()) {   // flags, step, max, hold, min, alpha
            psp_map_glare[0] = (int)le(o + s7); psp_map_glare[1] = (int)le(o + s7 + 4);
            for (int j = 0; j < 4; j++) psp_map_glare[2 + j] = d[o + s7 + 8 + j];
            psp_map_glare[6] = 1;
        }
        uint32_t n8 = le(o + 0x50), s8 = le(o + 0x50 + 4);
        if ((int32_t)s8 > 0 && n8 >= 1 && o + s8 + 10 <= d.size()) {   // StgFog depth blur: flags, keys (x, z) x3
            psp_map_fog[0] = (int)le(o + s8);
            for (int j = 0; j < 3; j++) psp_map_fog[1 + j] = d[o + s8 + 4 + 2 * j] | d[o + s8 + 5 + 2 * j] << 8;
            psp_map_fog[4] = 1;
        }
        uint32_t nA = le(o + 0xA0), sA = le(o + 0xA4);
        if ((int32_t)sA > 0 && nA >= 1 && o + sA + 0x18 <= d.size() && (le(o + sA) & 1)) {   // StgHaze_SetParams (entry 0 only)
            size_t q = o + sA;
            psp_map_haze[0] = (int)le(q); psp_map_haze[1] = (int)le(q + 4);
            for (int j = 0; j < 3; j++) psp_map_haze[2 + j] = d[q + 8 + 2 * j] | d[q + 9 + 2 * j] << 8;
            for (int j = 0; j < 4; j++) psp_map_haze[5 + j] = d[q + 0xE + j];
            psp_map_haze[9] = d[q + 0x12] | 1 << 8;
            uint32_t u = le(q + 0x14); memcpy(&psp_map_haze_follow, &u, 4);
            printf("atm: calima del mapa: rgba %08X curva %d/%d %d/%d %d/%d, rejilla %dx%d, amplitud %d/%d, periodo %d, seguimiento %.3f, techo %.1f\n", (unsigned)psp_map_haze[1],
                   psp_map_haze[2] & 255, psp_map_haze[2] >> 8, psp_map_haze[3] & 255, psp_map_haze[3] >> 8, psp_map_haze[4] & 255, psp_map_haze[4] >> 8,
                   psp_map_haze[5], psp_map_haze[6], psp_map_haze[7], psp_map_haze[8], psp_map_haze[9] & 255, psp_map_haze_follow, psp_map_top);
        }
        { auto it = ttt_fx_tok.find(disc_pk[idx].fpath); psp_map_fx_tok = it == ttt_fx_tok.end() ? 0 : (int)it->second; printf("mapas: %s cargado, efectos del mod en mapa TTT: %s\n", disc_pk[idx].fpath.c_str(), psp_map_fx_tok ? "si" : "no"); }
        {   // haze switch of the mod's <map>.fx.txt: 0 off, 1 = this map's own or Dying Namek's StgHazeParam
            MapFx mf = map_fx(disc_dir + "/Packfile/" + disc_pk[idx].fpath);
            if (mf.haze == 0 && psp_map_haze[9]) { memset(psp_map_haze, 0, sizeof(psp_map_haze)); printf("atm: calima apagada por el mod (%s)\n", mf.txt.c_str()); }
            else if (mf.haze == 1 && !psp_map_haze[9]) {
                const uint8_t* q = HAZE_DYING_NAMEK;
                psp_map_haze[0] = (int)(q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24); psp_map_haze[1] = (int)(q[4] | q[5] << 8 | q[6] << 16 | (uint32_t)q[7] << 24);
                for (int j = 0; j < 3; j++) psp_map_haze[2 + j] = q[8 + 2 * j] | q[9 + 2 * j] << 8;
                for (int j = 0; j < 4; j++) psp_map_haze[5 + j] = q[0xE + j];
                psp_map_haze[9] = q[0x12] | 1 << 8;
                uint32_t u = q[0x14] | q[0x15] << 8 | q[0x16] << 16 | (uint32_t)q[0x17] << 24; memcpy(&psp_map_haze_follow, &u, 4);
                printf("atm: calima de Namek destruido encendida por el mod (%s), techo %.1f%s\n", mf.txt.c_str(), psp_map_top, psp_map_top < -1.0f ? "" : " (sin techo: fuerza completa)");
            }
        }
        printf("atm: mapa %s, sol (%.0f %.0f %.0f), tinte aire flags %d rgba %08X curva %d/%d %d/%d %d/%d, tinte agua flags %d, destello flags %d paso %d max %d reposo %d min %d alfa %d, desenfoque flags %d curva %d/%d %d/%d %d/%d\n",
               disc_pk[idx].fpath.c_str(), psp_map_sun[0], psp_map_sun[1], psp_map_sun[2], psp_map_tint[0][0], (unsigned)psp_map_tint[0][1],
               psp_map_tint[0][2] & 255, psp_map_tint[0][2] >> 8, psp_map_tint[0][3] & 255, psp_map_tint[0][3] >> 8, psp_map_tint[0][4] & 255, psp_map_tint[0][4] >> 8,
               psp_map_tint[1][0], psp_map_glare[0], psp_map_glare[1], psp_map_glare[2], psp_map_glare[3], psp_map_glare[4], psp_map_glare[5],
               psp_map_fog[4] ? psp_map_fog[0] : -1, psp_map_fog[1] & 255, psp_map_fog[1] >> 8, psp_map_fog[2] & 255, psp_map_fog[2] >> 8, psp_map_fog[3] & 255, psp_map_fog[3] >> 8);
    }
    if (on < 0) return;
    psp_map_water = (wfl & 2) || imm == 1;
    psp_map_wimm = imm == 1;   // water a fighter can sink in (MAPD +0x90 bit 0); ambient sea only = 0
    psp_map_gfxw = (wfl & 2) && imm == 1;   // BT3 GfxWater: stage water (env flag 2) and the MAPD +0x90 record bit 0
    psp_map_wsplash = 0;
    bool bt3s;
    { std::lock_guard<std::mutex> g(bt3_stage_mx); bt3s = bt3_stage_of.count(disc_pk[idx].fpath) > 0; }
    if (bt3s && imm == 1) { psp_map_wsplash = 0xFF000000u | (icol & 0xFFFFFFu); printf("agua: color del agua del escenario BT3 (registro de tinte): (%u %u %u)\n", icol & 255, (icol >> 8) & 255, (icol >> 16) & 255); }
    else if (!bt3s) {   // entry 10 (single-texture ATEX): CLUT colours weighted by how many pixels use them, by alpha and by saturation (the white highlights barely count)
        uint32_t o = be(4 + 4 * 10), e = be(4 + 4 * 11), C = le(o + 0x28), D = le(o + 0x2C);
        int fmt = o + 0x4C <= d.size() ? d[o + 0x4B] : 0, nc = fmt == 8 ? 16 : fmt == 9 ? 256 : 0;
        if (e > o && nc && 0x80ull + C + D <= e - o && D >= (uint32_t)nc * 4 && o + 0x80 + C + D <= d.size()) {
            std::vector<double> use(nc, 0.0);
            for (uint32_t i = 0; i < C; i++) { uint8_t b = d[o + 0x80 + i]; if (nc == 16) { use[b & 15] += 1; use[b >> 4] += 1; } else use[b] += 1; }
            const uint8_t* cl = &d[o + 0x80 + C];
            int amax = 1; for (int i = 0; i < nc; i++) amax = std::max(amax, (int)cl[4 * i + 3]);
            double s[3] = {0, 0, 0}, w = 0;
            for (int i = 0; i < nc; i++) {
                double r = cl[4 * i], g = cl[4 * i + 1], b = cl[4 * i + 2], mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
                double k = use[i] * cl[4 * i + 3] / amax * ((mx > 0 ? (mx - mn) / mx : 0) + 0.05);
                s[0] += r * k; s[1] += g * k; s[2] += b * k; w += k;
            }
            if (w > 0) {
                int r = (int)(s[0] / w + 0.5), g = (int)(s[1] / w + 0.5), b = (int)(s[2] / w + 0.5);
                psp_map_wsplash = 0xFF000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
                printf("agua: color de la salpicadura (entrada 10, %dx%d, %d colores): (%d %d %d)\n", 1 << d[o + 0x49], 1 << d[o + 0x48], nc, r, g, b);
            }
        }
    }
    psp_map_wlevel = lvl == lvl ? lvl : 0.0f;
    psp_map_wcol = imm == 1 ? icol : 0x80805C5Cu;
    printf("agua: mapa %s, nivel %.1f, bits 0x%02X, inmersion %s, color (%u %u %u %u): agua %s\n", disc_pk[idx].fpath.c_str(), psp_map_wlevel, wfl & 0xFF,
           imm == 1 ? "si" : "no", icol & 255, (icol >> 8) & 255, (icol >> 16) & 255, icol >> 24, psp_map_water ? "si" : "no");
    uint32_t e13 = be(4 + 4 * 14) - be(4 + 4 * 13);
    psp_map_bolts = e13 > 0 && on == 1;
    printf("cel: mapa %s, entrada 13 %u bytes, particulas de ambiente %s: rayos %s\n", disc_pk[idx].fpath.c_str(), e13, on == 1 ? "encendidas" : "apagadas", psp_map_bolts ? "si" : "no");
}

static uint32_t disc_read(uint64_t pos, uint8_t* dst, uint32_t n) {
    DiscBusy busy;
    int map_idx = -1;
    disc_read_at3 = false;
    disc_last_read = std::chrono::steady_clock::now().time_since_epoch().count();
    memset(dst, 0, n);
    uint64_t end_pos = pos + n;
    size_t k0 = (size_t)(std::lower_bound(disc_spans.begin(), disc_spans.end(), (uint32_t)(pos / DISC_SECTOR),
                                          [](const DiscSpan& t, uint32_t s) { return t.lbn < s; }) - disc_spans.begin());
    while (k0 && !disc_spans[--k0].sectors) {}   // empty entries share their sector with the next one
    for (size_t k = k0; k < disc_spans.size(); k++) {
        const DiscSpan& t = disc_spans[k];
        uint64_t start = (uint64_t)t.lbn * DISC_SECTOR;
        if (start >= end_pos) break;
        uint64_t tope = start + t.sz;
        if (tope <= pos) continue;
        uint64_t a = std::max(pos, start), b = std::min(end_pos, tope);
        if (a >= b) continue;
        if (t.type == 2 && (((size_t)t.idx == cr_map_index) || (!strncasecmp(disc_pk[t.idx].fpath.c_str(), "Maps/", 5) && strncasecmp(disc_pk[t.idx].fpath.c_str(), "Maps/SFX/", 9)))) {
            map_idx = t.idx;
            if (a == start) map_sfx_ahead(t.idx);
        }
        uint8_t* d = dst + (a - pos);
        uint32_t amount = (uint32_t)(b - a);
        if (t.type == 2 && a == start) { bt3_pak_read((uint32_t)(d - psp_mem), t.idx); }
        const std::string* vr = t.type == 2 ? voice_raw_file((size_t)t.idx) : nullptr;
        if (t.type == 1) {
            memcpy(d, disc_index.data() + (a - start), std::min<uint64_t>(amount, disc_index.size() - (a - start)));
        } else if (vr) {
            disc_read_entry = t.idx;
            if (FILE* f = open_file(*vr, "rb")) { fseek(f, (long)(a - start), SEEK_SET); size_t got = fread(d, 1, amount, f); (void)got; fclose(f); }
        } else if (t.type == 2 && diso && disc_from_iso[t.idx]) {
            disc_read_entry = t.idx;
#ifdef _WIN32
            _fseeki64(diso, (long long)(diso_base + (uint64_t)disc_pk[t.idx].sector0 * DISC_SECTOR + (a - start)), SEEK_SET);
#else
            fseeko(diso, (off_t)(diso_base + (uint64_t)disc_pk[t.idx].sector0 * DISC_SECTOR + (a - start)), SEEK_SET);
#endif
            if (fread(d, 1, amount, diso) != amount) printf("disc: lectura corta del ISO en %s\n", disc_pk[t.idx].fpath.c_str());
        } else if (t.type == 2 && disc_pk[t.idx].folder) {
            disc_read_entry = t.idx;
            const Container* c3 = container_of(disc_pk[t.idx].key);
            if (c3 && c3->type == 5) { disc_read_at3 = true; at3_range(*c3, (uint32_t)(a - start), d, amount); continue; }
            if (a == start) lip_read((uint32_t)(d - psp_mem), t.idx);
            if (a == start && amount == t.sz) { disc_build_into(t.idx, d); continue; }
            std::vector<uint8_t>* v = disc_build(t.idx);
            memcpy(d, v->data() + (a - start), amount);
        } else {
            if (t.type == 2) { disc_read_entry = t.idx; if (a == start) lip_read((uint32_t)(d - psp_mem), t.idx); }
            FILE* f = t.type == 2 ? disc_open_entry(t.idx) : fopen(disc_files[t.idx].local.c_str(), "rb");
            if (!f) continue;
            fseek(f, (long)(a - start), SEEK_SET);
            if (fread(d, 1, amount, f) != amount) printf("disc: lectura corta en el tramo tipo %d idx %d\n", t.type, t.idx);
            if (t.type == 2) closed_hud_mark(t.idx, (uint32_t)(a - start), d, amount);
            if (t.type == 0) fclose(f);
        }
    }
    if (map_idx >= 0) disc_map_check(map_idx);
    return n;
}

static bool disc_find(const char* fpath, uint32_t* lbn, uint32_t* sz) {
    if (!disc_ready) return false;
    const char* q = strchr(fpath, ':');
    q = q ? q + 1 : fpath;
    while (*q == '/') q++;
    for (auto& a : disc_files)
        if (!strcasecmp(a.fpath.c_str(), q)) { *lbn = a.lbn; *sz = a.sz; return true; }
    if (strstr(q, "PACKFILE.BIN") || strstr(q, "packfile.bin")) { *lbn = disc_pk_lbn; *sz = disc_pk_size ? disc_pk_size : disc_pk_sectors * DISC_SECTOR; return true; }
    return false;
}

// check against the ISO: compares entry by entry with the original PACKFILE
static uint64_t v_equal = 0, v_data = 0, v_reads = 0;
static FILE* v_iso = nullptr;
static uint32_t v_iso_lbn = 0;
static std::set<uint64_t> v_seen;
static int v_warnings = 0;

static void v_find(FILE* f, uint64_t pos) {
#ifdef _WIN32
    _fseeki64(f, (long long)pos, SEEK_SET);
#else
    fseeko(f, (off_t)pos, SEEK_SET);
#endif
}

static void v_hex(char* s, const uint8_t* d, uint32_t n) {
    uint32_t m = n < 16 ? n : 16;
    for (uint32_t k = 0; k < m; k++) sprintf(s + k * 3, "%02x ", d[k]);
    s[m * 3] = 0;
}

// inside an open pak, tells whether the byte falls in the table or which part
static void v_place(const PkEntry& x, uint32_t off, char* s, size_t n) {
    if (!x.folder) { snprintf(s, n, "archivo suelto"); return; }
    const Container* pc = container_of(x.key);
    if (!pc) { snprintf(s, n, "contenedor desconocido"); return; }
    const Container& c = *pc;
    uint32_t parts = (uint32_t)c.parts.size();
    if (c.type == 2) {
        if (off < TXT1_HDR + 4 * parts) { snprintf(s, n, "cabecera o tabla del txt1"); return; }
        uint32_t pos = TXT1_HDR + 4 * parts;
        for (uint32_t k = 0; k < parts; k++) {
            if (off < pos + c.parts[k].sz) { snprintf(s, n, "cadena %u/%u %s byte %u de %u", k + 1, parts, c.parts[k].fpath.c_str(), off - pos, c.parts[k].sz); return; }
            pos += c.parts[k].sz;
        }
        snprintf(s, n, "cola del txt1"); return;
    }
    uint32_t start_at = (4 + 4 * (parts + 1) + ALIN_PAK - 1) / ALIN_PAK * ALIN_PAK;
    if (off < start_at) { snprintf(s, n, "tabla de desplazamientos, indice %u", off < 4 ? 0 : (off - 4) / 4); return; }
    uint32_t pos = start_at;
    for (uint32_t k = 0; k < parts; k++) {
        uint32_t next = (pos + c.parts[k].sz + ALIGN_ENTRY - 1) / ALIGN_ENTRY * ALIGN_ENTRY;
        if (off < next) { snprintf(s, n, "parte %u/%u %s byte %u de %u", k + 1, parts, c.parts[k].fpath.c_str(), off - pos, c.parts[k].sz); return; }
        pos = next;
    }
    snprintf(s, n, "cola, byte %u tras el cuerpo", off - pos);
}

// container part where a byte falls: index, type and part end (relative to the entry)
static void v_locate(const PkEntry& x, uint32_t off, int* part, int* type, uint32_t* end_pos) {
    *part = -1; *type = 0; *end_pos = x.size0;
    const Container* pc = x.folder ? container_of(x.key) : nullptr;
    if (!pc || pc->type == 2) return;
    const Container& c = *pc;
    uint32_t parts = (uint32_t)c.parts.size();
    uint32_t pos = (4 + 4 * (parts + 1) + ALIN_PAK - 1) / ALIN_PAK * ALIN_PAK;
    if (off < pos) { *end_pos = pos; return; }
    for (uint32_t k = 0; k < parts; k++) {
        uint32_t next = (pos + c.parts[k].sz + ALIGN_ENTRY - 1) / ALIGN_ENTRY * ALIGN_ENTRY;
        if (off < next) { *part = (int)k; *type = c.parts[k].type; *end_pos = next; return; }
        pos = next;
    }
}

static void disc_verify(uint64_t pos, const uint8_t* dst, uint32_t n) {
    if (!disc_verify_mode || !disc_ready) return;
    if (!v_iso) {
        uint32_t t = 0;
        if (!iso_buscar("disc0:/PSP_GAME/USRDIR/PACKFILE.BIN", &v_iso_lbn, &t)) { printf("disc: no hay ISO para comparar\n"); disc_verify_mode = 0; return; }
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/game.iso", GAME_DIR);
        v_iso = fopen(fpath, "rb");
        if (!v_iso) { printf("disc: no puedo abrir el ISO para comparar\n"); disc_verify_mode = 0; return; }
        printf("disc: comparando entradas contra el PACKFILE del ISO en el sector %u\n", v_iso_lbn);
    }
    uint64_t end_pos = pos + n;
    v_reads++;
    std::vector<uint8_t> orig;
    size_t k0 = (size_t)(std::lower_bound(disc_spans.begin(), disc_spans.end(), (uint32_t)(pos / DISC_SECTOR),
                                          [](const DiscSpan& t, uint32_t s) { return t.lbn < s; }) - disc_spans.begin());
    while (k0 && !disc_spans[--k0].sectors) {}   // empty entries share their sector with the next one
    for (size_t j = k0; j < disc_spans.size(); j++) {
        const DiscSpan& t = disc_spans[j];
        uint64_t start = (uint64_t)t.lbn * DISC_SECTOR;
        if (start >= end_pos) break;
        if (t.type != 2) continue;
        if (start + t.sz <= pos) continue;
        uint64_t a = std::max(pos, start), b = std::min(end_pos, start + t.sz);
        if (a >= b) continue;
        PkEntry& x = disc_pk[t.idx];
        uint32_t off = (uint32_t)(a - start), amount = (uint32_t)(b - a);
        if (off >= x.size0) continue;
        if (disc_verify_mode < 2 && !x.folder && ends_with(x.fpath, ".unk")) continue;
        if (off + amount > x.size0) amount = x.size0 - off;
        orig.resize(amount);
        v_find(v_iso, (uint64_t)(v_iso_lbn + disc_pk_data + x.sector0) * DISC_SECTOR + off);
        if (fread(orig.data(), 1, amount, v_iso) != amount) continue;
        const uint8_t* d = dst + (a - pos);
        uint32_t k = 0;
        while (k < amount) {
            uint32_t k0 = k;
            while (k < amount && orig[k] == d[k]) k++;
            v_equal += k - k0;
            if (k == amount) break;
            int part, type; uint32_t part_end;
            v_locate(x, off + k, &part, &type, &part_end);
            uint32_t rest = part_end > off + k ? part_end - (off + k) : 16;
            // expected differences: the whole vagp blob (markers and zeros) and the texture markers
            if (type == 3 || (disc_verify_mode < 2 && type == 4)) { k += rest; continue; }
            if (k + 16 <= amount && !memcmp(d + k, TEX_MARKER, 8)) { k += 16; continue; }
            v_data++;
            uint64_t key = ((uint64_t)t.idx << 32) | (uint32_t)(part + 1);
            if (v_seen.insert(key).second && v_warnings++ < 200) {
                char sitio[320], h1[64], h2[64];
                v_place(x, off + k, sitio, sizeof(sitio));
                v_hex(h1, orig.data() + k, amount - k);
                v_hex(h2, d + k, amount - k);
                printf("disc: DIFIERE %s en el byte %u de %u (%s)\n  iso:   %s\n  local: %s\n",
                       x.fpath.c_str(), off + k, x.size0, sitio, h1, h2);
            }
            k += rest;
        }
    }
    if (v_reads % 500 == 0)
        printf("disc: %llu lecturas, %llu bytes iguales, %llu tramos distintos, %d entradas malas\n",
               (unsigned long long)v_reads, (unsigned long long)v_equal,
               (unsigned long long)v_data, v_warnings);
}

#include "disc/reload.h"
#include "disc/renders.h"
#include "disc/text_names.h"

// settings overlay (ui/overlay.h): a texture of a game pak (found by its file name) as RGBA, and text with the app's fonts.
// Entry numbers come from the menu editor (counted from 1): entry - 1 is tried first, then the number itself and the next one,
// and the first RHG whose texture holds the rect wins
bool psp_ui_piece(const char* name, int ent, int tex, int rw, int rh, int* w, int* h, std::vector<uint32_t>* px) {
    static std::map<std::string, std::vector<uint8_t>> paks;
    if (!disc_ready) return false;
    auto it = paks.find(name);
    if (it == paks.end()) {
        std::vector<uint8_t> own;
        std::string want = lower(name);
        int pk = -1;
        for (size_t k = 0; k < disc_pk.size() && pk < 0; k++) { std::string l = lower(disc_pk[k].fpath); if (l.size() >= want.size() && !l.compare(l.size() - want.size(), want.size(), want) && (l.size() == want.size() || l[l.size() - want.size() - 1] == '/')) pk = (int)k; }
        if (pk >= 0) {
            if (disc_pk[pk].folder) { std::vector<uint8_t>* v = disc_build(pk); if (v) own = *v; }
            else read_file(disc_dir + "/Packfile/" + disc_pk[pk].fpath, own);
        }
        it = paks.emplace(name, std::move(own)).first;
        printf("ajustes: pak %s %s, %u bytes\n", name, pk >= 0 ? disc_pk[pk].fpath.c_str() : "no encontrado", (unsigned)it->second.size());
    }
    const std::vector<uint8_t>& d = it->second;
    auto be = [&](size_t o) { return o + 4 <= d.size() ? (uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3] : 0u; };
    uint32_t cnt = be(0);
    if (!cnt || cnt > 4096) return false;
    for (int e : {ent - 1, ent, ent + 1}) {
        if (e < 0 || (uint32_t)e >= cnt) continue;
        uint32_t a = be(4 + 4 * e), b = (uint32_t)e + 1 < cnt ? be(8 + 4 * e) : be(4 + 4 * cnt);
        if (!b || b > d.size()) b = (uint32_t)d.size();
        if (a + 4 > b || memcmp(&d[a], "RHG\0", 4)) continue;
        std::vector<std::pair<uint32_t, std::vector<uint32_t>>> tx;
        std::vector<std::pair<int, int>> sz;
        if (!rhg_decode(&d[a], b - a, tx, sz) || tex >= (int)tx.size() || sz[tex].first < rw || sz[tex].second < rh) continue;
        *w = sz[tex].first; *h = sz[tex].second;
        *px = std::move(tx[tex].second);
        // an exposed texture: its first 16 data bytes are the png marker, the image is the registered png
        size_t mo = (size_t)a + tx[tex].first;
        const char* src = "datos del pak";
        if (mo + 16 <= b && !memcmp(&d[mo], TEX_MARKER, 8)) {
            uint32_t id, chk;
            memcpy(&id, &d[mo + 8], 4); memcpy(&chk, &d[mo + 12], 4);
            TexPng* t = chk == ~id ? tex_load(id) : nullptr;
            if (t && t->w > 0 && t->h > 0 && t->w <= *w && t->h <= *h && t->px.size() >= (size_t)t->w * t->h) {
                for (int y = 0; y < t->h; y++) memcpy(&(*px)[(size_t)y * *w], &t->px[(size_t)y * t->w], (size_t)t->w * 4);
                src = "png";
            }
            else {
                int bad = std::min(*w, 32);
                if (*h > 1) for (int x = 0; x < bad; x++) (*px)[x] = (*px)[(size_t)*w + x];
                src = "datos del pak, marca tapada";
                printf("ajustes: png de la textura %u no usable (%dx%d)\n", id, t ? t->w : 0, t ? t->h : 0);
            }
        }
        printf("ajustes: pieza de %s entrada %d -> %d, textura %d %dx%d (%s)\n", name, ent, e, tex, *w, *h, src);
        return true;
    }
    printf("ajustes: %s sin RHG con la textura %d de %dx%d cerca de la entrada %d\n", name, tex, rw, rh, ent);
    return false;
}

static const ft::Font& ui_font(int style, float size) {
    static std::map<int, ft::Font> fonts;
    int key = (style & 1) * 1000 + (int)lroundf(size * 100.0f);
    auto it = fonts.find(key);
    if (it == fonts.end()) { it = fonts.emplace(key, ft::load(style & 1 ? "../../games/ttt/fonts/map_name" : "../../games/ttt/fonts/sz_name")).first; it->second.size = size; }
    return it->second;
}
// width of a text in game pixels (pen advance, kerning included)
float psp_ui_text_w(int style, float size, const std::string& s) {
    const ft::Font& f = ui_font(style, size);
    if (!f.ok || f.scale <= 0) return 0;
    std::vector<uint32_t> cp = ft::utf32(s);
    float x = 0;
    for (size_t i = 0; i < cp.size(); i++) {
        auto it = f.g.find(cp[i]);
        if (it == f.g.end()) it = f.g.find('?');
        if (it == f.g.end()) continue;
        if (i) { auto k = f.kern.find((uint64_t)cp[i - 1] << 32 | cp[i]); if (k != f.kern.end()) x += k->second; }
        x += it->second.xa;
    }
    return x / f.scale * (f.size > 0 ? f.size : 1.0f);
}

bool psp_ui_text(int style, float size, const std::string& s, int cw, int ch, int q, int align, int* w, int* h, std::vector<uint32_t>* px) {
    const ft::Font& fn = ui_font(style, size);
    if (!fn.ok) return false;
    ft::Img im = ft::compose(fn, s, cw, ch, q, align);
    if (!im.w) return false;
    *w = im.w; *h = im.h; *px = std::move(im.px);
    return true;
}
