/* SPDX-License-Identifier: GPL-3.0-only
 * (CANAIS MIDI) the fifth pad bank: pads 1-16 = MIDI channels 1-16, lit = the keys and pads go out there,
 * at most 8 at once, with every note-off on the channels its note-on used. */
#define main obv_host_main
#include "../firmware/src/obervave.c"
#undef main
#include <assert.h>
static int32_t audio[HALF_FRAMES*2];
static void render(void){memset(audio,0,sizeof audio);engine_audio(audio,HALF_FRAMES);}

static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);return 1;}}while(0)

static void tap_bank(void){fm1_ms+=100;fm1_in.buttons=1u<<BTN_KNOB_BANK;pf_bank_poll(fm1_in.buttons,0);fm1_ms+=100;fm1_in.buttons=0;pf_bank_poll(0,1u<<BTN_KNOB_BANK);}
static void pad(unsigned p,unsigned v){uint32_t dirty=0;host_qput(0,(int32_t)(p<<8|v));smk_pads(48,&dirty);}
static void tap(unsigned p){pad(p,100);fm1_ms+=40;pad(p,0);fm1_ms+=40;}
static void key(unsigned k,unsigned vel){
    if(vel){fm1_in.notes|=1u<<k;fm1_in.key_vel[SMK_KEY_F3+k]=(uint8_t)vel;}else fm1_in.notes&=~(1u<<k);
    keys_in(0);
}
/* the messages sent since the mark: their count of a kind (0x90 / 0x80), and the channels they used */
static unsigned mark;
static void mo_mark(void){mark=host_mo_n;}
static unsigned mo_count(unsigned kind,uint16_t *chans,unsigned note,unsigned *vel){
    unsigned n=0;if(chans)*chans=0;
    for(unsigned i=mark;i<host_mo_n;i++){unsigned st=host_mo[i]&0xFF,d1=host_mo[i]>>8&0x7F,d2=host_mo[i]>>16&0x7F;
        if((st&0xF0)!=kind||d1!=note)continue;n++;if(chans)*chans|=(uint16_t)(1u<<(st&15));if(vel)*vel=d2;}
    return n;
}
#define CH(c) (1u<<((c)-1u))   /* channel c (1-16) as a mask bit */

int main(void){
    host_mo_quiet=1;
    engine_init();pf_init();pat_init(&pat);
    for(unsigned t=0;t<4;t++){trk_load(t,0);set.mix_lvl[t]=100;}
    set.m_vol=100;set.m_dj=64;set.tempo=120;set.midi_ch=0;fx_sync();mix_apply();
    seq_trk=0;layer=L_NONE;rec=REC_OFF;page=PG_PERFORM;keys_prev=fm1_in.notes=0;fm1_ms=3000;
    const unsigned K=7,NOTE=KEY0+7;              /* keybed key 7 at the base octave */
    uint16_t ch;unsigned vel;

    /* Nothing lit: as before, the track's channel (OMNI: 1), one message each way. */
    CHECK(mch_mask==0&&mch_out_mask()==CH(1));
    mo_mark();key(K,90);CHECK(mo_count(0x90,&ch,NOTE,&vel)==1&&ch==CH(1)&&vel==90);
    mo_mark();key(K,0);CHECK(mo_count(0x80,&ch,NOTE,0)==1&&ch==CH(1));
    set.midi_ch=5;seq_trk=2;CHECK(mch_out_mask()==CH(7));seq_trk=0;set.midi_ch=0;

    /* KNOB BANK: PERFORMANCE -> SPLIT/LAYER -> PATTERNS -> CANAIS MIDI -> PERFORMANCE. */
    tap_bank();CHECK(pf_bank()==3);tap_bank();CHECK(pf_bank()==2);tap_bank();CHECK(pf_bank()==5);
    CHECK(pf_page_request==PG_PERFORM+1u);
    tap_bank();CHECK(pf_bank()==0);tap_bank();tap_bank();tap_bank();CHECK(pf_bank()==5);

    /* Pads 1, 8 and 15 light channels 1, 8 and 15; nothing else changes (part, favourites, sound). */
    uint32_t used=pf_store.used,trk=seq_trk,prog=trk_prog[0];
    tap(0);tap(7);tap(14);
    CHECK(mch_mask==(CH(1)|CH(8)|CH(15)));
    CHECK(seq_trk==trk&&pf_store.used==used&&trk_prog[0]==prog&&!pf_manual_write);
    CHECK(mch_led(0)==RGB(60,230,110)&&mch_led(7)==RGB(60,230,110)&&mch_led(14)==RGB(60,230,110));
    CHECK(mch_led(1)!=mch_led(0)&&mch_led(15)!=mch_led(0));
    smk_leds();CHECK(padled_rgb[0]!=padled_rgb[1]&&padled_rgb[7]==padled_rgb[0]);

    /* A key goes out on all three at its velocity, and its release on all three; the local sound still plays. */
    unsigned q0=nq_w;mo_mark();key(K,77);
    CHECK(mo_count(0x90,&ch,NOTE,&vel)==3&&ch==(CH(1)|CH(8)|CH(15))&&vel==77);
    CHECK(nq_w!=q0);
    mo_mark();key(K,0);CHECK(mo_count(0x80,&ch,NOTE,0)==3&&ch==(CH(1)|CH(8)|CH(15)));

    /* A channel switched off while a note is held ends it there at once, and only there; the release then
     * reaches the two left, and not the channel lit after the note-on. */
    key(K,60);mo_mark();tap(7);
    CHECK(mch_mask==(CH(1)|CH(15)));
    CHECK(mo_count(0x80,&ch,NOTE,0)==1&&ch==CH(8));
    tap(3);CHECK(mch_mask==(CH(1)|CH(4)|CH(15)));
    mo_mark();key(K,0);CHECK(mo_count(0x80,&ch,NOTE,0)==2&&ch==(CH(1)|CH(15)));

    /* At most 8: the ninth is refused, flashes red, and changes nothing; switching one off frees a place. */
    tap(3);CHECK(mch_mask==(CH(1)|CH(15)));
    for(unsigned p=1;p<7;p++)tap(p);            /* 1..7 and 15 = 8 */
    CHECK(mch_count(mch_mask)==8&&mch_mask==(0x7Fu|CH(15)));
    tap(9);CHECK(mch_count(mch_mask)==8&&!(mch_mask&CH(10)));
    CHECK(mch_led(9)==C_RED);fm1_ms+=500;CHECK(mch_led(9)!=C_RED);
    mo_mark();key(K,100);CHECK(mo_count(0x90,&ch,NOTE,0)==8&&ch==(0x7Fu|CH(15)));key(K,0);
    tap(0);tap(9);CHECK(mch_count(mch_mask)==8&&(mch_mask&CH(10))&&!(mch_mask&CH(1)));

    /* Eight channels x a 4-note chord: 32 note-ons, all through (the USB ring holds 64), and 32 note-offs. */
    mo_mark();for(unsigned k=0;k<4;k++)key(K+k,100);
    unsigned ons=0;for(unsigned k=0;k<4;k++)ons+=mo_count(0x90,0,KEY0+K+k,0);CHECK(ons==32);
    mo_mark();for(unsigned k=0;k<4;k++)key(K+k,0);
    unsigned offs=0;for(unsigned k=0;k<4;k++)offs+=mo_count(0x80,0,KEY0+K+k,0);CHECK(offs==32);

    /* A live pad (REC on) keeps the channels it started on, and a channel switched off ends its chord there. */
    smk_live[2][0]=60;smk_live[2][1]=64;smk_live_mask[2]=CH(2)|CH(10);
    mo_mark();tap(9);CHECK(mo_count(0x80,&ch,60,0)==1&&ch==CH(10)&&mo_count(0x80,0,64,0)==1);
    CHECK(smk_live_mask[2]==CH(2));smk_live[2][0]=smk_live[2][1]=0;smk_live_mask[2]=0;

    /* Every pad off: back to the track's channel, as before. */
    for(unsigned p=0;p<16;p++)if(mch_mask>>p&1u)tap(p);
    CHECK(mch_mask==0&&mch_out_mask()==CH(1));
    mo_mark();key(K,50);CHECK(mo_count(0x90,&ch,NOTE,0)==1&&ch==CH(1));key(K,0);

    /* DRUM pads go out on the lit channels too, the release on the hit's own. */
    tap(4);tap(11);CHECK(mch_mask==(CH(5)|CH(12)));
    obv_drums_choose(0,0);render();render();
    pf_nav(4);CHECK(pf_bank()==4);
    mo_mark();pad(0,100);CHECK(mo_count(0x90,&ch,DRUM_NOTE[0],0)==2&&ch==(CH(5)|CH(12)));
    mch_mask=CH(3);                              /* (lit elsewhere meanwhile) */
    mo_mark();pad(0,0);CHECK(mo_count(0x80,&ch,DRUM_NOTE[0],0)==2&&ch==(CH(5)|CH(12)));
    CHECK(drum_pad_mask[0]==0);
    /* DRUM-X selected: KNOB BANK goes PATTERNS -> DRUM PADS -> CANAIS MIDI -> PERFORMANCE. */
    pf_nav(2);tap_bank();CHECK(pf_bank()==4);tap_bank();CHECK(pf_bank()==5);tap_bank();CHECK(pf_bank()==0);

    /* The overlay's lines fit the 240-pixel screen, the longest of each kind. */
    CHECK(text_w(&FONT_S,"NENHUM: CANAL 16 DA PARTE")<=232&&text_w(&FONT_S,"9 10 11 12 13 14 15 16")<=232);
    CHECK(text_w(&FONT_S,"MAXIMO 8 CANAIS")<=232&&text_w(&FONT_S,"PADS CANAIS MIDI")<=232);
    /* A picture of the overlay, to look at (build/host/canais-overlay.ppm). */
    pf_nav(5);page=PG_PERFORM;mch_mask=CH(1)|CH(8)|CH(15);
    for(unsigned i=0;i<20;i++){fm1_ms+=40;app_idle();}
    mch_overlay();for(unsigned i=0;i<5;i++){fm1_ms+=40;app_idle();}
    write_ppm_as("build/host/canais-overlay.ppm");
    printf("PASS: CANAIS MIDI: %u checks; pads 1-16 = channels, max 8, note-offs on their own channels, held notes ended on switch-off, fallback to the part's channel, drum pads, bank cycle.\n",checks);
    return 0;
}
