/* SPDX-License-Identifier: GPL-3.0-only */
#define main obv_host_main
#include "../firmware/src/obervave.c"
#undef main
#include <assert.h>
static int32_t audio[HALF_FRAMES*2];
static void render(void){memset(audio,0,sizeof audio);engine_audio(audio,HALF_FRAMES);}
static void tap_bank(void){fm1_ms+=100;fm1_in.buttons=1u<<BTN_KNOB_BANK;pf_bank_poll(fm1_in.buttons,0);fm1_ms+=100;fm1_in.buttons=0;pf_bank_poll(0,1u<<BTN_KNOB_BANK);}
static void pad(unsigned p,unsigned v){uint32_t dirty=0;host_qput(0,(int32_t)(p<<8|v));smk_pads(48,&dirty);}
int main(int argc,char**argv){
 engine_init();pf_init();pat_init(&pat);for(unsigned t=0;t<4;t++){trk_load(t,0);set.mix_lvl[t]=100;}set.m_vol=100;set.m_dj=64;set.tempo=120;fx_sync();mix_apply();seq_trk=0;layer=L_NONE;rec=REC_OFF;
 /* ANALOG: the original three banks, then CANAIS MIDI (5) before PERFORMANCE again. */
 for(unsigned i=0;i<12;i++){tap_bank();assert(pf_bank()==(i%4==0?3u:i%4==1?2u:i%4==2?5u:0u));}
 /* Engine choice and every sound-edit page leave pads in their selected bank. */
 obv_drums_choose(0,0);render();render();assert(pf_bank()==0);page=PG_DSOUND;assert(!smk_drums_hits());pad(1,100);pad(1,0);assert(seq_trk==1&&pf_bank()==0);
 seq_trk=0;tap_bank();assert(pf_bank()==3);tap_bank();assert(pf_bank()==2);tap_bank();assert(pf_bank()==4&&pf_page_request==PG_DSOUND+1u);page=PG_DSOUND;
 uint16_t idle=smk_dim(RGB(166,66,255),179u);assert(drum_pad_led(0)==idle);for(unsigned p=8;p<16;p++)assert(!drum_pad_led(p));
 /* Reserved pads do nothing and do not capture/recall favourites. */
 uint32_t used=pf_store.used;for(unsigned p=8;p<16;p++){pad(p,127);fm1_ms+=900;pf_hold_poll();pad(p,0);}assert(pf_store.used==used&&!pf_manual_write&&pf_bank()==4);
 /* Hits, white holds and releases: no favourite, part or bank side effects. */
 for(unsigned p=0;p<8;p++){pad(p,111);assert(drum_pad_led(p)==RGB(255,255,255));render();assert(drum_stamp[0][p]);assert(drum_ui_lane[0]==p&&drum_pad_down[p]);pad(p,0);assert(!drum_pad_down[p]);}
 for(unsigned i=0;i<35;i++)render();for(unsigned p=0;p<8;p++)assert(drum_pad_led(p)==idle);
 /* Observe actual sequence-triggered timestamps without a held pad. */
 pat_init(&pat);drums_reset(0);pat.st[0][0].note[0]=36;pat.st[0][0].vel=100;ctl_seq_run=1;render();assert(drum_stamp[0][0]&&drum_pad_led(0)==RGB(255,255,255));assert(drum_pad_led(1)==idle);ctl_seq_run=0;
 /* Other track's hit must not illuminate the selected kit. */
 drums_reset(0);obv_drums_choose(2,0);render();render();note_event_p(2,1,38,100);render();seq_trk=0;assert(drum_pad_led(1)==idle);
 /* Record through explicit bank with correct pitch; release targets original track/channel. */
 drums_reset(0);pat_init(&pat);seq_trk=0;set.midi_ch=4;rec=REC_READY;ctl_seq_run=0;fm1_ms+=100;pad(0,98);render();assert(rec!=REC_OFF&&pf_bank()==4&&(drum_pattern_mask(&pat,0,0)&1u));assert(drum_pad_channel[0]==3);
 trk_select(1);assert(pf_bank()==4&&!smk_drums_hits());for(unsigned p=0;p<16;p++)assert(drum_pad_led(p)==0);pad(0,0);assert(!drum_pad_down[0]&&!seq_live(0,36));assert(!seq_live(1,36));
 unsigned original=seq_trk;pad(3,110);pad(3,0);assert(seq_trk==original&&pf_bank()==4&&!drum_pad_down[3]);
 rec=REC_OFF;ctl_seq_run=0;layer=L_NONE;draw_header();draw_page();draw_scope();hints_key=~0u;draw_hints();
 if(argc>1){char p[300];snprintf(p,sizeof p,"%s-inactive.ppm",argv[1]);write_ppm_as(p);}
 tap_bank();assert(pf_bank()==5);tap_bank();assert(pf_bank()==0);seq_trk=0;tap_bank();tap_bank();tap_bank();assert(pf_bank()==4);
 page=PG_DSOUND;for(unsigned i=0;i<35;i++)render();draw_header();draw_page();draw_scope();hints_key=~0u;draw_hints();
 if(argc>1){char p[300];snprintf(p,sizeof p,"%s-active.ppm",argv[1]);write_ppm_as(p);}
 /* Long bank returns directly to performance and preserves transport state. */
 rec=REC_READY;ctl_seq_run=1;fm1_in.buttons=1u<<BTN_KNOB_BANK;pf_bank_poll(fm1_in.buttons,0);fm1_ms+=800;pf_bank_poll(0,0);assert(pf_bank()==0&&rec==REC_READY&&ctl_seq_run);fm1_in.buttons=0;pf_bank_poll(0,1u<<BTN_KNOB_BANK);
 assert(sizeof(patch_t)==80&&sizeof(pattern_t)==14476&&sizeof(psnd_t)==380);
 puts("PASS: conditional bank, persistent ownership/engine/page changes, 8 hits/velocity, reserved lower pads, violet70/white and actual sequence feedback, per-part feedback isolation, REC/original note-off+channel, inactive UI, long escape and unchanged saved layouts.");
 return 0;
}
