/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave in a browser: the firmware's own sources (firmware/src/obervave.c, unchanged) on the host build's fake
 * FM-1 (tests/host.c: screen, settings, patterns, MIDI queue, buttons, LEDs), compiled to WebAssembly by
 * web/sim/build.sh (zig cc, wasm32-freestanding). The pattern follows Makkuro37's web/sim (M. Geisler, GPL-3.0) and
 * winampFM's simulator (hericdk, GPL-3.0): the page's AudioWorklet calls sim_render(n) and the device's clock runs
 * with the audio.
 *
 * The app's main loop never returns (app_main: for (;;) { platform_poll(); ... }, the splash polls too), so it runs
 * as a coroutine: Binaryen's Asyncify (wasm-opt --asyncify) lets platform_poll() park the whole loop and the next
 * millisecond resume it right there. One pass of the loop per millisecond of audio, as tests/host.c does.
 * The ISR is app_audio(256 frames) as on the device; sim_render serves the worklet's 128-frame quanta out of it. */
#define BOARD_SMK37 1
#ifndef APP_VER
#define APP_VER "1.0.0"
#endif
#define OBV_HOST 1
#define OBV_WASM 1
#include <stdint.h>
#include <stddef.h>

#define EXPORT __attribute__((used, visibility("default")))

/* the few libc calls (struct copies, zeroing; host.c's settings): bulk-memory instructions */
void *memset(void *d, int c, size_t n) { return __builtin_memset(d, c, n); }
void *memcpy(void *d, const void *s, size_t n) { return __builtin_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return __builtin_memmove(d, s, n); }
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}
#define printf(...) ((void)0)                   /* (host.c's log lines: nowhere to go) */
static char *getenv(const char *n) { (void)n; return 0; }   /* (host.c's test switches: none in the browser) */

#include "../../firmware/src/obervave.c"

/* ---- Asyncify (wasm-opt --asyncify): the main loop as a coroutine */
__attribute__((import_module("asyncify"), import_name("start_unwind"))) void asyncify_start_unwind(void *p);
__attribute__((import_module("asyncify"), import_name("stop_unwind"))) void asyncify_stop_unwind(void);
__attribute__((import_module("asyncify"), import_name("start_rewind"))) void asyncify_start_rewind(void *p);
__attribute__((import_module("asyncify"), import_name("stop_rewind"))) void asyncify_stop_rewind(void);
static struct { void *pos, *end; } aco;         /* the parked loop's locals go here (Asyncify's data) */
static uint8_t aco_stack[16384] __attribute__((aligned(8)));
static volatile uint32_t parked, booted;

/* the app calls this every pass: park until the next millisecond. Not inlined: Asyncify instruments the callers of
 * the function that unwinds, and app_main must be one of them */
__attribute__((noinline)) static void platform_poll(void)
{
    if (!parked) {
        parked = 1;
        asyncify_start_unwind(&aco);
    } else {
        parked = 0;
        asyncify_stop_rewind();
    }
}
static void sim_app(void) { app_main(); }
static void (*volatile sim_app_p)(void) = sim_app;   /* (called through a pointer: app_main never returns, so a direct
                                                       call would let the compiler drop what follows it) */
__attribute__((noinline)) static void sim_pass(void)   /* one pass of the main loop (the first: boot, up to its first poll) */
{
    if (!booted) {
        booted = 1;
        aco.pos = aco_stack;
        aco.end = aco_stack + sizeof aco_stack;
    } else
        asyncify_start_rewind(&aco);
    sim_app_p();
    asyncify_stop_unwind();
}

/* ---- the clock: audio in the ISR's 256-frame blocks, the main loop every millisecond */
#define SIM_MAXN 1024u
static float out_l[SIM_MAXN], out_r[SIM_MAXN];
static int32_t blk[2u * HALF_FRAMES];
static uint32_t blk_pos, ms_acc;                /* (the first block is silence: the ISR's first half goes out before
                                                   anything was rendered; so the passes and the blocks take turns as in
                                                   tests/host.c, and the sound is its WAV's, 256 frames later) */
static int32_t master_q12 = 4096;               /* the MASTER knob (platform.c: (k10 * k10) >> 8), unity at first */
static int32_t peak;

EXPORT int sim_init(uint32_t rate)              /* 44100 only (the engine's tables) */
{
    if (rate != FS || booted) return booted ? 0 : -1;
    sim_pass();
    master_q12 = (smk_master * smk_master) >> 8;
    return 0;
}
EXPORT uint32_t sim_rate(void) { return FS; }
EXPORT float *sim_out_l(void) { return out_l; }
EXPORT float *sim_out_r(void) { return out_r; }
EXPORT void sim_render(uint32_t n)              /* n frames (<= 1024) into sim_out_l / sim_out_r, -1..1 */
{
    uint32_t i, j;
    if (!booted) return;
    for (i = 0; i < n && i < SIM_MAXN; i++) {
        if (blk_pos >= HALF_FRAMES) {           /* the ISR: a block, the MASTER knob, the device's clip */
            master_q12=(smk_master*smk_master)>>8;
            memset(blk, 0, sizeof blk);
            app_audio(blk, HALF_FRAMES);
            for (j = 0; j < 2u * HALF_FRAMES; j++) {
                int32_t v = (blk[j] * master_q12) >> 12;
                blk[j] = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
                if (blk[j] > peak) peak = blk[j];
                else if (-blk[j] > peak) peak = -blk[j];
            }
            blk_pos = 0;
        }
        out_l[i] = (float)blk[2u * blk_pos] * (1.0f / 32768.0f);
        out_r[i] = (float)blk[2u * blk_pos + 1u] * (1.0f / 32768.0f);
        blk_pos++;
        if ((ms_acc += 1000u) >= FS) {           /* a millisecond: the clock, then a pass of the main loop */
            ms_acc -= FS;
            fm1_ms++;
            fm1_in.frames++; /* synthetic input scan: allow ADC settling checks */
            sim_pass();
        }
    }
}
EXPORT uint32_t sim_ms(void) { return fm1_ms; }
EXPORT int32_t sim_peak(void) { int32_t p = peak; peak = 0; return p; }   /* the loudest sample since the last call (Q15) */
EXPORT void sim_master(uint32_t k10)            /* the MASTER knob, 0..1023 (the device's curve); 1024: unity, as the
                                                   host build's WAV (tests/sim_test.mjs compares the two) */
{
    if (k10 > 1024u) k10 = 1024u;
    smk_master = k10 > 1023u ? 1023 : (int32_t)k10;
    master_q12 = (int32_t)((k10 * k10) >> 8);
}

/* ---- the panel */
static uint32_t key_velocity=100u;
EXPORT void sim_key(uint32_t k, uint32_t down) {
    if(k>=37u)return;
    fm1_in.key_vel[k]=(uint8_t)key_velocity;
    if(down)fm1_in.keys|=(uint64_t)1<<k; else fm1_in.keys&=~((uint64_t)1<<k);
    if(k>=5u&&k<=31u){if(down)fm1_in.notes|=1u<<(k-5u);else fm1_in.notes&=~(1u<<(k-5u));}
    else host_qput(1,(int32_t)(k<<8|(down?key_velocity:0u)));
}
EXPORT void sim_velocity(uint32_t v) {key_velocity=v<1u?1u:v>127u?127u:v;}
EXPORT void sim_pad(uint32_t p,uint32_t velocity) { if(p<16u)host_qput(0,(int32_t)(p<<8|(velocity&127u))); }
EXPORT void sim_fader(uint32_t f,uint32_t v) { if(f<4u)fm1_in.fader[f]=(uint8_t)(v&127u); }
EXPORT void sim_wheel(uint32_t w,uint32_t v) { if(w<2u)fm1_in.wheel[w]=(uint16_t)v; }
EXPORT uint32_t sim_pad_rgb(uint32_t p) { return p<16u?padled_rgb[p]:0u; }
/* (CANAIS MIDI) what the firmware sends out over USB MIDI: a running count and a ring of the last 4096
 * messages (status | data1 << 8 | data2 << 16), and the channels lit on the CANAIS pad bank */
EXPORT uint32_t sim_midi_out_n(void) { return host_mo_n; }
EXPORT uint32_t sim_midi_out(uint32_t i) { return host_mo[i % 4096u]; }
EXPORT uint32_t sim_mch_mask(void) { return mch_mask; }
EXPORT uint32_t sim_track(void) {return seq_trk;}
EXPORT uint32_t sim_bank(void) {return pf_bank();}
EXPORT uint32_t sim_favourite(void) {return perf_header_slot();}
EXPORT uint32_t sim_favourites_used(void) {return pf_store.used;}
EXPORT uint32_t sim_pending(void) {return pf_pending;}
EXPORT uint32_t sim_program(uint32_t t) {return t<NTRK?trk_prog[t]:0u;}
EXPORT uint32_t sim_chorus(uint32_t t) {return t<NTRK?set.mix_cho[t]:0u;}
EXPORT uint32_t sim_delay(uint32_t t) {return t<NTRK?set.mix_dly[t]:0u;}
EXPORT uint32_t sim_reverb(uint32_t t) {return t<NTRK?set.mix_rev[t]:0u;}
EXPORT void sim_midi(uint32_t st,uint32_t d1,uint32_t d2);
EXPORT void sim_panic(void) {
    for(uint32_t k=0;k<37u;k++)sim_key(k,0);
    for(uint32_t p=0;p<16u;p++)sim_pad(p,0);
    for(uint32_t b=0;b<18u;b++){fm1_in.buttons&=~(1u<<b);host_rel|=1u<<b;}
    for(uint32_t ch=0;ch<16u;ch++)sim_midi(0xb0u|ch,123u,0);
}
EXPORT void sim_button(uint32_t b, uint32_t down)   /* BTN_* (platform.c): 0 OCT- 1 OCT+ 2 FX 3 SEL 4 ENV 5 LFO 6 EDIT
                                                       7 GLO 8 HOME 9 SAVE 10 ARP 11 SEQ 12 PLAY 13 REC */
{
    if (b >= 18u) return;
    if (down) {
        fm1_in.buttons |= 1u << b;
        host_edges |= 1u << b;
    } else {
        fm1_in.buttons &= ~(1u << b);
        if (host_edges >> b & 1u) host_rel_next |= 1u << b;   /* pressed and let go before the app saw it: a tap */
        else host_rel |= 1u << b;
    }
}
EXPORT void sim_enc(uint32_t e, int32_t steps)  /* ENC_*: 0-3 SCREEN 1-4, 4 SELECT, 5 TRACK, 6 PRESET, 7 MASTER; + = clockwise */
{
    if(e==ENC_MASTER){smk_master=clampi(smk_master+steps*24,0,1023);master_q12=(smk_master*smk_master)>>8;}
    else if (e < (uint32_t)HOST_NENC) { host_enc[e] += steps; host_enc_pos[e] += steps; }
}
EXPORT void sim_midi(uint32_t st, uint32_t d1, uint32_t d2)   /* a MIDI message into the app's USB-MIDI in */
{
    if (st < 0x80u || st == 0xF0u || st == 0xF7u || st == 0xFEu || mq_w - mq_r >= 64u) return;
    mq[mq_w % 64u][0] = (uint8_t)st; mq[mq_w % 64u][1] = (uint8_t)(d1 & 0x7Fu); mq[mq_w % 64u][2] = (uint8_t)(d2 & 0x7Fu);
    mq_w++;
}
EXPORT void sim_note(uint32_t note, uint32_t vel)   /* a note on MIDI channel 1 (vel 0 = off) */
{
    sim_midi(vel ? 0x90u : 0x80u, note, vel);
}

/* ---- what the panel shows */
EXPORT uint16_t *sim_fb(void) { return fb; }    /* 240 x 240 RGB565, row by row */
EXPORT uint32_t sim_fb_hash(void)               /* changes when the screen does */
{
    uint32_t h = 2166136261u, i;
    const uint32_t *p = (const uint32_t *)fb;
    for (i = 0; i < 240u * 240u / 2u; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
EXPORT uint32_t sim_leds_buttons(void) { return (uint32_t)host_leds; }       /* bit BTN_* lit */
EXPORT uint32_t sim_leds_keys(void) { return (uint32_t)(host_leds >> 14) & 0x7FFFFFFu; }  /* bit k: key k lit */
EXPORT uint32_t sim_glow_keys(void) { return (uint32_t)(host_glow >> 14) & 0x7FFFFFFu; }  /* the dim layer */
EXPORT uint32_t sim_page(void) { return page; }
EXPORT uint32_t sim_prog(void) { return trk_prog[0]; }   /* (track A's program) */

/* ---- storage: the settings sector (globals) and the 8 pattern slots (3.0: four tracks with their sounds), in RAM. The
 * page may keep them (localStorage) and put them back before sim_init: blob 0 = settings, 1..8 = patterns 1..8, byte
 * 0 = 1 if used, all HOST_BLOB_SZ long */
EXPORT uint32_t sim_blob_size(void) { return HOST_BLOB_SZ; }
static uint8_t perf_blob[2][HOST_BLOB_SZ];
EXPORT uint32_t sim_blob_count(void) {return NBLOB+3u;}
EXPORT uint8_t *sim_blob(uint32_t i) {return i==0u?host_store:i<=NBLOB?host_slot[i-1u]:i<NBLOB+3u?perf_blob[i-(NBLOB+1u)]:0;}
EXPORT void sim_restore(void) {
    for(uint32_t i=0;i<2u;i++){uint32_t n;memcpy(&n,perf_blob[i],4);if(n<=4096u){host_perf_len[i]=n;memcpy(host_perf[i],perf_blob[i]+4,n);}}
}
EXPORT uint32_t sim_store_hash(void)            /* changes when a save happens */
{
    uint32_t h = 2166136261u, i, b;
    for(b=0;b<2u;b++){memcpy(perf_blob[b],&host_perf_len[b],4);memcpy(perf_blob[b]+4,host_perf[b],4096);}
    for (b = 0; b < NBLOB+3u; b++) {
        const uint8_t *p = sim_blob(b);
        for (i = 0; i < HOST_BLOB_SZ; i++) h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

EXPORT uint32_t sim_pattern(void){return set.pat;}
EXPORT uint32_t sim_patterns_used(void){return pat_used;}
EXPORT uint32_t sim_pattern_queue(void){return pat_queue;}
EXPORT uint32_t sim_pattern_pending(void){return pb_write_target;}
EXPORT uint32_t sim_pattern_confirm(void){return pb_confirm;}

EXPORT uint32_t sim_writes(uint32_t kind){return kind<4u?host_write_count[kind]:0u;}

EXPORT uint32_t sim_fx(uint32_t i){return i<NFXP?set.fx[i]:0u;}
EXPORT uint32_t sim_manual_pending(void){return manual_store_request;}

EXPORT int32_t sim_master_current(void) {return smk_master;}

EXPORT uint32_t sim_tempo(void){return set.tempo;}
EXPORT uint32_t sim_param(uint32_t t,uint32_t p){return t<NTRK&&p<NPARAM?live_p[t].v[p]:0;}

/* Experimental API: the same actions as the contextual ENGINE page. */
EXPORT void sim_analog(uint32_t t){if(t<NTRK){obv_analog_choose(t);dirty_hdr=dirty_page=dirty_tags=1;}}
EXPORT uint32_t sim_engine(uint32_t t){return t<NTRK?obv_engine_track[t].active:0;}
EXPORT uint32_t sim_engine_patch(uint32_t t){return t<NTRK?obv_patch_engine(&live_p[t]):0;}

/* DRUMS uses the same event queue as MIDI; no UI-side DSP. */
EXPORT void sim_drums_preset(uint32_t t,uint32_t n){if(t<NTRK){obv_drums_choose(t,n);dirty_hdr=dirty_page=dirty_tags=1;}}
EXPORT void sim_drum_hit(uint32_t t,uint32_t l,uint32_t v){if(t<NTRK&&l<8u){note_event_p(t,1,DRUM_NOTE[l],v);note_event_p(t,0,DRUM_NOTE[l],0);}}
EXPORT uint32_t sim_drums_pad_mode(void){return smk_drums_hits();}
EXPORT uint32_t sim_drum_busy(uint32_t t){return t<NTRK?drums_busy(t):0;}
EXPORT uint32_t sim_melodic_busy(void){uint32_t n=0;for(unsigned i=0;i<NV;i++)n+=vc[i].gate?1u:0u;return n;}

EXPORT uint32_t sim_drum_grid(void){return pf_bank()==1u&&drum_grid_shown();}
EXPORT uint32_t sim_drum_record(void){return obv_drums_selected()&&rec&&(pf_bank()==4u||(pf_bank()==1u&&drum_grid_shown()));}
EXPORT uint32_t sim_drum_lane(void){return drum_ui_lane[seq_trk];}
EXPORT uint32_t sim_drum_mask(uint32_t t,uint32_t n){return t<NTRK&&n<SEQ_STEPS?drum_pattern_mask(&pat,t,n):0;}
EXPORT uint32_t sim_seq_pos(uint32_t t){return t<NTRK?ui_seq_pos[t]:0;}
EXPORT uint32_t sim_seq_view(void){return seq_view;}

EXPORT uint32_t sim_drum_step_bits(void){uint32_t bits=0,t=seq_trk;for(unsigned i=0;i<16;i++){uint32_t n=seq_pg[t]*16u+i;if(n<pat.trk[t].len&&(drum_pattern_mask(&pat,t,n)>>drum_ui_lane[t]&1u))bits|=1u<<i;}return bits;}

EXPORT int32_t sim_seq_transpose(void){return ctl_seq_transpose;}
EXPORT uint32_t sim_transpose_keys(void){return transpose_keys;}

EXPORT uint32_t sim_keyboard_mode(void){return keyboard_live(&pat)->mode;}
EXPORT uint32_t sim_keyboard_active(void){return keyboard_active();}
EXPORT uint32_t sim_keyboard_part(uint32_t i){return keyboard_live(&pat)->part[i%2];}
EXPORT int32_t sim_keyboard_octave(uint32_t i){return keyboard_live(&pat)->octave[i%2];}
EXPORT uint32_t sim_keyboard_level(uint32_t i){return keyboard_live(&pat)->level[i%2];}
EXPORT uint32_t sim_keyboard_split(void){return keyboard_live(&pat)->split;}
EXPORT uint32_t sim_keyboard_page(void){return page==PG_KEYBOARD;}
EXPORT uint32_t sim_gates_part(uint32_t p,uint32_t seq){uint32_t n=0;for(unsigned i=0;i<NV;i++)n+=vc[i].gate&&vc[i].part==p&&vc[i].seq_owned==seq;return n;}
EXPORT uint32_t sim_step_note(uint32_t p,uint32_t n,uint32_t i){return pat.st[p%4][n%256].note[i%4];}

EXPORT uint32_t sim_keyboard_row(void){return pat.keyboard_selected;}
EXPORT uint32_t sim_keyboard_edit_row(void){return kb_edit_row;}

EXPORT uint32_t sim_preset_save_state(void){return preset_ui;}
EXPORT uint32_t sim_preset_save_slot(void){return save_to+1u;}
EXPORT uint32_t sim_preset_save_part(void){return preset_part;}

EXPORT uint32_t sim_lcd_fills(void){return host_lcd_fills;}
EXPORT uint32_t sim_lcd_blits(void){return host_lcd_blits;}
EXPORT uint32_t sim_lcd_pixels(void){return host_lcd_pixels;}

EXPORT int32_t sim_part_octave(uint32_t t){return patch_keyboard_octave(&live_p[t%4]);}


EXPORT uint32_t sim_fader_jump(void){return !!smk_fader_jump();}
EXPORT uint32_t sim_fader_caught(uint32_t i){return i<4u?smk_fader_owner[i].picked:0u;}
