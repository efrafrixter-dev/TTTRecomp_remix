#pragma once
#include "psp.h"
#include "audio/atrac3.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

// Sound: sceSas mixer (ADPCM VAG, PCM, noise, ADSR envelopes) and waveOut output.
// sceAtrac decodes with the ATRAC3 port in atrac3.h.

#define SAS_VOICES 32
#define SAS_ENV_MAX 0x40000000
#define SAS_VOL_MAX 0x1000
#define SAS_PITCH 0x1000
#define SND_NBUF 12
#define SND_RING 32768

static int snd_cfg_done = 0;
static int snd_enabled = 1, snd_tone = 0, snd_log = 0, snd_sas = 1, snd_env = 1;
static int snd_block = 512, snd_queue = 6, snd_vol = 100, snd_rate_forced = 0;
static int snd_rate = 44100;
static int sfx_loop = 1;
// sfx stereo: 0 original (PSP level panning), 1 3D (interaural delay and head shadow), 2 wide 3D
int snd_3d = 0;

static void snd_cfg() {
    if (snd_cfg_done) return;
    snd_cfg_done = 1;
    const char* e;
    if ((e = getenv("TTT_SOUND"))) snd_enabled = atoi(e);
    if ((e = getenv("TTT_SOUND_TONE"))) snd_tone = atoi(e);
    if ((e = getenv("TTT_SOUND_LOG"))) snd_log = atoi(e);
    if ((e = getenv("TTT_SAS"))) snd_sas = atoi(e);
    if ((e = getenv("TTT_SAS_ADSR"))) snd_env = atoi(e);
    if ((e = getenv("TTT_SFX_LOOP"))) sfx_loop = atoi(e);
    if ((e = getenv("TTT_SOUND_HZ"))) snd_rate_forced = atoi(e);
    if ((e = getenv("TTT_SOUND_BLOCK"))) snd_block = atoi(e);
    if ((e = getenv("TTT_SOUND_QUEUE"))) snd_queue = atoi(e);
    if ((e = getenv("TTT_SOUND_VOL"))) snd_vol = atoi(e);
    if ((e = getenv("TTT_SFX_3D"))) snd_3d = atoi(e);
    if (snd_block < 64) snd_block = 64;
    if (snd_block > 4096) snd_block = 4096;
    if (snd_queue < 2) snd_queue = 2;
    if (snd_queue > SND_NBUF - 2) snd_queue = SND_NBUF - 2;
    if (snd_rate_forced) snd_rate = snd_rate_forced;
}

// ---------------- device ----------------

static long long snd_sent_blocks = 0, snd_overflows = 0, snd_starved = 0;

#ifdef _WIN32
static HWAVEOUT snd_dev = nullptr;
static WAVEHDR snd_hdr[SND_NBUF];
static int16_t* snd_mem[SND_NBUF];
static int snd_next = 0;

static int snd_pending() {
    int n = 0;
    for (int i = 0; i < SND_NBUF; i++) if (!(snd_hdr[i].dwFlags & WHDR_DONE)) n++;
    return n;
}

static long long snd_played();

static int snd_open() {
    if (snd_dev) return 1;
    WAVEFORMATEX f;
    memset(&f, 0, sizeof(f));
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = (DWORD)snd_rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = (DWORD)snd_rate * 4;
    if (waveOutOpen(&snd_dev, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        snd_dev = nullptr;
        snd_enabled = 0;
        printf("sound: waveOut no abre a %d Hz, audio desactivado\n", snd_rate);
        fflush(stdout);
        return 0;
    }
    for (int i = 0; i < SND_NBUF; i++) {
        snd_mem[i] = (int16_t*)calloc((size_t)snd_block * 2, 2);
        memset(&snd_hdr[i], 0, sizeof(WAVEHDR));
        snd_hdr[i].lpData = (char*)snd_mem[i];
        snd_hdr[i].dwBufferLength = (DWORD)snd_block * 4;
        waveOutPrepareHeader(snd_dev, &snd_hdr[i], sizeof(WAVEHDR));
        snd_hdr[i].dwFlags |= WHDR_DONE;
    }
    printf("sound: waveOut abierto a %d Hz, bloque %d muestras, cola %d\n", snd_rate, snd_block, snd_queue);
    fflush(stdout);
    return 1;
}

static long long snd_played() {
    if (!snd_dev) return 0;
    MMTIME t;
    memset(&t, 0, sizeof(t));
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(snd_dev, &t, sizeof(t)) != MMSYSERR_NOERROR || t.wType != TIME_SAMPLES) return 0;
    return (long long)(uint32_t)t.u.sample;
}

static int snd_send(const int16_t* pcm) {
    for (int k = 0; k < SND_NBUF; k++) {
        int i = (snd_next + k) % SND_NBUF;
        if (snd_hdr[i].dwFlags & WHDR_DONE) {
            memcpy(snd_mem[i], pcm, (size_t)snd_block * 4);
            snd_hdr[i].dwFlags &= ~(DWORD)WHDR_DONE;
            snd_hdr[i].dwBufferLength = (DWORD)snd_block * 4;
            waveOutWrite(snd_dev, &snd_hdr[i], sizeof(WAVEHDR));
            snd_next = (i + 1) % SND_NBUF;
            return 1;
        }
    }
    return 0;
}
#else
static int snd_pending() { return 0; }
static long long snd_played() { return 0; }
static int snd_open() { snd_enabled = 0; return 0; }
static int snd_send(const int16_t*) { return 0; }
#endif

// ---------------- mix ring ----------------

static int32_t snd_ring[SND_RING * 2];
static long long snd_dev_pos = 0;
static long long snd_ch_pos[16];
static int snd_ch_live[16];
static double snd_phase = 0;

static long long snd_limit() {
    long long l = -1;
    for (int c = 0; c < 16; c++)
        if (snd_ch_live[c] && (l < 0 || snd_ch_pos[c] < l)) l = snd_ch_pos[c];
    return l < 0 ? snd_dev_pos : l;
}

static long long snd_lag() {
    long long a = snd_limit() - snd_played();
    return a < 0 ? 0 : a;
}

static void snd_pump() {
    if (!snd_enabled) return;
    static int16_t tmp[4096 * 2];
    long long lim = snd_limit();
    while (lim - snd_dev_pos >= snd_block && snd_pending() < SND_NBUF) {
        for (int i = 0; i < snd_block; i++) {
            size_t k = (size_t)((snd_dev_pos + i) & (SND_RING - 1)) * 2;
            int l = snd_ring[k], r = snd_ring[k + 1];
            snd_ring[k] = 0; snd_ring[k + 1] = 0;
            if (snd_tone) {
                l = r = (int)(8000.0 * sin(snd_phase));
                snd_phase += 6.283185307 * 440.0 / snd_rate;
            }
            if (snd_vol != 100) { l = l * snd_vol / 100; r = r * snd_vol / 100; }
            tmp[i * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            tmp[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        if (!snd_send(tmp)) break;
        snd_dev_pos += snd_block;
        snd_sent_blocks++;
    }
    if (snd_dev_pos > 0 && snd_played() >= snd_dev_pos) snd_starved++;
}

// channel 8 = sceAudioOutput2, 0..7 = normal channels
static void snd_put(int channel, uint32_t addr, int frames, int vl, int vr, int mono) {
    snd_cfg();
    if (!snd_enabled || !snd_open()) return;
    if (channel < 0 || channel > 15) channel = 15;
    snd_ch_live[channel] = 1;
    long long p = snd_ch_pos[channel];
    if (p < snd_dev_pos) p = snd_dev_pos;
    if (p - snd_dev_pos > SND_RING - frames - 64) { p = snd_dev_pos; snd_overflows++; }
    const int16_t* s = (const int16_t*)psp_p(addr);
    for (int i = 0; i < frames; i++) {
        size_t k = (size_t)((p + i) & (SND_RING - 1)) * 2;
        int a = mono ? s[i] : s[i * 2];
        int b = mono ? s[i] : s[i * 2 + 1];
        snd_ring[k] += (a * vl) >> 15;
        snd_ring[k + 1] += (b * vr) >> 15;
    }
    snd_ch_pos[channel] = p + frames;
    snd_pump();
}

static void snd_pause(int frames) {
    uint64_t base = (uint64_t)frames * 1000000ull / (uint64_t)snd_rate;
    if (snd_enabled) {
        double target = (double)snd_queue * snd_block;
        double f = 1.0 + 0.5 * ((double)snd_lag() - target) / target;
        if (f < 0.5) f = 0.5;
        if (f > 1.6) f = 1.6;
        base = (uint64_t)(base * f);
    }
    psp_block(psp_time_us() + base);
}

// ---------------- sceSas mixer ----------------

struct SasVoice {
    int type, on, paused, end, jump, last;
    uint32_t addr, size;
    int loop, loop_start, block, idx, ppos;
    int s1, s2, prev, act;
    unsigned frac, lfsr;
    int pitch, step, vl, vr, fxl, fxr, noise;
    int wav;
    int16_t pcm[28];
    int64_t height, sus_level;
    int env_state, ra, rd, rs, rr, ma, md, ms, mr;
    int echo[64], echo_pos;
    float shade;
    int src_owner, src_ok;   // underwater filter: the battle sound queue entry that started it (owner slot, -1 = at a point)
    float src_y, uw_fc, uw_g, uw_s, uw_z[2];
};

struct SasCore {
    uint32_t addr;
    int used, grain, max_voices, mode, hz;
    SasVoice v[SAS_VOICES];
};

static SasCore sas_n[4];
static long long sas_calls = 0;

static SasCore* sas_find(uint32_t addr) {
    for (int i = 0; i < 4; i++) if (sas_n[i].used && sas_n[i].addr == addr) return &sas_n[i];
    for (int i = 0; i < 4; i++) if (!sas_n[i].used) {
        memset(&sas_n[i], 0, sizeof(SasCore));
        sas_n[i].used = 1; sas_n[i].addr = addr; sas_n[i].grain = 256; sas_n[i].max_voices = SAS_VOICES;
        for (int k = 0; k < SAS_VOICES; k++) {
            sas_n[i].v[k].pitch = sas_n[i].v[k].step = SAS_PITCH;
            sas_n[i].v[k].vl = sas_n[i].v[k].vr = SAS_VOL_MAX;
            sas_n[i].v[k].end = 1;
            sas_n[i].v[k].lfsr = 0x7654321;
            sas_n[i].v[k].sus_level = SAS_ENV_MAX / 2;
        }
        return &sas_n[i];
    }
    return &sas_n[0];
}

static void sas_step(SasVoice& v) {
    v.step = v.pitch;
    if (v.type != 4) return;
    SfxWav* w = sfx_wav(v.wav);
    if (w && w->freq && w->rate) v.step = (int)(((int64_t)v.pitch * w->rate) / (int)w->freq);
    if (v.step < 1) v.step = 1;
}

// a sound exposed as wav leaves a marker at the start of its data, searched where the game copied it
static void sas_detect_wav(SasVoice& v) {
    v.wav = 0;
    if (!v.addr || v.size < 16) return;
    uint32_t id = sfx_marker_id(psp_p(v.addr));
    if (!id) return;
    v.wav = (int)id;
    v.type = 4;
}

static int sfx_log_mode = -1;
static long sfx_log_n = 0;
#define SFX_LOG_MAX 12000

static int sfx_log_enabled() {
    if (sfx_log_mode < 0) { const char* e = getenv("TTT_SFX_LOG"); sfx_log_mode = e ? atoi(e) : 0; }
    return sfx_log_mode > 0 && sfx_log_n <= SFX_LOG_MAX;
}

// fields the pan law 0x08A61F90 reads in the channel (s0): vol +52, pan +56/+97, factor +96; effect +60, +64/+99, +98
static void sfx_log_pan(uint32_t s0) {
    if (s0 < 0x08800000u || s0 >= 0x0A000000u) return;
    printf(" | canal 0x%08X vol %u paneo %d+%d factor %d efx %u %d+%d %d", s0, psp_l32(s0 + 52), psp_l8(s0 + 56), psp_l8(s0 + 97), psp_l8(s0 + 96),
           psp_l32(s0 + 60), psp_l8(s0 + 64), psp_l8(s0 + 99), psp_l8(s0 + 98));
}

static void sfx_log_event(const char* what, const SasVoice& v, int voice, uint32_t extra) {
    if (sfx_log_mode < 0) { const char* e = getenv("TTT_SFX_LOG"); sfx_log_mode = e ? atoi(e) : 0; }
    if (!sfx_log_mode || sfx_log_n > SFX_LOG_MAX) return;
    sfx_log_n++;
    SfxWav* w = v.type == 4 ? sfx_wav(v.wav) : nullptr;
    printf("sfx %-8s voz %2d  %s  estado %d fin %d alt %lld vl %d vr %d cuadro %ld\n", what, voice, w ? w->name.c_str() : "(adpcm)", v.env_state, v.end, (long long)v.height, v.vl, v.vr, cuadros);
    fflush(stdout);
}

// game stack at keyOn: 0x08A5E150 <- 0x08A61AF4 <- 0x08A60F04 <- 0x08A5F300 <- 0x08A5E928 <- 0x0881AB5C <- 0x0881B13C <- game
static uint32_t sfx_origin(uint32_t sp, uint32_t* channel, uint32_t* stack) {
    uint32_t pf = sp + 16, f04 = pf + 48;
    *channel = 0; stack[0] = stack[1] = stack[2] = stack[3] = 0;
    if (psp_l32(sp) != 0x08A61CB4u || psp_l32(pf + 32) != 0x08A6107Cu) return 0;
    *channel = psp_l32(pf + 16);
    uint32_t r = psp_l32(f04 + 12), f300 = f04 + 16, f928 = f300 + 144;
    uint32_t org = r == 0x08A5F3A0u ? psp_l32(f300 + 136) : r == 0x08A5F4F0u ? psp_l32(f300 + 140) : 0;
    if (org != 0x08A5EAC4u || psp_l32(f928 + 52) != 0x0881AC74u) return org;
    uint32_t fab5c = f928 + 64, fb13c = fab5c + 48;
    stack[2] = psp_l32(f928 + 28); stack[3] = psp_l32(f928 + 24);
    if (psp_l32(fab5c + 36) != 0x0881B164u) return org;
    stack[0] = psp_l32(fb13c);
    if (stack[0] == 0x0881B188u) stack[1] = psp_l32(fb13c + 16);
    return org;
}

static void sfx_log(const SasVoice& v, int voice, uint32_t sp, int live, uint32_t s0) {
    if (sfx_log_mode < 0) { const char* e = getenv("TTT_SFX_LOG"); sfx_log_mode = e ? atoi(e) : 0; }
    if (!sfx_log_mode || sfx_log_n > SFX_LOG_MAX) return;
    sfx_log_n++;
    uint32_t ch, stack[4], org = sfx_origin(sp, &ch, stack);
    SfxWav* w = v.type == 4 ? sfx_wav(v.wav) : nullptr;
    printf("sfx voz %2d  %s :: %s  dir 0x%08X tam %u  cuadro %ld vivas %d juego %08X sonido %d vol %u | vl %d vr %d paso %d adsr %d/%d %d/%d %d/%d %d/%d sus %lld", voice,
           w ? w->pak.c_str() : "-", w ? w->name.c_str() : "(adpcm)", v.addr, v.size, cuadros, live, stack[0], (int)stack[2], stack[3],
           v.vl, v.vr, v.step, v.ma, v.ra, v.md, v.rd, v.ms, v.rs, v.mr, v.rr, (long long)v.sus_level);
    printf(" efx %d %d", v.fxl, v.fxr);
    sfx_log_pan(s0);
    if (ch) {
        uint32_t id = psp_l32(ch + 84), n = psp_l32(0x08B45664u);
        printf(" | id %08X canales", id);
        for (uint32_t i = 0; i < n && i < 64; i++) {
            uint32_t c = 0x08B45678u + i * 104;
            if (psp_l32(c + 84) == id) printf(" %u:%d", psp_l8(c), (int)(int8_t)psp_l8(c + 1));
        }
    }
    printf("\n");
    fflush(stdout);
}

// game query "voice ended?" (0x08A5F2B4), only when the answer changes
static uint32_t sfx_end_last[SAS_VOICES + 1];
static void sfx_log_end(uint32_t sp, uint32_t flags) {
    if (sfx_log_mode <= 0 || sfx_log_n > SFX_LOG_MAX) return;
    if (psp_l32(sp) != 0x08A5F2E4u) return;
    uint32_t voice = psp_l32(sp + 16), org = psp_l32(sp + 20);
    if (voice >= SAS_VOICES) return;
    uint32_t key = ((flags >> voice) & 1) | (org << 1);
    if (sfx_end_last[voice] == key + 1) return;
    sfx_end_last[voice] = key + 1;
    sfx_log_n++;
    printf("sfx termino? voz %2u -> %u | cuadro %ld origen 0x%08X\n", voice, (flags >> voice) & 1, cuadros, org);
    fflush(stdout);
}

static void sas_block(SasVoice& v) {
    static const int F[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    if (v.jump) { v.block = v.loop_start; v.jump = 0; }
    if (v.last) { v.end = 1; return; }
    if (v.block < 0 || (uint32_t)(v.block * 16 + 16) > v.size) { v.end = 1; return; }
    const uint8_t* d = psp_p(v.addr + (uint32_t)v.block * 16);
    int pr = d[0] >> 4, sh = d[0] & 15, fl = d[1];
    if (pr > 4) pr = 0;
    if (sh > 12) sh = 9;
    if (fl == 7) { v.end = 1; return; }
    if (fl == 6) v.loop_start = v.block;
    for (int i = 0; i < 28; i++) {
        int n = (i & 1) ? (d[2 + i / 2] >> 4) : (d[2 + i / 2] & 15);
        int s = (int)(int16_t)(n << 12) >> sh;
        s += (v.s1 * F[pr][0] + v.s2 * F[pr][1]) >> 6;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v.s2 = v.s1; v.s1 = s;
        v.pcm[i] = (int16_t)s;
    }
    if (fl == 3) { if (v.loop) v.jump = 1; else v.last = 1; }
    v.block++;
    v.idx = 0;
}

static int sas_advance(SasVoice& v) {
    if (v.end) return 0;
    if (v.type == 1) {
        if (v.idx >= 28) { sas_block(v); if (v.end) return 0; }
        return v.pcm[v.idx++];
    }
    if (v.type == 2) {
        int n = (int)(v.size / 2);
        if (n <= 0) { v.end = 1; return 0; }
        if (v.ppos >= n) {
            if (v.loop) v.ppos = v.loop_start;
            else { v.end = 1; return 0; }
        }
        const int16_t* s = (const int16_t*)psp_p(v.addr);
        return s[v.ppos++];
    }
    if (v.type == 4) {
        SfxWav* w = sfx_wav(v.wav);
        if (!w || w->n <= 0) { v.end = 1; return 0; }
        int limit = w->n;
        if (w->length && v.size < w->length) limit = (int)((int64_t)w->n * v.size / w->length);
        int looping = sfx_loop && v.loop && w->loop_end > w->loop_start && w->loop_end <= limit;
        int end = looping ? w->loop_end : limit;
        if (v.ppos >= end) {
            if (!looping) { v.end = 1; return 0; }
            v.ppos = w->loop_start > 0 ? w->loop_start : 0;
        }
        return w->pcm[v.ppos++];
    }
    if (v.type == 3) {
        v.lfsr = (v.lfsr >> 1) ^ ((v.lfsr & 1) ? 0xB4000000u : 0u);
        return (int)(int16_t)(v.lfsr >> 8);
    }
    v.end = 1;
    return 0;
}

static int sas_read(SasVoice& v) {
    int s = v.prev + (((v.act - v.prev) * (int)v.frac) >> 12);
    v.frac += (unsigned)v.step;
    while (v.frac >= 4096) { v.frac -= 4096; v.prev = v.act; v.act = sas_advance(v); }
    return s;
}

static void sas_curve(SasVoice& v, int mode, int rate) {
    switch (mode) {
    case 0: v.height += rate; break;
    case 1: v.height -= rate; break;
    case 2: v.height += (v.height <= (int64_t)SAS_ENV_MAX * 3 / 4) ? rate : rate / 4; break;
    case 3: v.height += (((SAS_ENV_MAX - v.height) * rate) >> 30) + 1; break;
    case 4: v.height -= ((v.height * rate) >> 30) + 1; break;
    case 5: v.height = rate; break;
    default: v.height -= rate; break;
    }
    if (v.height > SAS_ENV_MAX) v.height = SAS_ENV_MAX;
    if (v.height < 0) v.height = 0;
}

static void sas_env_step(SasVoice& v) {
    if (!snd_env) return;
    switch (v.env_state) {
    case 1: sas_curve(v, v.ma, v.ra); if (v.height >= SAS_ENV_MAX) { v.height = SAS_ENV_MAX; v.env_state = 2; } break;
    case 2: sas_curve(v, v.md, v.rd); if (v.height <= v.sus_level) v.env_state = 3; break;
    case 3: sas_curve(v, v.ms, v.rs); if (v.height <= 0) { v.height = 0; v.env_state = 0; v.end = 1; } break;
    case 4: sas_curve(v, v.mr, v.rr); if (v.height <= 0) { v.height = 0; v.env_state = 0; v.end = 1; } break;
    default: break;
    }
}

static void sas_key_on(SasVoice& v) {
    v.on = 1; v.end = 0; v.jump = 0; v.last = 0;
    v.block = 0; v.idx = 28; v.ppos = 0;
    v.s1 = v.s2 = v.prev = v.act = 0; v.frac = 0;
    memset(v.echo, 0, sizeof(v.echo)); v.echo_pos = 0; v.shade = 0;
    v.src_ok = 0; v.src_owner = -1; v.src_y = 0; v.uw_fc = 20000.0f; v.uw_g = 1.0f; v.uw_s = 0.0f; v.uw_z[0] = v.uw_z[1] = 0.0f;
    v.height = snd_env ? 0 : SAS_ENV_MAX;
    v.env_state = snd_env ? 1 : 3;
}

static void sas_key_off(SasVoice& v) {
    v.on = 0;
    if (v.type == 4) v.loop = 0;
    if (!snd_env) { v.end = 1; v.env_state = 0; v.height = 0; }
    else v.env_state = 4;
}

static int32_t sas_mix[4096 * 2];

// the game pans by lowering only the far ear (0x08A61F90); in 3D that ear also arrives late and without highs
struct SasSpace { int near_lvl, far_lvl, right, delay; float a; bool active; };

static SasSpace sas_space(const SasVoice& v) {
    SasSpace e;
    e.right = v.vr > v.vl;
    e.near_lvl = e.right ? v.vr : v.vl;
    e.far_lvl = e.right ? v.vl : v.vr;
    e.delay = 0; e.a = 0;
    e.active = snd_3d > 0 && e.near_lvl > 0 && e.far_lvl < e.near_lvl;
    if (!e.active) return e;
    double x = 1.0 - (double)e.far_lvl / e.near_lvl;
    x *= snd_3d >= 2 ? 2.2 : 1.5;
    if (x > 1.0) x = 1.0;
    int level = (int)(e.near_lvl * (1.0 - 0.75 * x));
    if (level < e.far_lvl) e.far_lvl = level;
    e.delay = (int)(x * snd_rate * 0.00066 + 0.5);
    if (e.delay > 63) e.delay = 63;
    double fc = 18000.0 - 15000.0 * x;
    e.a = (float)(1.0 - exp(-6.283185307 * fc / snd_rate));
    return e;
}

// port voices: BT3 sounds the game has no voice for (vol 0..0x7F, pan -0x40..0x40 as BT3 Snd_PlaySeEx), mixed into the first SAS core
bool psp_bt3_snd_pcm(int id, const int16_t** pcm, int* n, int* rate);
struct PortVoice { const int16_t* p; int n, rate; double pos; float gl, gr; };
static std::mutex pv_mx;
static std::vector<PortVoice> pv_list;
void psp_snd_bt3(int id, int vol, int pan) {
    const int16_t* p; int n, r;
    if (!psp_bt3_snd_pcm(id, &p, &n, &r) || vol <= 0) return;
    if (vol > 0x7F) vol = 0x7F;
    int pp = pan + 0x40; pp = pp < 0 ? 0 : pp > 0x7F ? 0x7F : pp;
    float g = vol / 127.0f * 0.3f;   // level matched by ear to TTT SFX
    PortVoice v{p, n, r, 0.0, g * std::min(1.0f, (127 - pp) / 64.0f), g * std::min(1.0f, pp / 64.0f)};
    std::lock_guard<std::mutex> l(pv_mx);
    if (pv_list.size() < 16) pv_list.push_back(v);
}
bool psp_tp_snd_pcm(const int16_t** pcm, int* n, int* rate);
void psp_snd_tp(float gain) {
    const int16_t* p; int n, r;
    if (gain <= 0.0f || !psp_tp_snd_pcm(&p, &n, &r)) return;
    PortVoice v{p, n, r, 0.0, gain, gain};
    std::lock_guard<std::mutex> l(pv_mx);
    if (pv_list.size() < 16) pv_list.push_back(v);
}
static void pv_mix(int grain, int hz) {
    std::lock_guard<std::mutex> l(pv_mx);
    if (pv_list.empty()) return;
    for (auto& v : pv_list) {
        double st = (double)v.rate / (hz > 0 ? hz : 44100);
        for (int i = 0; i < grain; i++) {
            int a = (int)v.pos;
            if (a + 1 >= v.n) { v.pos = v.n; break; }
            float f = (float)(v.pos - a), m = v.p[a] + (v.p[a + 1] - v.p[a]) * f;
            sas_mix[i * 2] += (int)(m * v.gl);
            sas_mix[i * 2 + 1] += (int)(m * v.gr);
            v.pos += st;
        }
    }
    pv_list.erase(std::remove_if(pv_list.begin(), pv_list.end(), [](const PortVoice& v) { return v.pos + 1 >= v.n; }), pv_list.end());
}
// underwater sound (TTT_SND_WATER): listener = fighter slot 0; source = the queue entry's owner (its submerged state) or its point
// (below the level, world Y down). Modes: 1 both under, 2 listener under / source out, 3 listener out / source under
static std::atomic<uint32_t> uw_mask{0}, uw_lvl{0x7FC00000u};
static std::atomic<int64_t> uw_ms{0};
static int64_t uw_now() { return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void psp_snd_water(uint32_t mask, float level) { uint32_t u; memcpy(&u, &level, 4); uw_mask = mask; uw_lvl = u; uw_ms = uw_now(); }
static uint32_t lip_voice[64];   // shout id a voice started (lip sync)
static void sas_src(SasVoice& v, Ctx& c) {
    uint32_t ch = 0, st[4] = {0, 0, 0, 0};
    sfx_origin(c.r[29], &ch, st);
    if (st[1] != 0x0893D6F4u && st[1] != 0x0893D684u) return;
    uint32_t q = psp_l32(0x08A7DBC8u), e = c.r[20];
    if (!q || q >= 0x10000000u || e < q || e >= q + 0x400u || (e & 3)) return;
    FReg y; y.u = psp_l32(e + 4);
    if (!(y.f == y.f) || std::fabs(y.f) > 1e6f) return;
    v.src_ok = 1; v.src_y = y.f; v.src_owner = (int8_t)(psp_l32(e + 0x1C) & 0xFF);
}
static int uw_on() {
    static int v = 1, n = 0;
    if ((n++ & 255) == 0) { const char* e = getenv("TTT_SND_WATER"); v = e && *e ? atoi(e) : 1; }
    return v;
}
static int uw_mode(const SasVoice& v, uint32_t mask, float lvl) {
    bool lis = mask & 1;
    bool src = v.src_ok ? (v.src_owner >= 0 && v.src_owner < 8 ? (mask >> v.src_owner & 1) != 0 : v.src_y > lvl) : lis;
    return lis && src ? 1 : lis ? 2 : src ? 3 : 0;
}
static float uw_bus[4096], uw_rv[4][8192], uw_damp[4];
static int uw_rp[4], uw_tail[4];
static void sas_core(SasCore* s, int grain) {
    memset(sas_mix, 0, (size_t)grain * 2 * sizeof(int32_t));
    static const float FC[4] = {20000.0f, 720.0f, 1080.0f, 420.0f}, GN[4] = {1.0f, 0.85f, 0.62f, 0.58f}, SN[4] = {0.0f, 0.3f, 0.21f, 0.0f};
    bool uw = uw_on() > 0 && uw_now() - uw_ms.load() < 500;
    uint32_t umask = uw ? uw_mask.load() : 0;
    float ulvl; { uint32_t u = uw_lvl.load(); memcpy(&ulvl, &u, 4); }
    if (!(ulvl == ulvl)) uw = false;
    int hz = s->hz > 0 ? s->hz : snd_rate;
    float sm = 1.0f - std::exp(-(float)grain / (0.15f * (float)hz));
    bool bus = false;
    if (grain > 4096) grain = 4096;
    memset(uw_bus, 0, sizeof(float) * (size_t)grain);
    for (int k = 0; k < s->max_voices && k < SAS_VOICES; k++) {
        SasVoice& v = s->v[k];
        if (v.type == 0 || v.end || v.paused || !v.env_state) continue;
        SasSpace e = sas_space(v);
        int ic = e.right ? 1 : 0, il = 1 - ic;
        int md = uw ? uw_mode(v, umask, ulvl) : 0;
        if (!(v.uw_fc > 1.0f)) { v.uw_fc = 20000.0f; v.uw_g = 1.0f; v.uw_s = 0.0f; v.uw_z[0] = v.uw_z[1] = 0.0f; }
        v.uw_fc = std::exp(std::log(v.uw_fc) + (std::log(FC[md]) - std::log(v.uw_fc)) * sm);
        v.uw_g += (GN[md] - v.uw_g) * sm; v.uw_s += (SN[md] - v.uw_s) * sm;
        bool flt = v.uw_fc < 16000.0f || v.uw_g < 0.98f || v.uw_s > 0.01f;
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, sv = (float)(v.vl + v.vr) * (0.5f / 4096.0f);
        if (flt) {   // RBJ low-pass, Q 0.707
            float w = 6.2831853f * std::min(v.uw_fc, 0.45f * (float)hz) / (float)hz, cs = std::cos(w), al = std::sin(w) * 0.70710678f, a0 = 1.0f + al;
            b1 = (1.0f - cs) / a0; b0 = b2 = b1 * 0.5f; a1 = -2.0f * cs / a0; a2 = (1.0f - al) / a0;
            bus = bus || v.uw_s > 0.01f;
        } else v.uw_z[0] = v.uw_z[1] = 0.0f;
        for (int i = 0; i < grain; i++) {
            int m = sas_read(v);
            sas_env_step(v);
            int g = (int)(v.height >> 15);
            m = (int)(((int64_t)m * g) >> 15);
            if (flt) {
                float x = (float)m, y = b0 * x + v.uw_z[0];
                v.uw_z[0] = b1 * x - a1 * y + v.uw_z[1]; v.uw_z[1] = b2 * x - a2 * y;
                uw_bus[i] += y * v.uw_s * sv;
                m = (int)(y * v.uw_g);
            }
            if (snd_3d > 0) { v.echo[v.echo_pos & 63] = m; v.echo_pos++; }
            if (e.active) {
                int r = v.echo[(v.echo_pos - 1 - e.delay) & 63];
                v.shade += e.a * ((float)r - v.shade);
                sas_mix[i * 2 + ic] += (m * e.near_lvl) >> 12;
                sas_mix[i * 2 + il] += ((int)v.shade * e.far_lvl) >> 12;
            } else {
                v.shade = (float)m;
                sas_mix[i * 2] += (m * v.vl) >> 12;
                sas_mix[i * 2 + 1] += (m * v.vr) >> 12;
            }
            if (v.end) break;
        }
    }
    int cix = (int)(s - sas_n);
    if (bus) uw_tail[cix] = hz / 2;
    if (uw_tail[cix] > 0) {   // short underwater room: two taps (37 / 53 ms) fed back through a damping low-pass, 0.5 s tail
        float* rb = uw_rv[cix];
        int d1 = std::min(8191, (int)(0.037f * (float)hz)), d2 = std::min(8191, (int)(0.053f * (float)hz));
        for (int i = 0; i < grain; i++) {
            int p = uw_rp[cix];
            float y = rb[(p - d1) & 8191] * 0.5f + rb[(p - d2) & 8191] * 0.35f;
            uw_damp[cix] += 0.3f * (y - uw_damp[cix]);
            rb[p] = uw_bus[i] + uw_damp[cix] * 0.55f;
            uw_rp[cix] = (p + 1) & 8191;
            sas_mix[i * 2] += (int)y; sas_mix[i * 2 + 1] += (int)y;
        }
        uw_tail[cix] -= grain;
        if (uw_tail[cix] <= 0) { memset(rb, 0, sizeof(uw_rv[0])); uw_damp[cix] = 0; uw_tail[cix] = 0; }
    }
    if (s == &sas_n[0]) pv_mix(grain, s->hz);
    sas_calls++;
}

static void sas_output(uint32_t addr, int grain, int vl, int vr, int mix) {
    int16_t* o = (int16_t*)psp_p(addr);
    for (int i = 0; i < grain * 2; i++) {
        int s = (int)(((int64_t)sas_mix[i] * (i & 1 ? vr : vl)) >> 12);
        if (mix) s += o[i];
        o[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

static unsigned sas_rate_simple(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    unsigned r = (unsigned)((7 - (n & 3)) << 26) >> (n >> 2);
    return r ? r : 1;
}

// ---------------- dispatch ----------------

static int snd_samples2 = 1024;
static int snd_ch_n[8], snd_ch_fmt[8], snd_ch_res[8];
static int snd_trace = 0;

static void snd_log_voice(const char* what, uint32_t core, int voice, uint32_t a, uint32_t b, uint32_t d) {
    if (!snd_log || snd_trace > 1200) return;
    snd_trace++;
    printf("sas %-16s nucleo 0x%08X voz %2d  0x%08X 0x%08X 0x%08X\n", what, core, voice, a, b, d);
}

#define ATRAC_N 6
// underwater sound for character voices (ATRAC served from wav): the speaker = the slot whose model pak shares the voice's folder
// (exact, else the character folder: forms, voice packs); same modes and filters as the SFX; other voices (announcer) untouched
static std::atomic<uint32_t> uw_pak[8];
void psp_snd_paks(const uint32_t* p) { for (int i = 0; i < 8; i++) uw_pak[i] = p[i]; }
struct UwFx { float fc = 20000.0f, g = 1.0f, s = 0.0f, z[2] = {0, 0}, rv[8192] = {0}, damp = 0; int rp = 0, tail = 0; uint32_t wid = 0; int owner = -2; };
static UwFx uw_at[ATRAC_N];
static int uw_voice_owner(uint32_t wid) {
    std::string k = lower(at3_key_of(wid));
    size_t v = k.find("/voices/");
    if (k.compare(0, 11, "characters/") || v == std::string::npos) return -2;
    std::string vd = k.substr(0, v);
    std::lock_guard<std::mutex> g(lip_mx);
    int ch = -1;
    for (int i = 0; i < 8; i++) {
        uint32_t pk = uw_pak[i].load();
        auto it = pk ? lip_paks.find(pk) : lip_paks.end();
        if (it == lip_paks.end()) continue;
        std::string f = lower(disc_pk[it->second].fpath), dir = f.substr(0, f.find_last_of('/'));
        if (dir == vd) return i;
        if (ch < 0 && lip_char_dir(dir) == lip_char_dir(vd)) ch = i;
    }
    return ch;
}
static void uw_voice(int di, uint32_t wid, int16_t* x, int n) {
    if (di < 0 || di >= ATRAC_N) return;
    UwFx& f = uw_at[di];
    if (f.wid != wid) { f.wid = wid; f.owner = uw_voice_owner(wid); }
    static const float FC[4] = {20000.0f, 720.0f, 1080.0f, 420.0f}, GN[4] = {1.0f, 1.3f, 0.95f, 0.9f}, SN[4] = {0.0f, 0.3f, 0.21f, 0.0f};   // voices: make-up gain for the lost highs
    float lvl; { uint32_t u = uw_lvl.load(); memcpy(&lvl, &u, 4); }
    int md = 0;
    if (f.owner != -2 && uw_on() > 0 && uw_now() - uw_ms.load() < 500 && lvl == lvl) {
        uint32_t m = uw_mask.load();
        bool lis = m & 1, src = f.owner >= 0 ? (m >> f.owner & 1) != 0 : lis;
        md = lis && src ? 1 : lis ? 2 : src ? 3 : 0;
    }
    const int B = 256;
    float sm = 1.0f - std::exp(-(float)B / (0.15f * 44100.0f));
    int d1 = (int)(0.037f * 44100.0f), d2 = (int)(0.053f * 44100.0f);
    for (int o = 0; o < n; o += B) {
        f.fc = std::exp(std::log(f.fc) + (std::log(FC[md]) - std::log(f.fc)) * sm);
        f.g += (GN[md] - f.g) * sm; f.s += (SN[md] - f.s) * sm;
        bool flt = f.fc < 16000.0f || f.g < 0.98f || f.s > 0.01f;
        if (!flt && f.tail <= 0) { f.z[0] = f.z[1] = 0; continue; }
        float w = 6.2831853f * f.fc / 44100.0f, cs = std::cos(w), al = std::sin(w) * 0.70710678f, a0 = 1.0f + al;
        float b1 = (1.0f - cs) / a0, b0 = b1 * 0.5f, b2 = b0, a1 = -2.0f * cs / a0, a2 = (1.0f - al) / a0;
        if (f.s > 0.01f) f.tail = 22050;
        for (int i = o; i < o + B && i < n; i++) {
            float in = (float)x[i], y = in;
            if (flt) { y = b0 * in + f.z[0]; f.z[0] = b1 * in - a1 * y + f.z[1]; f.z[1] = b2 * in - a2 * y; }
            float r = 0;
            if (f.tail > 0) {
                r = f.rv[(f.rp - d1) & 8191] * 0.5f + f.rv[(f.rp - d2) & 8191] * 0.35f;
                f.damp += 0.3f * (r - f.damp);
                f.rv[f.rp] = y * f.s + f.damp * 0.55f;
                f.rp = (f.rp + 1) & 8191;
                f.tail--;
            }
            float out = (flt ? y * f.g : in) + r;
            x[i] = (int16_t)(out > 32767.0f ? 32767 : out < -32768.0f ? -32768 : (int)out);
        }
        if (f.tail <= 0 && !(f.s > 0.01f)) { memset(f.rv, 0, sizeof f.rv); f.damp = 0; f.tail = 0; }
    }
}
struct AtracDec { int used, codec, ch_in, ch_out, bytes, errors, wav; long long cuadros, dumped; FILE* f; At3 dec; uint32_t tw; int64_t tf, tn; int tlog, synth; };
static AtracDec atrac[ATRAC_N];
static int atrac_dump = -1;

// TTT_ATRAC_DUMP=1: saves the raw stream each decoder receives to atrac_<id>.bin
static void atrac_dump_stream(AtracDec& d, uint32_t src, int bytes) {
    if (atrac_dump < 0) { const char* e = getenv("TTT_ATRAC_DUMP"); atrac_dump = e ? atoi(e) : 0; }
    if (!atrac_dump || !src || d.dumped > 12000) return;
    if (!d.f) {
        char fname[32];
        snprintf(fname, sizeof(fname), "atrac_%d.bin", (int)(&d - atrac));
        d.f = fopen(fname, "ab");
        if (!d.f) return;
        if (ftell(d.f) == 0) {
            uint32_t hdr[4] = {0x44525441u, (uint32_t)d.codec, (uint32_t)((d.ch_in << 16) | d.ch_out), (uint32_t)d.bytes};
            fwrite(hdr, 4, 4, d.f);
        }
    }
    fwrite(psp_p(src), 1, (size_t)bytes, d.f);
    d.dumped++;
}

static int hle_trace_left = 0;
static void (*hle_ring_dump_fn)() = [] {};
static bool snd_hle(Ctx& c, const char* n, uint32_t* out) {
    if (strncmp(n, "__sceSas", 8) && strncmp(n, "sceAudio", 8) && strncmp(n, "sceAtrac", 8)) return false;
    snd_cfg();
    uint32_t a0 = c.r[4], a1 = c.r[5], a2 = c.r[6], a3 = c.r[7];
    uint32_t t0 = c.r[8], t1 = c.r[9], t2 = c.r[10];
    uint32_t res = 0;

    if (!strncmp(n, "__sceSas", 8)) {
        if (!snd_sas) {
            res = (strstr(n, "GetEndFlag") || strstr(n, "GetPauseFlag")) ? 0xFFFFFFFFu : 0;
            *out = res;
            return true;
        }
        SasCore* s = sas_find(a0);
        int k = (int)a1;
        SasVoice* v = (k >= 0 && k < SAS_VOICES) ? &s->v[k] : nullptr;
        if (!strcmp(n, "__sceSasInit")) {
            s->grain = (int)a1; s->max_voices = (int)a2; s->mode = (int)a3; s->hz = (int)t0;
            if (s->grain < 16 || s->grain > 4096) s->grain = 256;
            if (s->max_voices < 1 || s->max_voices > SAS_VOICES) s->max_voices = SAS_VOICES;
            if (!snd_rate_forced && s->hz >= 8000 && s->hz <= 48000) snd_rate = s->hz;
            printf("sas init: nucleo 0x%08X grano %d voces %d modo %d %d Hz\n", a0, s->grain, s->max_voices, s->mode, s->hz);
            fflush(stdout);
        } else if (!strcmp(n, "__sceSasSetVoice") && v) {
            if (v->type && !v->end && v->env_state) sfx_log_event("reemplazo", *v, k, a2);
            // negative size = empty PPVA slot: the hardware stays silent (same as PPSSPP)
            v->type = a2 && (int)a3 > 0 ? 1 : 0; v->addr = a2; v->size = (int)a3 > 0 ? a3 : 0; v->loop = 1;
            v->loop_start = 0; v->end = 1; v->env_state = 0;
            if (v->type) { sas_detect_wav(*v); sas_step(*v); }
            else if ((int)a3 < 0 && sfx_log_mode > 0) { printf("sfx voz %2d  hueco vacio (tam %d) dir 0x%08X\n", k, (int)a3, a2); fflush(stdout); }
            snd_log_voice("setVoice", a0, k, a2, a3, t0);
        } else if (!strcmp(n, "__sceSasSetVoicePCM") && v) {
            v->type = a2 ? 2 : 0; v->addr = a2; v->size = a3;
            v->loop = (int)t0 >= 0 ? 1 : 0; v->loop_start = (int)t0 > 0 ? (int)t0 : 0;
            v->end = 1; v->env_state = 0;
            snd_log_voice("setVoicePCM", a0, k, a2, a3, t0);
        } else if (!strcmp(n, "__sceSasSetPitch") && v) {
            v->pitch = (int)a2 < 1 ? 1 : ((int)a2 > 0x4000 ? 0x4000 : (int)a2);
            sas_step(*v);
            snd_log_voice("setPitch", a0, k, a2, 0, 0);
        } else if (!strcmp(n, "__sceSasSetVolume") && v) {
            int al = v->vl, ar = v->vr;
            v->vl = (int)a2; v->vr = (int)a3; v->fxl = (int)t0; v->fxr = (int)t1;
            if (v->vl < 0) v->vl = -v->vl;
            if (v->vr < 0) v->vr = -v->vr;
            // 0x08A61DA0 = per-frame update of a playing channel (0x08A61CFC)
            uint32_t org = psp_l32(c.r[29]);
            if (org == 0x08A61DA0u && v->type && !v->end && v->env_state && (al != v->vl || ar != v->vr) && sfx_log_enabled()) {
                sfx_log_n++;
                SfxWav* w = v->type == 4 ? sfx_wav(v->wav) : nullptr;
                printf("sfx volumen voz %2d  %s  vl %d vr %d antes %d %d efx %d %d cuadro %ld", k, w ? w->name.c_str() : "(adpcm)",
                       (int)a2, (int)a3, al, ar, (int)t0, (int)t1, cuadros);
                sfx_log_pan(c.r[16]);
                printf("\n");
                fflush(stdout);
            }
            snd_log_voice("setVolume", a0, k, a2, a3, t0);
        } else if (!strcmp(n, "__sceSasSetKeyOn") && v) {
            if (v->type == 1) { sas_detect_wav(*v); sas_step(*v); }
            sas_key_on(*v);
            if (!v->type) { v->end = 1; v->env_state = 0; }
            int live = 0;
            for (int i = 0; i < SAS_VOICES; i++) if (s->v[i].type && !s->v[i].end && s->v[i].env_state) live++;
            sfx_log(*v, k, c.r[29], live, c.r[16]);
            snd_log_voice(v->type == 4 ? "keyOn wav" : "keyOn", a0, k, v->addr, v->size, (uint32_t)(v->type == 4 ? v->wav : v->pitch));
            psp_sfx_hook(v->addr, v->size, c.r[31]);
            sas_src(*v, c);
            if (lip_voice[k]) { psp_lip_shout_off(lip_voice[k]); lip_voice[k] = 0; }
            {   // battle sound of a fighter (queue entry in s4, EBOOT_TTT 113.1): its shouts move the mouth
                uint32_t lch = 0, lst[4] = {0, 0, 0, 0}; sfx_origin(c.r[29], &lch, lst);
                // the queue entry: stored by the queue loop at +8 of its frame (0x0893D4C0), above the 0x10 frame of 0x0881B13C; s4 is reused by 0x0881AB5C
                uint32_t q = psp_l32(0x08A7DBC8u), qe = psp_l32(c.r[29] + 16 + 48 + 16 + 144 + 64 + 48 + 0x10 + 8);
                if ((lst[0] == 0x0893D6F4u || lst[0] == 0x0893D684u) && q && q < 0x10000000u && qe >= q && qe < q + 0x400u && !(qe & 3)) {
                    int owner = (int8_t)psp_l8(qe + 0x1C), bank = psp_l8(qe + 0x1D), snd = (int)psp_l32(qe + 0x18);
                    if (owner >= 0 && owner < 8) lip_voice[k] = v->type == 4 ? psp_lip_sfx(v->wav, v->loop != 0, owner, snd, bank) : v->type == 1 ? psp_lip_raw(v->addr, v->size, v->pitch, v->loop != 0, owner, snd, bank) : 0;
                    static int ll = 0;
                    if ((snd == 35 || snd == 36) && !lip_voice[k] && ll++ < 10) printf("lip: sonido %d banco %d sin boca: dueno %d, tipo %d, tam %u, paso %d\n", snd, bank, owner, v->type, v->size, v->pitch);
                } else if (lst[0] == 0x0893D6F4u || lst[0] == 0x0893D684u) {
                    static int lq = 0;
                    if (lq++ < 6) printf("lip: entrada de la cola no valida: cola 0x%08X entrada 0x%08X sonido %d\n", q, qe, (int)lst[2]);
                }
            }
        } else if (!strcmp(n, "__sceSasSetKeyOff") && v) {
            sfx_log_event("keyOff", *v, k, c.r[31]);
            if (lip_voice[k]) { psp_lip_shout_off(lip_voice[k]); lip_voice[k] = 0; }
            sas_key_off(*v);
            snd_log_voice("keyOff", a0, k, v->addr, (uint32_t)v->height, 0);
        } else if (!strcmp(n, "__sceSasSetNoise") && v) {
            v->type = a2 ? 3 : v->type; v->noise = (int)a2;
        } else if (!strcmp(n, "__sceSasSetADSR") && v) {
            if (a2 & 1) v->ra = (int)a3;
            if (a2 & 2) v->rd = (int)t0;
            if (a2 & 4) v->rs = (int)t1;
            if (a2 & 8) v->rr = (int)t2;
            snd_log_voice("setADSR", a0, k, a2, a3, t0);
        } else if (!strcmp(n, "__sceSasSetADSRmode") && v) {
            if (a2 & 1) v->ma = (int)a3;
            if (a2 & 2) v->md = (int)t0;
            if (a2 & 4) v->ms = (int)t1;
            if (a2 & 8) v->mr = (int)t2;
            snd_log_voice("setADSRmode", a0, k, a2, a3, t0);
        } else if (!strcmp(n, "__sceSasSetSimpleADSR") && v) {
            unsigned e1 = a2 & 0xFFFF, e2 = a3 & 0xFFFF;
            v->ra = (int)sas_rate_simple((int)((e1 >> 8) & 0x7F));
            v->ma = (e1 & 0x8000) ? 2 : 0;
            v->rd = (int)sas_rate_simple((int)(((e1 >> 4) & 0x0F) * 4));
            v->md = 4;
            v->sus_level = (int64_t)SAS_ENV_MAX * (int)((e1 & 0x0F) + 1) / 16;
            v->rs = (int)sas_rate_simple((int)((e2 >> 6) & 0x7F));
            v->ms = (e2 & 0x8000) ? 4 : ((e2 & 0x4000) ? 1 : 0);
            v->rr = (int)sas_rate_simple((int)((e2 & 0x1F) * 4));
            v->mr = (e2 & 0x20) ? 4 : 1;
            snd_log_voice("simpleADSR", a0, k, e1, e2, 0);
        } else if (!strcmp(n, "__sceSasSetSL") && v) {
            v->sus_level = (int64_t)(int)a2;
            if (v->sus_level <= 0 || v->sus_level > SAS_ENV_MAX) v->sus_level = SAS_ENV_MAX / 2;
        } else if (!strcmp(n, "__sceSasSetPause")) {
            if (sfx_log_mode > 0) { printf("sfx pausa    mascara 0x%08X valor %u\n", a1, a2); fflush(stdout); }
            for (int i = 0; i < SAS_VOICES; i++) if (a1 & (1u << i)) s->v[i].paused = a2 ? 1 : 0;
        } else if (!strcmp(n, "__sceSasGetPauseFlag")) {
            for (int i = 0; i < SAS_VOICES; i++) if (s->v[i].paused) res |= 1u << i;
        } else if (!strcmp(n, "__sceSasGetEndFlag")) {
            for (int i = 0; i < SAS_VOICES; i++) if (s->v[i].end || !s->v[i].env_state) res |= 1u << i;
            sfx_log_end(c.r[29], res);
        } else if (!strcmp(n, "__sceSasGetEnvelopeHeight")) {
            res = v ? (uint32_t)v->height : 0;
        } else if (!strcmp(n, "__sceSasGetAllEnvelopeHeights")) {
            for (int i = 0; i < SAS_VOICES; i++) psp_s32(a1 + (uint32_t)i * 4, (uint32_t)s->v[i].height);
        } else if (!strcmp(n, "__sceSasGetGrain")) {
            res = (uint32_t)s->grain;
        } else if (!strcmp(n, "__sceSasSetGrain")) {
            if ((int)a1 >= 16 && (int)a1 <= 4096) s->grain = (int)a1;
        } else if (!strcmp(n, "__sceSasGetOutputmode")) {
            res = (uint32_t)s->mode;
        } else if (!strcmp(n, "__sceSasSetOutputmode")) {
            s->mode = (int)a1;
        } else if (!strcmp(n, "__sceSasCore")) {
            sas_core(s, s->grain);
            sas_output(a1, s->grain, SAS_VOL_MAX, SAS_VOL_MAX, 0);
        } else if (!strcmp(n, "__sceSasCoreWithMix")) {
            int vl = (int)a2, vr = (int)a3;
            if (vl < 0) vl = -vl;
            if (vr < 0) vr = -vr;
            sas_core(s, s->grain);
            sas_output(a1, s->grain, vl, vr, 1);
        }
        *out = res;
        return true;
    }

    if (!strncmp(n, "sceAtrac", 8)) {
        int id = (int)a0;
        AtracDec* d = (id >= 0 && id < ATRAC_N) ? &atrac[id] : nullptr;
        if (!strcmp(n, "sceAtracLowLevelDecode")) {
            int bytes = d && d->bytes ? d->bytes : 0x180;
            int samples = d && d->codec == 0x1000 ? 2048 : 1024;
            int canales = d && d->ch_out ? d->ch_out : 2;
            if (d) { atrac_dump_stream(*d, a1, bytes); d->cuadros++; }
            bool done = false;
            uint32_t wid = 0, woff = 0;
            // stream exposed as wav: the marker carries the id and the offset inside the entry
            // music past its last frame (item 41): the game keeps reading the ring with the frame grid shifted or past the
            // entry; a marker anywhere in the frame gives its position, and without one the wav continues from frame 0 (TTT_AT3_LOOP=0 off)
            static int loop_on = -1;
            if (loop_on < 0) { const char* e = getenv("TTT_AT3_LOOP"); loop_on = !(e && *e == '0'); }
            bool mk = d && a1 && a3 && at3_marker(psp_p(a1), &wid, &woff);
            int64_t shift = 0;
            if (!mk && loop_on && d && a1 && a3 && d->tw) {
                const uint8_t* q = psp_p(a1);
                for (int k = 1; k + 16 <= bytes && !mk; k++)
                    if (q[k] == 'T' && at3_marker(q + k, &wid, &woff)) { mk = true; shift = k; }
                static int sl = 0;
                if (mk && sl++ < 20) printf("at3 musica: id %d marca desplazada %lld bytes en la trama %u\n", id, (long long)shift, woff / (uint32_t)(bytes > 0 ? bytes : 304));
                if (!mk && d->ch_in >= 2 && (d->synth || d->tf >= d->tn - 3) && d->tn > 0) {
                    int64_t nf = d->tf + 1 >= d->tn ? 0 : d->tf + 1;
                    static int ul = 0;
                    if (!d->synth && ul++ < 20) printf("at3 musica: id %d sin marca tras la trama %lld (primeros bytes %02X %02X %02X %02X), sigue desde la trama 0\n", id, (long long)d->tf, q[0], q[1], q[2], q[3]);
                    d->synth = 1;
                    if (at3_fill(d->tw, nf, (int16_t*)psp_p(a3), true)) { d->tf = nf; done = true; }
                }
            }
            if (mk) {
                bool stereo = d->ch_in >= 2;
                int64_t fr = ((int64_t)woff - shift + (bytes > 0 ? bytes : (int)AT3_STEP) / 2) / (bytes > 0 ? bytes : (int)AT3_STEP);
                d->synth = 0;
                if (stereo) {   // music trace (item 41): start, jumps, last frames, another id on the same wav
                    static int tl = 0;
                    if (d->tw != wid) {
                        d->tw = wid; d->tn = at3_frames_of(wid, true); d->tlog = 0;
                        for (int k = 0; k < ATRAC_N; k++) if (&atrac[k] != d && atrac[k].used && atrac[k].tw == wid && tl < 200) { tl++; printf("at3 musica: id %d y id %d sirven el mismo wav %u\n", id, k, wid); }
                        if (tl++ < 200) printf("at3 musica: id %d empieza %s en la trama %lld de %lld\n", id, at3_key_of(wid).c_str(), (long long)fr, (long long)d->tn);
                    } else if ((fr < d->tf || fr > d->tf + 2) && tl < 200) { tl++; printf("at3 musica: id %d salta de la trama %lld a %lld (de %lld)\n", id, (long long)d->tf, (long long)fr, (long long)d->tn); }
                    if (fr >= d->tn - 3 && d->tlog < 4 && tl < 200) { tl++; d->tlog++; printf("at3 musica: id %d lee la trama final %lld de %lld\n", id, (long long)fr, (long long)d->tn); }
                    d->tf = fr;
                }
                if (at3_fill(wid, fr, (int16_t*)psp_p(a3), stereo)) {
                    if (!stereo) uw_voice((int)(d - atrac), wid, (int16_t*)psp_p(a3), 2048);
                    if (!d->wav) { d->wav = 1; printf("at3: id %d sirviendo wav %u, %s\n", id, wid, stereo ? "estereo" : "mono"); fflush(stdout); }
                    done = true;
                }
            }
            if (!done && d && d->dec.listo && a1 && a3 && at3_decodificar(d->dec, psp_p(a1)) == 0) {
                int16_t* o = (int16_t*)psp_p(a3);
                int ce = d->dec.canales;
                for (int i = 0; i < 1024; i++)
                    for (int k = 0; k < canales; k++) {
                        float v = d->dec.sal[k < ce ? k : ce - 1][i] * 32767.0f;
                        o[i * canales + k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
                    }
                done = true;
            } else if (!done && d && d->dec.listo) {
                d->errors++;
                if (d->errors == 1 && d->dec.canales == 2 && !d->dec.conjunto) {
                    at3_iniciar(d->dec, 2, d->bytes, 1);
                    printf("atrac: id %d probando estereo conjunto\n", id);
                    fflush(stdout);
                }
            }
            if (!done && a3) memset(psp_p(a3), 0, (size_t)samples * canales * 2);
            if (a2) psp_s32(a2, (uint32_t)bytes);
            if (t0) psp_s32(t0, (uint32_t)samples);
            psp_block(psp_time_us() + 100);
        } else if (!strcmp(n, "sceAtracLowLevelInitDecoder")) {
            if (d && a1) {
                d->ch_in = (int)psp_l32(a1); d->ch_out = (int)psp_l32(a1 + 4); d->bytes = (int)psp_l32(a1 + 8);
                if (d->bytes < 8 || d->bytes > 8192) d->bytes = 0x180;
                d->errors = 0;
                if (d->codec == 0x1001) at3_iniciar(d->dec, d->ch_in, d->bytes, 0);
                else d->dec.listo = 0;
                printf("atrac init: id %d codec 0x%04X canales %d->%d, %d bytes por cuadro%s\n", id, d->codec, d->ch_in, d->ch_out, d->bytes,
                       d->dec.listo ? "" : " (sin decodificador, silencio)");
                fflush(stdout);
            }
        } else if (!strcmp(n, "sceAtracGetAtracID")) {
            int k = 0;
            while (k < ATRAC_N && atrac[k].used) k++;
            if (k >= ATRAC_N) { *out = 0x80630007u; return true; }
            memset(&atrac[k], 0, sizeof(AtracDec));
            atrac[k].used = 1; atrac[k].codec = (int)a0;
            printf("atrac: id %d creado para codec 0x%04X\n", k, (int)a0);
            fflush(stdout);
            res = (uint32_t)k;
        } else if (!strcmp(n, "sceAtracReleaseAtracID")) {
            if (d && !d->used) {
                static unsigned rep = 0;
                rep++;
                if (rep == 1) hle_ring_dump_fn();
                if (rep == 1 || rep == 2 || rep == 3 || rep == 1000 || rep == 100000) {
                    uint32_t s0 = c.r[16], s1 = c.r[17], sp = c.r[29];
                    printf("atrac: id %d liberado otra vez (%u), hilo %d, ra 0x%08X, s0 0x%08X, s1 0x%08X, sp 0x%08X\n", id, rep, psp_thread_current(), c.r[31], s0, s1, sp);
                    auto dump = [](const char* t, uint32_t a, int nw) {
                        if (a < 0x08800000u || a >= 0x10000000u) return;
                        printf("atrac:   %s 0x%08X:", t, a);
                        for (int k = 0; k < nw; k++) printf(" %08X", psp_l32(a + 4u * k));
                        printf("\n");
                    };
                    dump("s0", s0, 40); dump("s1", s1, 48);
                    uint32_t pp = sp + 0x501C;
                    if (pp >= 0x08800000u && pp < 0x10000000u) { uint32_t q = psp_l32(pp); dump("[sp+501C]", q, 16); if (q >= 0x08800000u && q < 0x10000000u) dump("[[sp+501C]+8]", psp_l32(q + 8), 8); }
                    fflush(stdout);
                }
                if (rep > 3) d = nullptr;
            } else if (d && d->used) {
                static int logs = 0;
                if (logs++ < 6) printf("atrac: id %d liberado por ra 0x%08X, hilo %d, s0 0x%08X\n", id, c.r[31], psp_thread_current(), c.r[16]);
            }
            if (d) { if (d->f) fclose(d->f); d->f = nullptr; d->used = 0; printf("atrac: id %d liberado tras %lld cuadros%s\n", id, d->cuadros, d->tw ? (", wav en la trama " + std::to_string(d->tf) + " de " + std::to_string(d->tn)).c_str() : ""); d->tw = 0; }
        } else if (!strcmp(n, "sceAtracDecodeData")) {
            if (a2) psp_s32(a2, 1024);
            if (a3) psp_s32(a3, 0);
            if (t0) psp_s32(t0, 0);
            psp_block(psp_time_us() + 100);
        }
        *out = res;
        return true;
    }

    if (!strcmp(n, "sceAudioOutput2Reserve") || !strcmp(n, "sceAudioSRCChReserve")) {
        if (a0) snd_samples2 = (int)a0;
        if (!strcmp(n, "sceAudioSRCChReserve") && !snd_rate_forced && (int)a1 >= 8000 && (int)a1 <= 48000) snd_rate = (int)a1;
        printf("audio: %s muestras %d, salida a %d Hz\n", n, snd_samples2, snd_rate);
        fflush(stdout);
    } else if (!strcmp(n, "sceAudioOutput2Release")) {
        snd_ch_live[8] = 0;
    } else if (!strcmp(n, "sceAudioOutput2OutputBlocking") || !strcmp(n, "sceAudioSRCOutputBlocking")) {
        int vol = (int)a0;
        static int t = 0;
        if (t < 8) {
            t++;
            const int16_t* q = a1 ? (const int16_t*)psp_p(a1) : nullptr;
            long long e = 0;
            if (q) for (int i = 0; i < snd_samples2 * 2; i++) e += (long long)q[i] * q[i];
            printf("audio salida2: vol 0x%08X buf 0x%08X rms %.1f\n", a0, a1,
                   q ? sqrt((double)e / (snd_samples2 * 2)) : 0.0);
            fflush(stdout);
        }
        if (vol < 0) vol = 0x8000;
        if (vol > 0x8000) vol = 0x8000;
        if (a1) snd_put(8, a1, snd_samples2, vol, vol, 0);
        static long long n_out = 0;
        if (++n_out % 400 == 0) {
            int act = 0;
            for (int i = 0; i < 4; i++) if (sas_n[i].used)
                for (int q = 0; q < SAS_VOICES; q++) if (sas_n[i].v[q].type && !sas_n[i].v[q].end) act++;
            printf("sound: salida %lld, atraso %lld muestras, %lld vacios, %d voces vivas\n",
                   n_out, snd_lag(), snd_starved, act);
            fflush(stdout);
        }
        snd_pause(snd_samples2);
        res = (uint32_t)snd_samples2;
    } else if (!strcmp(n, "sceAudioChReserve")) {
        int ch = (int)a0;
        if (ch < 0 || ch > 7) { ch = 0; while (ch < 8 && snd_ch_res[ch]) ch++; if (ch >= 8) { *out = 0x80000004u; return true; } }
        snd_ch_res[ch] = 1;
        snd_ch_n[ch] = (int)a1 > 0 ? (int)a1 : 1024;
        snd_ch_fmt[ch] = (int)a2;
        printf("audio: canal %d reservado, %d muestras, formato %d\n", ch, snd_ch_n[ch], snd_ch_fmt[ch]);
        fflush(stdout);
        res = (uint32_t)ch;
    } else if (!strcmp(n, "sceAudioChRelease")) {
        int ch = (int)a0 & 7;
        snd_ch_res[ch] = 0;
        snd_ch_live[ch] = 0;
    } else if (!strcmp(n, "sceAudioSetChannelDataLen")) {
        int ch = (int)a0 & 7;
        if ((int)a1 > 0) snd_ch_n[ch] = (int)a1;
    } else if (!strcmp(n, "sceAudioChangeChannelConfig")) {
        snd_ch_fmt[(int)a0 & 7] = (int)a1;
    } else if (!strcmp(n, "sceAudioChangeChannelVolume")) {
        res = 0;
    } else if (!strcmp(n, "sceAudioOutputBlocking") || !strcmp(n, "sceAudioOutputPanned") ||
               !strcmp(n, "sceAudioOutputPannedBlocking")) {
        int ch = (int)a0 & 7;
        bool pan = strstr(n, "Panned") != nullptr;
        int vl = pan ? (int)a1 : (int)a1, vr = pan ? (int)a2 : (int)a1;
        uint32_t buf = pan ? a3 : a2;
        if (vl < 0) vl = 0;
        if (vr < 0) vr = 0;
        if (vl > 0x8000) vl = 0x8000;
        if (vr > 0x8000) vr = 0x8000;
        int mono = (snd_ch_fmt[ch] & 0x10) ? 1 : 0;
        if (buf) snd_put(ch, buf, snd_ch_n[ch], vl, vr, mono);
        if (strstr(n, "Blocking")) snd_pause(snd_ch_n[ch]);
        res = (uint32_t)snd_ch_n[ch];
    } else if (!strcmp(n, "sceAudioOutput2GetRestSample") || !strcmp(n, "sceAudioGetChannelRestLen") ||
               !strcmp(n, "sceAudioGetChannelRestLength")) {
        res = 0;
    } else {
        return false;
    }
    *out = res;
    return true;
}

static void snd_report() {
    if (!snd_cfg_done) return;
    for (int i = 0; i < ATRAC_N; i++) if (atrac[i].f) { fflush(atrac[i].f); fclose(atrac[i].f); atrac[i].f = nullptr; }
    for (int i = 0; i < ATRAC_N; i++) if (atrac[i].used || atrac[i].cuadros)
        printf("atrac: id %d codec 0x%04X %d->%d canales %d bytes, %lld cuadros, %d errores, %lld volcados\n", i, atrac[i].codec,
               atrac[i].ch_in, atrac[i].ch_out, atrac[i].bytes, atrac[i].cuadros, atrac[i].errors, atrac[i].dumped);
    printf("\nsonido: %lld bloques enviados (%d muestras), %lld llamadas al mezclador sas,"
           " %lld desbordes, %lld vacios, atraso final %lld muestras\n",
           snd_sent_blocks, snd_block, sas_calls, snd_overflows, snd_starved, snd_lag());
    for (int i = 0; i < 4; i++) {
        if (!sas_n[i].used) continue;
        int act = 0;
        for (int k = 0; k < SAS_VOICES; k++) if (sas_n[i].v[k].type && !sas_n[i].v[k].end) act++;
        printf("sound: nucleo 0x%08X grano %d voces %d modo %d %d Hz, %d voces vivas al final\n",
               sas_n[i].addr, sas_n[i].grain, sas_n[i].max_voices, sas_n[i].mode, sas_n[i].hz, act);
    }
    fflush(stdout);
}
