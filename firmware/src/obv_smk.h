/* SPDX-License-Identifier: GPL-3.0-only
 * SMK-37 port of OberVave: Copyright (C) 2026 zvenson */
/* OberVave on the M-Vave SMK-37 (BOARD_SMK37; SMK37.md, smk37-port.yaml). The FM-1's panel keeps its jobs (the
 * middle 27 keys, the 14 buttons by printed name, the knobs: platform.c); this file adds what the SMK has more:
 *
 *   pads        the step sequencer: the 16 steps of the selected track's page, in reading order (top row 1-8,
 *               bottom 9-16). A pad is the step's white key in the SEQ layer: pressed on an empty step it sets it at
 *               once with the pen chord (the last notes played) at the pad's velocity; a set step pressed and let go
 *               is cleared; held, the knobs edit it (NOTE LEVEL RATCHET GATE) and a later pad makes a long note. While
 *               REC is on (armed or recording) the pads play the pen chord live at their velocity, as the keys play.
 *               Lit: set steps in the track's colour, the playhead bright, the held step red, beats 1 5 9 13 glow.
 *   < >         the step page of the selected track (1-8, as OCT- / OCT+ in the SEQ layer and the black keys)
 *   outer keys  C3-E3 and G#5-C6 extend the keyboard (notes below F3 and above G5), with velocity, through the
 *               same path as the keys (EDIT held: they erase their note)
 *   keys        velocity from the unit's factory curves (hal/smk_input.h); the FM-1 plays 100
 *   faders      two banks, FADER BANK switches (LED lit on B): A = PERFORM's CUTOFF RESO ENV AMT RELEASE,
 *               B = CHORUS / DELAY / REVERB sends for the selected track, and MASTER (K8). A fader acts once it moves (also
 *               after a bank change), so nothing jumps; a short bar under the page shows it
 *   wheels      pitch = bend, +-2 semitones (the program's BEND range, 2 in every factory program), every voice;
 *               mod = a gentle vibrato from the program's LFO, up to +-1/2 semitone (obv_engine.h ctl_vib).
 *               Self-calibrating (smk_wheel.h)
 *   KNOB BANK   held: the panel check (a bring-up screen in place of a serial console: the flash part, the factory
 *               calibration, the ADC, faders, wheels, keys, buttons and every pad's reading above its baseline)
 */
#include "smk_wheel.h"

static const uint8_t SMK_WK[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* step -> its white key */
#define SMK_TRK_A RGB(96, 176, 255)             /* track A: the screen's light blue accent (C_AMBER) */
#define SMK_TRK_B RGB(226, 232, 240)            /* track B: the screen's legends (C_CREAM) */

static uint32_t smk_bank;                       /* FADER BANK: 0 = A (PERFORM), 1 = B (effects, MASTER) */
static uint8_t smk_fad_last[4] = {0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t smk_live[16][SEQ_CHORD];         /* the notes a pad plays live (REC on): its release ends them */
static uint16_t smk_live_mask[16];              /* (CANAIS MIDI) the channels a pad's live notes went out on */
static uint8_t smk_ext_note[FM1_NKEY], smk_ext_erase[FM1_NKEY];   /* an outer key's note; 1: it erases (EDIT held) */
static uint32_t smk_view_prev;
static const char *smk_ov1;                     /* the overlay under the page (faders, banks, step pages), 1.5 s */
static char smk_ov2[40];
static uint32_t smk_ov_t, smk_ov_on, smk_ov_dirty;

static void smk_overlay(const char *l1, const char *l2)
{
    uint32_t i;
    smk_ov1 = l1;
    for (i = 0; l2[i] && i + 1u < sizeof smk_ov2; i++) smk_ov2[i] = l2[i];
    smk_ov2[i] = 0;
    smk_ov_t = fm1_ms;
    smk_ov_on = smk_ov_dirty = 1;
}

/* in the screen's 40 ms slot: 1 while the overlay holds the hints / voice-LED strip (it redraws them when it ends) */
static int smk_overlay_draw(uint32_t *leds_q)
{
    if (!smk_ov_on) return 0;
    if (fm1_ms - smk_ov_t >= 1500u) {
        smk_ov_on = 0;
        hints_key = 0xFFFFFFFFu;
        *leds_q = 0xFFFFFFFFu;
        return 0;
    }
    if (smk_ov_dirty) { smk_ov_dirty = 0; ui_hints(smk_ov1, smk_ov2, C_AMBER); }
    return 1;
}

static void smk_bar(char *s, uint32_t v, uint32_t max)   /* [#######---------] */
{
    uint32_t k, f = max ? (v * 16u + max / 2u) / max : 0u;
    s[0] = 0;
    scat(s, "[");
    for (k = 0; k < 16u; k++) scat(s, k < f ? "#" : "-");
    scat(s, "]");
}

/* Outer notes are also serviced during LCD drawing; pad editing remains in the main loop.
 * Adapted from genkigenki's PR snapshot e9dc9a7; retain 3.0.1 note_in and MIDI_OUT_CH. */
static void smk_outer(uint32_t base, uint32_t notes_only, uint32_t *dirty);
static int32_t smk_ext_back = -1;

#include "obv_perf.h"

/* ---- the pads and the outer keys (main loop, after the keys) */
static void smk_seq_pad_event(int32_t e,uint32_t *dirty)
{
        uint32_t p = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0xFFu, k = SMK_WK[p], q;
        if (vel) {
            if (rec) {                              /* REC on: the pad plays the pen chord, recorded as the keys are */
                smk_live_mask[p] = mch_out_mask();
                for (q = 0; q < SEQ_CHORD; q++) {
                    uint32_t n = q < (pen_n ? pen_n : 1u) ? (pen_n ? pen_chord[q] : pen) : 0u;
                    smk_live[p][q] = (uint8_t)n;
                    if (!n) continue;
                    play_note(1, n, vel); mch_send(1, smk_live_mask[p], n, vel);
                    if (rec_keys()) rec_note(1, n, vel);
                }
            } else {                                /* a step: as its white key in the SEQ layer */
                smk_pads_held |= 1u << p;
                if (!(smk_pads_held & ~(1u << p))) smk_pad_t0 = fm1_ms;
                smk_step_vel = vel;
                seq_key_down(k);
                smk_step_vel = 0;
            }
        } else if (smk_pads_held >> p & 1u) {
            smk_pads_held &= ~(1u << p);
            seq_key_up(k);
            if (!smk_pads_held && step_held >= 0 && layer != L_SEQ) {   /* (the page moved under the pad: let go) */
                step_held = -1;
                audition_off();
            }
        } else {
            for (q = 0; q < SEQ_CHORD; q++) {
                uint32_t n = smk_live[p][q];
                smk_live[p][q] = 0;
                if (!n) continue;
                play_note(0, n, 0); mch_send(0, smk_live_mask[p], n, 0);
                if (rec_keys()) rec_note(0, n, 0);
                for (uint32_t u = 0; u < NTRK; u++) ctl_live_hold[u][n >> 5 & 3u] &= ~(1u << (n & 31u));   /* (let go: the sequencer's) */
            }
            smk_live_mask[p] = 0;
        }
        *dirty = 1;
}

static uint8_t drum_pad_part[8],drum_pad_channel[8],drum_pad_down[8];
static uint16_t drum_pad_mask[8];               /* (CANAIS MIDI) the channels a drum pad's hit went out on */
static int smk_drums_hits(void){return obv_drums_selected()&&(pf_bank()==4u||(!layer&&pf_bank()==1u&&drum_grid_shown()&&rec));}
/* Same audio-owned timestamps as ui_drum_hits: reflects live, MIDI and sequence hits. */
static uint16_t drum_pad_led(uint32_t p){
 if(p>=8u||!obv_drums_selected())return 0;
 uint32_t t=seq_trk%NTRK;
 uint32_t hit=(drum_pad_down[p]&&drum_pad_part[p]==t)||(drum_stamp[t][p]&&eng_t+1u-drum_stamp[t][p]<FS/10u);
 /* Violet at 70% of the requested RGB level; bright white during each hit. */
 return hit?RGB(255,255,255):smk_dim(RGB(166,66,255),179u);
}
/* ---- (CANAIS MIDI) the fifth pad bank: pad n = MIDI channel n (top row 1-8, bottom 9-16), lit = sending.
 * A tap toggles it; a ninth is refused (the pad flashes red). Switching a channel off ends the notes held on it. */
static uint32_t mch_refused_p, mch_refused_t;
static void mch_overlay(void)
{
    static char l2[40];
    uint32_t c, n = 0;
    l2[0] = 0;
    for (c = 0; c < 16u; c++) if (mch_mask >> c & 1u) { char b[4]; if (n++) scat(l2, " "); b[0] = 0; fmt_u(b, c + 1u); scat(l2, b); }
    if (!n) { scat(l2, "NENHUM: CANAL "); { char b[4]; b[0] = 0; fmt_u(b, MIDI_OUT_CH + 1u); scat(l2, b); } scat(l2, " DA PARTE"); }
    smk_overlay("CANAIS MIDI", l2);
}
static void mch_release(uint32_t c)               /* channel c switched off: its held notes end now, there only */
{
    uint32_t r, p, q;
    uint16_t bit = (uint16_t)(1u << c);
    for (r = 0; r < 128u; r++) if (panel_tx_mask[r] & bit) { midi_send(0x80u | c, panel_tx_note[r], 0); panel_tx_mask[r] &= (uint16_t)~bit; }
    for (p = 0; p < 16u; p++) if (smk_live_mask[p] & bit) {
        for (q = 0; q < SEQ_CHORD; q++) if (smk_live[p][q]) midi_send(0x80u | c, smk_live[p][q], 0);
        smk_live_mask[p] &= (uint16_t)~bit;
    }
    for (p = 0; p < 8u; p++) if (drum_pad_mask[p] & bit) { midi_send(0x80u | c, DRUM_NOTE[p], 0); drum_pad_mask[p] &= (uint16_t)~bit; }
}
static void mch_pad(uint32_t p)
{
    uint16_t bit = (uint16_t)(1u << (p & 15u));
    if (mch_mask & bit) { mch_mask &= (uint16_t)~bit; mch_release(p & 15u); }
    else if (mch_count(mch_mask) >= MCH_MAX) { mch_refused_p = p; mch_refused_t = fm1_ms; smk_overlay("CANAIS MIDI", "MAXIMO 8 CANAIS"); return; }
    else mch_mask |= bit;
    mch_overlay();
}
static uint16_t mch_led(uint32_t p)
{
    if (mch_refused_t && mch_refused_p == p && fm1_ms - mch_refused_t < 400u) return C_RED;
    if (mch_mask >> p & 1u) return RGB(60, 230, 110);
    return smk_dim(C_CREAM, mch_mask ? 10u : (p == MIDI_OUT_CH ? 60u : 10u));   /* none lit: the part's channel glows */
}

static void smk_pads(uint32_t base, uint32_t *dirty)
{
    int32_t e;
    uint32_t v;
    while ((e = smk_pad_take()) >= 0) {
        uint32_t p=((uint32_t)e>>8)&15u,vel=(uint32_t)e&255u;
        if(preset_ui)continue;
        if(vel)pf_role[p]=pf_bank()==5u?8u:pf_bank()==4u?(obv_drums_selected()&&p<8u?4u:6u):pf_bank()==3u?7u:obv_drums_selected()&&pf_bank()==1u&&!rec&&!layer?5u:(pf_bank()==1u&&drum_grid_shown())?(rec?(p<8u?4u:6u):5u):pf_bank()==1u?1u:pf_bank()==2u?3u:2u;
        if(pf_role[p]==8u){if(vel)mch_pad(p);*dirty=1;}
        else if(pf_role[p]==7u){keyboard_pad(p,vel);*dirty=1;}
        else if(pf_role[p]==1u)smk_seq_pad_event(e,dirty);
        else if(pf_role[p]==2u)pf_event(p,vel);
        else if(pf_role[p]==3u)pb_event(p,vel);
        else if(pf_role[p]==5u){if(vel){drum_grid_pad(p,vel);dirty_steps=dirty_page=1;}*dirty=1;}
        else if(pf_role[p]==4u){
            if(vel){drum_pad_part[p]=(uint8_t)seq_trk;drum_pad_channel[p]=(uint8_t)MIDI_OUT_CH;drum_pad_mask[p]=mch_out_mask();drum_pad_down[p]=1;drum_ui_lane[seq_trk]=(uint8_t)p;dirty_page=1;note_event_p(seq_trk,1,DRUM_NOTE[p],vel);rec_note_at(seq_trk,1,DRUM_NOTE[p],vel);mch_send(1,drum_pad_mask[p],DRUM_NOTE[p],vel);}
            else {uint32_t t=drum_pad_part[p],n=DRUM_NOTE[p];drum_pad_down[p]=0;
                note_event_p(t,0,n,0);rec_note_at(t,0,n,0);ctl_live_hold[t][n>>5]&=~(1u<<(n&31u));
                mch_send(0,drum_pad_mask[p],n,0);drum_pad_mask[p]=0;}
            *dirty=1;
        }
        if(!vel)pf_role[p]=0;
    }
    pf_hold_poll();
    smk_outer(base, 0, dirty);
    v = SMK_PAD_HELD;                           /* the step view comes and goes with a held pad */
    if (v != smk_view_prev) { smk_view_prev = v; *dirty = 1; hints_key = 0xFFFFFFFFu; }
}

static void smk_outer(uint32_t base, uint32_t notes_only, uint32_t *dirty)
{
    int32_t e;
    for (;;) {
        uint32_t kk, vel;
        if (smk_ext_back >= 0) { e = smk_ext_back; smk_ext_back = -1; }
        else if ((e = smk_extkey_take()) < 0) break;
        kk = ((uint32_t)e >> 8) % FM1_NKEY; vel = (uint32_t)e & 0xFFu;
        /* EDIT/undo belongs to the main loop. Preserve FIFO order behind the deferred event. */
        if (notes_only && (vel ? layer == L_EDIT : smk_ext_note[kk] && smk_ext_erase[kk])) {
            smk_ext_back = e;
            break;
        }
        if (vel) {
            uint32_t n = (uint32_t)clampi((int32_t)base + (int32_t)kk - (int32_t)SMK_KEY_F3, 1, 127);
            smk_ext_note[kk] = (uint8_t)n;
            smk_ext_erase[kk] = layer == L_EDIT;
            if (smk_ext_erase[kk]) {
                if (!layer_used) undo_save();
                layer_used = 1;
                erase_note(n);
                say("ERASED");
            } else {
                panel_midi_send(1,n,vel);
                panel_note_in(1, n, vel);
            }
        } else if (smk_ext_note[kk]) {
            uint32_t n = smk_ext_note[kk];
            smk_ext_note[kk] = 0;
            if (smk_ext_erase[kk]) erase_on[(uint32_t)clampi((int32_t)n - (obv_drums_selected()?0:pat.trk[seq_trk].transp), 1, 127)] = 0;
            else {
                panel_midi_send(0,n,0);
                panel_note_in(0, n, 0);
            }
        }
        if (notes_shown()) *dirty = 1;          /* (a note alone changes no knob page) */
    }
}

/* ---- FADER BANK, < > (main loop, with the other buttons) */
static void smk_buttons(uint32_t b, uint32_t *dirty)
{
    static char msg[24];
    uint32_t t = seq_trk, pg = seq_pg[t];
    if (b & (1u << BTN_FADER_BANK)) {
        uint32_t i;
        smk_bank ^= 1u;
        for (i = 0; i < 4u; i++) smk_fad_last[i] = 0xFF;   /* a new bank: each fader waits until it moves */
        smk_overlay(smk_bank ? "FADER BANK B" : "FADER BANK A",
                    smk_bank ? "CHO/DLY/REV SEND + MASTER" : (obv_drums_selected()?"MORPH TONE DECAY FOLD":"CUTOFF RESO ENV AMT RELEASE"));
    }
    if (b & ((1u << BTN_LEFT) | (1u << BTN_RIGHT))) {
        if (b & (1u << BTN_LEFT) && seq_pg[t]) seq_pg[t]--;
        if (b & (1u << BTN_RIGHT) && seq_pg[t] + 1u < seq_pages(t)) seq_pg[t]++;
        msg[0] = 0; scat(msg, "PAGE "); fmt_u(msg + slen(msg), seq_pg[t] + 1u); scat(msg, " OF "); fmt_u(msg + slen(msg), seq_pages(t));
        smk_overlay(msg, seq_pg[t] == pg ? (b & (1u << BTN_LEFT) ? "THE FIRST PAGE" : "THE LAST PAGE: LENGTH ADDS MORE")
                                         : t == 0u ? "TRACK A ON THE PADS" : t == 1u ? "TRACK B ON THE PADS" : t == 2u ? "TRACK C ON THE PADS" : "TRACK D ON THE PADS");
        *dirty = 1;
    }
}

/* Fader ownership includes identical-value recalls and UI/part/bank changes.
 * The four physical faders retain fixed destinations; they never follow Attack
 * or the currently displayed parameter. K1..K8 remain relative encoders. */
static uint32_t smk_fader_context(void)
{
    uint32_t h = smk_controls_epoch;
#define SMK_CTX(v) h = h * 33u + (uint32_t)(v)
    SMK_CTX(page); SMK_CTX(layer); SMK_CTX(seq_trk); SMK_CTX(smk_bank);
    SMK_CTX(mix_row); SMK_CTX(seq_view);
    SMK_CTX(pf_bank()); SMK_CTX(obv_patch_engine(&live_p[seq_trk%NTRK]));
    SMK_CTX(keyboard_pattern_epoch); SMK_CTX(ui_pat_swapped);
    SMK_CTX(preset_ui); SMK_CTX(pb_confirm); SMK_CTX(pat.keyboard_selected); SMK_CTX(kb_edit_row);
#undef SMK_CTX
    return h;
}
static uint32_t smk_control_context(void)
{
    return (smk_fader_context()*33u+(uint32_t)step_held)*33u+seq_cur[seq_trk%NTRK];
}
typedef struct { uint32_t context, value; uint8_t seen, picked, jump; } smk_fader_owner_t;
static smk_fader_owner_t smk_fader_owner[4];
static uint32_t smk_fader_value(unsigned i, unsigned p)
{
    unsigned t=seq_trk%NTRK;
    return !smk_bank ? ed_patch(p)->v[p] : i==0 ? set.mix_cho[t] : i==1 ? set.mix_dly[t] : i==2 ? set.mix_rev[t] : (uint32_t)smk_master;
}
static void smk_faders(uint32_t *dirty_page, uint32_t *dirty_hdr, uint32_t *dirty_tags)
{
    static const uint8_t FP[4]={P_CUTOFF,P_RESO,P_FENV_AMT,P_AR};
    static const char *const FN[2][4]={{"CUTOFF","RESO","ENV AMT","RELEASE"},{"CHO SEND","DLY SEND","REV SEND","MASTER"}};
    static char l1[40],l2[32];
    uint32_t t=seq_trk%NTRK,context=smk_fader_context(),jump=!!smk_fader_jump();
    for(unsigned i=0;i<4;i++) {
        unsigned p=obv_drums_selected()&&i==3u?DP_FOLD:FP[i];
        unsigned max=!smk_bank ? (unsigned)obv_param_max(ed_patch(p),p) : i==3 ? 1023u : 127u;
        unsigned value=smk_fader_value(i,p),v=fm1_in.fader[i],prev=smk_fad_last[i];
        unsigned target=(value*127u+max/2u)/max,waiting;
        smk_fader_owner_t *o=&smk_fader_owner[i];
        /* Seed the physical position, never use the former context's position
         * to claim a crossing against a freshly recalled target. */
        if(!o->seen||o->context!=context||o->value!=value||o->jump!=jump||prev==255u||fm1_in.frames<200u||preset_ui||pb_confirm) {
            o->seen=1;o->context=context;o->value=value;o->jump=(uint8_t)jump;o->picked=0;
            smk_fad_last[i]=(uint8_t)v;continue;
        }
        if(v==prev)continue;
        smk_fad_last[i]=(uint8_t)v;
        if(!o->picked&&((prev<=target&&v>=target)||(prev>=target&&v<=target)))o->picked=1;
        waiting=!jump&&!o->picked;
        char val[12];
        if(!waiting) {
#ifndef OBV_HOST
            uint32_t irq = fm1__lock();
#endif
            if(smk_fader_context()!=context){
                o->seen=0;
#ifndef OBV_HOST
                fm1__unlock(irq);
#endif
                continue;
            }
            if(!smk_bank) {
                set_param(p,(int32_t)((v*max+63u)/127u));
                if(p==P_CUTOFF)cut_shown_t=fm1_ms+1u;
                *dirty_page=*dirty_hdr=1;
            } else if(i<3u) {
                uint8_t *send=i==0?set.mix_cho:i==1?set.mix_dly:set.mix_rev;
                send[t]=(uint8_t)v;
                if(i&&v){pf_store.last_send[i-1][t]=(uint8_t)v;pf_changed();}
                mix_apply();set_dirty=1;set_t=fm1_ms;
                *dirty_page=*dirty_hdr=*dirty_tags=1;
            } else smk_master=(int32_t)(v*1023u/127u);
            o->value=smk_fader_value(i,p); /* our own write does not disarm pickup */
#ifndef OBV_HOST
            fm1__unlock(irq);
#endif
        }
        if(!smk_bank)param_value(val,p,ed_patch(p)->v[p]);
        else {fmt_u(val,((waiting?target:v)*100u+63u)/127u);scat(val,"%");}
        l1[0]=0;scat(l1,smk_bank?"B":"A");fmt_u(l1+1,i+1u);scat(l1," ");
        scat(l1,(!smk_bank&&obv_drums_selected())?obv_voice_label(p):FN[smk_bank][i]);
        if(smk_bank&&i<3){char tr[3]={' ',TRK_L[t],0};scat(l1,tr);}
        scat(l1,waiting?" CATCH ":" ");scat(l1,val);
        smk_bar(l2,v,127u);smk_overlay(l1,l2);
    }
}

/* ---- the wheels (also between frames: bends follow the wheel, not the screen) */
static void smk_wheels(void)
{
    ctl_wheel_part = seq_trk % NTRK;
    enum { DEAD = 12, MOD_LO = 485, MOD_HI = 615 };
    static smk_pw_t pw;
    static int32_t mlo, mhi, last_b = 0x7FFFFFFF, last_m = -1;
    int32_t m = fm1_in.wheel[1], mw, b;
    if (fm1_in.frames < 300u)                   /* (the readings settle) */
        return;
    if (!mhi) {
        mlo = m < MOD_LO ? m : MOD_LO;
        mhi = m > MOD_HI ? m : MOD_HI;
    }
    if (m < mlo) mlo = m;
    if (m > mhi) mhi = m;
    mw = mhi - m <= DEAD ? 0 : (mhi - m - DEAD) * 127 / (mhi - mlo - DEAD > 1 ? mhi - mlo - DEAD : 1);   /* up = lower raw */
    mw = clampi(mw, 0, 127);
    b = smk_pw_bend(&pw, fm1_in.wheel[0], fm1_ms) * 8192 / (SMK_PW_ST * 4096);   /* -> the MIDI bend's -8192..8191 */
    b = clampi(b, -8192, 8191);
    if (b != last_b) { last_b = b; ctl_wheel_bend = b; }   /* (only on a change: a MIDI bend in still works at rest) */
    if (mw != last_m) {
        last_m = mw; ctl_vib = mw;
    }
}

/* ---- the pads' and the extra buttons' lights */
static uint16_t smk_dim(uint16_t c, uint32_t k)   /* c * k / 256 */
{
    return blend565(0, c, k);
}
static void smk_leds(void)
{
    uint32_t t = seq_trk, len = pat.trk[t].len, w0 = seq_pg[t] * 16u, ph = len ? ui_seq_pos[t] % len : 0u, p;
    uint16_t tc = C_TRKC[t % NTRK];
    for (p = 0; p < 16u; p++) {
        uint32_t n = w0 + p, q, live_on = 0;
        uint16_t c = 0;
        for (q = 0; q < SEQ_CHORD; q++) live_on |= smk_live[p][q];
        if (live_on) c = C_CREAM;               /* playing live (REC) */
        else if (n < len) {
            const step_t *st = seq_step(t, n);
            uint32_t set = st->note[0] && !(st->ratch & STEP_OFF);
            if ((int32_t)n == step_held) c = C_RED;
            else if (ctl_seq_run && n == ph) c = rec == REC_ON ? C_RED : set ? tc : smk_dim(tc, 90);   /* the playhead */
            else if (set) c = smk_dim(tc, st->chance < 100u ? 32 : 56);
            else if (st->note[0]) c = smk_dim(C_DIMT, 16);   /* switched off, notes kept */
            else if (!(p & 3u)) c = smk_dim(tc, t ? 8u : 16u);           /* beats 1 5 9 13 glow */
        }
        if(pf_bank()==0u)c=pf_led(p);
        if(obv_drums_selected()&&pf_bank()==1u&&!rec&&!layer){uint32_t n=seq_pg[t]*16u+p,l=drum_ui_lane[t];
            uint32_t on=n<pat.trk[t].len&&(drum_pattern_mask(&pat,t,n)>>l&1u)&&!(seq_step(t,n)->ratch&STEP_OFF);
            c=smk_dim(C_AMBER,on?150u:12u);if(n==seq_cur[t])c=on?C_RED:smk_dim(C_RED,64);
            if(ctl_seq_run&&n==ui_seq_pos[t])c=C_CREAM;
        }
        else if(pf_bank()==1u&&drum_grid_shown()&&rec&&p>=8u)c=0;
        else if(pf_bank()==4u)c=drum_pad_led(p);
        else if(smk_drums_hits()&&p<8u)c=drum_pad_led(p);
        else if(pf_bank()==2u)c=pb_led(p);
        else if(pf_bank()==3u)c=keyboard_pad_led(p);
        else if(pf_bank()==5u)c=mch_led(p);
        padled_set(p, (uint8_t)((c >> 11) * 255u / 31u), (uint8_t)(((c >> 5) & 63u) * 255u / 63u), (uint8_t)((c & 31u) * 255u / 31u));
    }
    fm1_led_key(BTN_FADER_BANK, smk_bank != 0u);
    fm1_led_key(BTN_KNOB_BANK, pf_bank()!=0u);   /* (lit while the panel check is up) */
    fm1_led_key(BTN_LEFT, seq_pg[t] > 0u);
    fm1_led_key(BTN_RIGHT, seq_pg[t] + 1u < seq_pages(t));
    padled_flush();                             /* (only when a colour changed; between LCD transfers) */
}

/* ---- KNOB BANK held: the panel check (OberVave has no serial console; this is what the kit's "smk" command shows) */
static void smk_hex(char *s, uint32_t v, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) s[k] = "0123456789ABCDEF"[(v >> (4u * (n - 1u - k))) & 15u];
    s[n] = 0;
}
#ifdef OBV_HOST
/* the host test's "dump" (tests/host.c): the pads, the controls, the steps on the pads */
static void host_dump(void)
{
    uint32_t p, t = seq_trk;
    printf("%6u ms  PADS", fm1_ms);
    for (p = 0; p < 16u; p++) printf(" %06X", padled_rgb[p]);
    printf("\n%6u ms  STATE bank=%u bend=%d vib=%d master=%d cutoff=%u reso=%u envamt=%u release=%u cho=%u dly=%u rev=%u"
           " trk=%u seqpg=%u len=%u held=%d view=%u rec=%u run=%u\n", fm1_ms, smk_bank, clampi(ctl_bend + ctl_wheel_bend, -8192, 8191), ctl_vib, smk_master,
           live.v[P_CUTOFF], live.v[P_RESO], live.v[P_FENV_AMT], live.v[P_AR], live.v[P_CHO_MIX], live.v[P_DLY_MIX],
           live.v[P_REV_MIX], t, seq_pg[t], pat.trk[t].len, step_held, seq_shown(), rec, ctl_seq_run);
    printf("%6u ms  STEPS", fm1_ms);
    for (p = 0; p < 16u && seq_pg[t] * 16u + p < pat.trk[t].len; p++) {
        const step_t *st = seq_step(t, seq_pg[t] * 16u + p);
        if (st->note[0]) printf(" %u:%u/%u%s", p + 1u, st->note[0], st->vel, st->note[1] ? "+" : "");
    }
    printf("\n");
}
#endif
