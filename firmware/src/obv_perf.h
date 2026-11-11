/* SPDX-License-Identifier: GPL-3.0-only
 * SMK-37 Performance bank, independent of the LF1 latency path.
 * Slots snapshot the selected part; effects remain the existing shared sends/returns. */
#define PF_HOLD_MS 700u
#define PF_TAG_V1 0x31564650u /* PFV1: delay/reverb sends only */
#define PF_TAG 0x32564650u /* PFV2: all three sends; chorus 128 means legacy/unspecified */
typedef struct { patch_t patch; uint8_t prog, edited, dly, rev, colour, cho; } pf_slot_t;
typedef struct { uint32_t tag, generation, used; uint8_t last_send[2][NTRK]; pf_slot_t slot[8]; } pf_store_t;
_Static_assert(sizeof(pf_store_t) <= SETTINGS_MAX, "performance favourites exceed sector");
static pf_store_t pf_store;
static uint32_t pf_manual_write;
static uint32_t pf_pending, pf_dirty, pf_t, pf_saved_bank = 1, pf_manual_seq;
static uint8_t pf_role[16], pf_fired[8], pf_target[8], pf_active[NTRK];
static uint32_t pf_down[8], pf_flash[16], pf_bank_t, pf_bank_down, pf_bank_held;
static pf_slot_t pf_capture[8];
static void trk_select(uint32_t t);
static uint16_t smk_dim(uint16_t c, uint32_t k);
static void pf_redraw(void);
static void mch_overlay(void);                 /* (CANAIS MIDI) obv_smk.h */
/* Manual pad ownership is independent of transport/REC and display pages. */
static uint32_t pf_seq_mode;
static uint32_t pf_bank(void){return pf_seq_mode?1u:pf_manual_seq;}
static uint32_t pf_seq_bank(void) {return pf_bank()==1u;}
static uint32_t perf_pattern_bank(void) {return pf_bank()==2u;}
/* DP1: explicit fourth manual bank; selection is never changed by part/engine/REC. */
static uint32_t perf_drum_bank(void) {return pf_bank()==4u;}
static uint32_t pb_down[NPAT],pb_source[NPAT],pb_fired[NPAT],pb_confirm,pb_confirm_t,pb_confirm_source;
static uint32_t preset_ui,save_ui_dirty,save_ui_restore,save_ui_t,preset_part,preset_pattern,preset_home_cancel,preset_down,preset_t,preset_long;
static patch_t preset_snapshot;
static void pb_cancel(void){if(pb_confirm)save_ui_restore=1;pb_confirm=0;copy_arm=0;pf_redraw();}
static uint32_t pb_busy(void){return pb_write_target&&(copy_to||pat_save_say);}
static unsigned save_ui_active(void){return preset_ui||pb_confirm;}
static void preset_begin(void){preset_part=seq_trk%NTRK;preset_pattern=set.pat;preset_snapshot=live_p[preset_part];save_to=trk_prog[preset_part];layer=L_NONE;layer_lock=0;step_held=-1;audition_off();entry_reset();pb_cancel();save_ui_dirty=1;}
static void preset_commit(void){
 if(pb_busy()||copy_to||pat_queue||set.pat!=preset_pattern){preset_ui=4;save_ui_t=fm1_ms;save_ui_dirty=1;say("FINISH PATTERN SAVE FIRST");return;}
 unsigned n=save_to%NPROG,t=preset_part;
 preset_snapshot=live_p[t];prog_put(n,&preset_snapshot);trk_prog[t]=n;trk_edited[t]=0;set.tprog[t]=(uint8_t)n;if(!t)set.prog=(uint8_t)n;
 for(unsigned u=0;u<NTRK;u++)if(u!=t&&trk_prog[u]==n&&!trk_edited[u]){live_p[u]=preset_snapshot;if(!u)fx_keep(&live_p[0]);}
 set_dirty=1;set_t=fm1_ms;manual_store_request=1;preset_ui=2;save_ui_dirty=1;
}
static uint32_t save_button_poll(uint32_t b,uint32_t rel){
 if(preset_ui==3&&fm1_ms-save_ui_t>=2200u){preset_ui=0;save_ui_restore=1;pf_redraw();}
 if(b>>BTN_SAVE&1u){
  preset_down=1;preset_t=fm1_ms;preset_long=0;
  if(preset_ui==1)preset_long=2;else if(preset_ui==2)preset_long=3;else{preset_ui=0;preset_begin();}
 }
 if(preset_down&&(fm1_in.buttons>>BTN_SAVE&1u)&&fm1_ms-preset_t>=700u&&!preset_long){preset_long=1;preset_ui=1;save_ui_dirty=1;}
 if((rel>>BTN_SAVE&1u)&&preset_down){
  preset_down=0;if(!preset_long||preset_long==2)preset_commit();
 }
 if((preset_ui==1||preset_ui==4)&&(b&((1u<<BTN_HOME)|(1u<<BTN_KNOB_BANK)))){preset_ui=0;preset_down=0;save_ui_restore=1;pf_redraw();if(b>>BTN_HOME&1u)preset_home_cancel=1;}
 b&=~(1u<<BTN_SAVE);
 if(preset_ui){uint32_t allowed=(1u<<BTN_PLAY)|(1u<<BTN_REC);b&=allowed;}
 return b;
}
static void pb_commit(uint32_t p){
 if(pb_source[p]!=set.pat){pb_cancel();say("PATTERN CHANGED / CANCELLED");return;}
 if(pb_busy()||pat_queue||pend_slot||preset_ui==2){pb_cancel();say("STOP TO FINISH SAVE FIRST");return;}
 pb_cancel();
 if(p==set.pat)pat_save_to(p);
 else{copy_arm=p+1u;copy_arm_t=fm1_ms;pat_copy(p);}
 if(pat_save_say||copy_to)pb_write_target=p+1u;pf_redraw();
}
static void pb_save(uint32_t p){
 pb_fired[p]=1;
 if(pb_source[p]!=set.pat){pb_cancel();say("PATTERN CHANGED: HOLD AGAIN");return;}
 if(pb_busy()||pat_queue||pend_slot||preset_ui){say("STOP TO FINISH SAVE FIRST");return;}
 if(pat_used>>p&1u){pb_cancel();pb_confirm=p+1u;pb_confirm_source=set.pat;pb_confirm_t=fm1_ms;save_ui_dirty=1;}
 else pb_commit(p);
 pf_redraw();
}
static void pb_event(uint32_t p,uint32_t vel){
 if(p>=NPAT)return;
 if(vel){
  if(pb_confirm){if(pb_confirm==p+1u&&fm1_ms-pb_confirm_t<4000u&&set.pat==pb_confirm_source){pb_source[p]=set.pat;pb_fired[p]=2;return;}pb_cancel();}
  pb_down[p]=fm1_ms;pb_source[p]=set.pat;pb_fired[p]=0;
 }else if(pb_fired[p]==2){pb_fired[p]=1;if(pb_confirm==p+1u&&fm1_ms-pb_confirm_t<4000u)pb_commit(p);else pb_cancel();}
 else if(!pb_fired[p]){
  if(fm1_ms-pb_down[p]>=PF_HOLD_MS)pb_save(p);
  else {pb_cancel();if(pb_busy())say("STOP TO FINISH SAVE FIRST");else pat_go(p);pf_redraw();}
 }
}
/* Saving overlays are opaque buffered strips, never a clear-then-draw cycle.
 * Countdown changes repaint only their own row; static overlays do no LCD IO. */
static void save_ui_countdown(unsigned seconds){
 char line[32];
 line[0]=0;scat(line,"CANCEL IN ");fmt_u(line+slen(line),seconds);scat(line," SEC");
 cv_begin(240,20,C_PANEL);cv_text(8,0,&FONT_S,line,C_RED);cv_blit(0,165);
}
static void save_ui_target(char *line){
 patch_t target;prog_get(save_to,&target);
 unsigned n=save_to%NPROG;scat(line,psect[n/PS_N].used[n%PS_N]?"USER: ":"FACTORY: ");
 unsigned at=slen(line),len=PATCH_NAME;
 while(len&&target.v[NPARAM+len-1u]==' ')len--;
 if(!len){scat(line,"(NO NAME)");return;}
 for(unsigned i=0;i<len;i++){unsigned c=target.v[NPARAM+i];line[at+i]=(char)(c>=32u&&c<=126u?c:' ');}line[at+len]=0;
}
static void save_ui_draw(void){
 static uint32_t kind=0xFFFFFFFFu,dest=0xFFFFFFFFu,part=0xFFFFFFFFu,wait=0xFFFFFFFFu,seconds=0xFFFFFFFFu;
 unsigned mode=pb_confirm?5u:preset_ui,slot=pb_confirm?pb_confirm:save_to;
 unsigned waiting=preset_ui==2?!!ctl_seq_run:0u;
 unsigned elapsed=fm1_ms-pb_confirm_t,left=elapsed>=4000u?0u:(4999u-elapsed)/1000u;
 unsigned redraw=save_ui_dirty||kind!=mode||dest!=slot||part!=preset_part||wait!=waiting;
 if(!redraw){if(pb_confirm&&seconds!=left){seconds=left;save_ui_countdown(left);}return;}
 save_ui_dirty=0;kind=mode;dest=slot;part=preset_part;wait=waiting;seconds=left;
 cv_begin(240,120,C_PANEL);cv_text(8,8,&FONT_S,"SMKSYNTH / SAVE",C_AMBER);cv_rect(8,30,224,1,C_RED);
 char line[40];line[0]=0;
 if(pb_confirm){scat(line,"OVERWRITE PATTERN ");fmt_u(line+slen(line),pb_confirm);cv_text(8,49,&FONT_S,line,C_RED);cv_text(8,75,&FONT_S,"REPLACE STORED PATTERN?",C_CREAM);}
 else{scat(line,preset_ui==1?"SAVE PRESET AS":preset_ui==2?"SAVING PRESET":preset_ui==3?"PRESET SAVED":"SAVE FAILED / BUSY");cv_text(8,49,&FONT_S,line,preset_ui==4?C_RED:C_CREAM);line[0]=0;scat(line,"PROGRAM ");fmt_u(line+slen(line),save_to+1u);scat(line," / PART ");unsigned n=slen(line);line[n]='A'+preset_part;line[n+1]=0;cv_text(8,78,&FONT_S,line,C_CREAM);
  line[0]=0;save_ui_target(line);cv_text(8,102,&FONT_S,line,C_AMBER);
 }
 cv_blit(0,0);cv_begin(240,120,C_PANEL);
 if(pb_confirm){line[0]=0;scat(line,"TAP PAD ");fmt_u(line+slen(line),pb_confirm);scat(line," TO CONFIRM");cv_text(8,12,&FONT_S,line,C_CREAM);line[0]=0;scat(line,"CANCEL IN ");fmt_u(line+slen(line),left);scat(line," SEC");cv_text(8,45,&FONT_S,line,C_RED);cv_text(8,80,&FONT_S,"OTHER PAD / PATCH: CANCEL",C_DIMT);}
 else if(preset_ui==1){cv_text(8,12,&FONT_S,"K7: CHOOSE PROGRAM",C_AMBER);cv_text(8,42,&FONT_S,"BT: CONFIRM OVERWRITE",C_RED);cv_text(8,80,&FONT_S,"PATCH: CANCEL",C_DIMT);}
 else if(preset_ui==2){cv_text(8,12,&FONT_S,waiting?"WAITING / STOP TO SAVE":"WAITING FOR AUDIO QUIET",C_AMBER);cv_text(8,48,&FONT_S,"DO NOT SWITCH OFF",C_RED);}
 else {cv_text(8,24,&FONT_S,preset_ui==3?"WRITE COMPLETED":"NOT SAVED / TRY AGAIN",preset_ui==3?C_AMBER:C_RED);if(preset_ui==4)cv_text(8,72,&FONT_S,"PATCH: CLOSE / BT: RETRY",C_DIMT);}
 cv_blit(0,120);
}
/* Opposite palettes, identical brightness levels. No persistence work here. */
#define PF_SLOT_BLUE RGB(16,48,255)
#define PF_SLOT_EMPTY 16u
#define PF_SLOT_STORED 48u
#define PF_SLOT_SELECTED 240u
#define PF_SLOT_EDITED 96u
#define PF_NAV_IDLE 32u
#define PF_NAV_PRESSED 255u
#define PF_FX_ACTIVE 176u
static uint32_t pb_edited(void){
    return pat_edited || (snd_dirty>>set.pat&1u) || snd_changed(set.pat);
}
static uint16_t pb_led(uint32_t p){
    uint16_t blue=PF_SLOT_BLUE;
    if(p>=NPAT)return 0;
    if(pb_write_target==p+1u&&pb_busy())return C_CREAM;
    if(pb_confirm==p+1u&&fm1_ms-pb_confirm_t<4000u)return smk_dim(C_RED,(fm1_ms/200u)&1u?255u:30u);
    if(pat_queue==p+1u)return smk_dim(blue,(fm1_ms/250u)&1u?240u:45u);
    if(!(pat_used>>p&1u))return smk_dim(C_RED,PF_SLOT_EMPTY);
    if(p==set.pat)return smk_dim(blue,pb_edited()?PF_SLOT_EDITED:PF_SLOT_SELECTED);
    return smk_dim(C_RED,PF_SLOT_STORED);
}
static void pf_changed(void) { pf_dirty = 1; pf_t = fm1_ms; }
static void pf_redraw(void) { dirty_hdr = dirty_page = dirty_steps = dirty_tags = 1; hints_key = 0xFFFFFFFFu; }
static void pf_message(const char *s, uint32_t n) {
    static char text[32]; text[0]=0;scat(text,s);fmt_u(text+slen(text),n);say(text);pf_redraw();
}
static int pf_valid(const pf_store_t *s) {
    uint32_t i,p;
    if(s->tag!=PF_TAG || s->used & ~255u)return 0;
    for(i=0;i<2;i++)for(p=0;p<NTRK;p++)if(s->last_send[i][p]>127u)return 0;
    for(i=0;i<8;i++)if(s->used>>i&1u) {
        const pf_slot_t *v=&s->slot[i];
        if(v->prog>=NPROG||v->edited>1u||v->dly>127u||v->rev>127u||v->colour>=NTRK||v->cho>128u)return 0;
        for(p=0;p<NPARAM;p++)if(v->patch.v[p]>PDEF[p].max)return 0;
    }
    return 1;
}
/* Read either generation without writing during boot. Old favourites retain the
 * current part's chorus send until explicitly saved again. */
typedef struct { patch_t patch; uint8_t prog, edited, dly, rev, colour; } pf_slot_v1_t;
typedef struct { uint32_t tag, generation, used; uint8_t last_send[2][NTRK]; pf_slot_v1_t slot[8]; } pf_store_v1_t;
static int pf_read(uint32_t bank, pf_store_t *out) {
    static pf_store_v1_t old;
    if (!perf_store_load(bank,out,sizeof *out) && pf_valid(out)) return 1;
    if (perf_store_load(bank,&old,sizeof old) || old.tag!=PF_TAG_V1) return 0;
    memset(out,0,sizeof *out);out->tag=PF_TAG;out->generation=old.generation;out->used=old.used;
    memcpy(out->last_send,old.last_send,sizeof out->last_send);
    for(uint32_t i=0;i<8;i++) {
        out->slot[i].patch=old.slot[i].patch;out->slot[i].prog=old.slot[i].prog;
        out->slot[i].edited=old.slot[i].edited;out->slot[i].dly=old.slot[i].dly;
        out->slot[i].rev=old.slot[i].rev;out->slot[i].colour=old.slot[i].colour;
        out->slot[i].cho=128u;
    }
    return pf_valid(out);
}
static void pf_init(void) {
    static pf_store_t other;
    uint32_t a=pf_read(0,&pf_store);
    uint32_t b=pf_read(1,&other);
    pf_saved_bank=0;
    if(b&&(!a||(int32_t)(other.generation-pf_store.generation)>0)){pf_store=other;pf_saved_bank=1;}
    else if(!a){memset(&pf_store,0,sizeof pf_store);pf_store.tag=PF_TAG;pf_saved_bank=1;}
    for(uint32_t t=0;t<NTRK;t++) {
        pf_active[t]=0xFF;
        if(!pf_store.last_send[0][t])pf_store.last_send[0][t]=set.mix_dly[t]?set.mix_dly[t]:127;
        if(!pf_store.last_send[1][t])pf_store.last_send[1][t]=set.mix_rev[t]?set.mix_rev[t]:127;
    }
    pf_dirty=0;pf_manual_write=0;pf_manual_seq=0;
}
static void pf_save_poll(void) {
    if((OBV_MANUAL_STORAGE&&!pf_manual_write)||!pf_dirty||ctl_seq_run||!engine_quiet()||fm1_ms-pf_t<2000u)return;
    uint32_t next=pf_saved_bank^1u;pf_store.generation++;
    if(perf_store_save(next,&pf_store,sizeof pf_store)==0){pf_saved_bank=next;pf_dirty=pf_pending=pf_manual_write=0;say("FAVOURITES SAVED");}
    else{pf_store.generation--;pf_t=fm1_ms;say("FAV SAVE RETRY");}
}
static void pf_snapshot(pf_slot_t *v,uint32_t t) {
    v->patch=live_p[t];v->prog=(uint8_t)trk_prog[t];v->edited=(uint8_t)trk_edited[t];
    v->cho=set.mix_cho[t];v->dly=set.mix_dly[t];v->rev=set.mix_rev[t];v->colour=(uint8_t)t;
}
static void pf_recall(uint32_t i,uint32_t t) {
    if(!(pf_store.used>>i&1u)){pf_message("FAV EMPTY ",i+1u);return;}
    const pf_slot_t *v=&pf_store.slot[i];patch_t p=v->patch;
    if(!t)fx_keep(&p); /* preserve shared FX and their tails */
    smk_controls_epoch++;
    live_p[t]=p;trk_prog[t]=v->prog;trk_edited[t]=v->edited;set.tprog[t]=v->prog;
    if(!t)set.prog=v->prog;
    if(v->cho<=127u)set.mix_cho[t]=v->cho;
    set.mix_dly[t]=v->dly;set.mix_rev[t]=v->rev;mix_apply();
    pf_active[t]=(uint8_t)i;set_dirty=1;set_t=fm1_ms;pf_message("FAV LOADED ",i+1u);
}
static void pf_toggle(uint32_t effect) {
    uint32_t t=seq_trk;uint8_t *send=effect?set.mix_rev:set.mix_dly;
    uint32_t p=effect?P_REV_MIX:P_DLY_MIX;
    if(send[t]){pf_store.last_send[effect][t]=send[t];send[t]=0;}
    else {
        send[t]=pf_store.last_send[effect][t]?pf_store.last_send[effect][t]:127;
        /* If the shared return is explicitly at zero, make the requested effect audible. */
        if(!live_p[0].v[p]){set_param(p,48);fx_sync();}
    }
    mix_apply();set_dirty=1;set_t=fm1_ms;pf_changed();pf_redraw();
    say(effect?(send[t]?"REVERB SEND ON":"REVERB SEND OFF"):(send[t]?"DELAY SEND ON":"DELAY SEND OFF"));
}
static void pf_commit(uint32_t i) {
    pf_store.slot[i]=pf_capture[i];pf_store.used|=1u<<i;pf_fired[i]=1;pf_active[pf_target[i]]=(uint8_t)i;
    pf_manual_write=1;
    pf_flash[i+8u]=fm1_ms+300u;pf_pending|=1u<<i;pf_changed();pf_message("FAV CAPTURED ",i+1u);
}
static void pf_event(uint32_t p,uint32_t vel) {
    uint32_t t=seq_trk;
    if(p<8u) {
        if(!vel)return;
        if(p<4u)pf_flash[p]=fm1_ms+120u;
        if(p<4u){trk_select(p);pf_redraw();}
        else if(p>=6u){trk_load(t,(trk_prog[t]+NPROG+(p==6u?-1:1))%NPROG);pf_active[t]=0xFF;pf_redraw();}
        else pf_toggle(p-4u);
        return;
    }
    uint32_t i=p-8u;
    if(vel){pf_down[i]=fm1_ms;pf_fired[i]=0;pf_target[i]=(uint8_t)t;pf_snapshot(&pf_capture[i],t);}
    else if(!pf_fired[i]){if(fm1_ms-pf_down[i]>=PF_HOLD_MS)pf_commit(i);else pf_recall(i,pf_target[i]);}
}
static void pf_hold_poll(void) {
    if(pb_confirm&&(fm1_ms-pb_confirm_t>=4000u||set.pat!=pb_confirm_source))pb_cancel();
    if(pb_write_target&&!pb_busy())pb_write_target=0;
    for(uint32_t p=0;p<NPAT;p++)if(pf_role[p]==3u&&!pb_fired[p]&&fm1_ms-pb_down[p]>=PF_HOLD_MS)pb_save(p);
    for(uint32_t i=0;i<8u;i++)if(pf_role[i+8u]==2u&&!pf_fired[i]&&fm1_ms-pf_down[i]>=PF_HOLD_MS){
        pf_commit(i);
    }
}
/* Only BANK 4 handles two-part gestures. Other banks retain their roles. */
static void keyboard_pad(uint32_t p,uint32_t vel){
 unsigned row=p/8u,local=p%8u;
 if(row>1u)return;
 if(!vel){if(row==kb_edit_row&&local<4u){kb_pad_down&=(uint8_t)~(1u<<local);if(local==kb_pad_first&&kb_pad_down)for(unsigned t=0;t<4;t++)if(kb_pad_down>>t&1u){kb_pad_first=(uint8_t)t;break;}}return;}
 if(kb_edit_row!=row){kb_pad_down=0;kb_edit_row=(uint8_t)row;}
 if(local==7u){fm1_irq_off();pat.keyboard_selected=(uint8_t)row;fm1_irq_on();pat_edited=1;pat_edit_t=fm1_ms;page=PG_KEYBOARD;pf_redraw();say(row?"KEYBOARD PLAYS ROW 2":"KEYBOARD PLAYS ROW 1");return;}
 if(local<4u){
  if(obv_patch_engine(&live_p[local])!=OBV_ANALOG){say("ANALOG PARTS ONLY");return;}
  if(!kb_pad_down)kb_pad_first=(uint8_t)local;
  else if(local!=kb_pad_first){keyboard_cfg_t k;keyboard_defaults(&k);k.mode=KB_LAYER;k.part[0]=kb_pad_first;k.part[1]=(uint8_t)local;keyboard_changed(k);say("LAYER CREATED / SAVE PATTERN");}
  kb_pad_down|=(uint8_t)(1u<<local);trk_select(local);page=PG_KEYBOARD;pf_redraw();return;
 }
 keyboard_cfg_t k=*keyboard_row(&pat,row);uint8_t mode=(uint8_t)(local-4u);
 if(mode&&(obv_patch_engine(&live_p[k.part[0]])!=OBV_ANALOG||obv_patch_engine(&live_p[k.part[1]])!=OBV_ANALOG)){say("CHOOSE TWO ANALOG PARTS");return;}
 k.mode=mode;keyboard_changed(k);page=PG_KEYBOARD;pf_redraw();say(mode?"ROW COMBINATION ON":"ROW SINGLE");
}
static uint16_t keyboard_pad_led(uint32_t p){
 unsigned row=p/8u,local=p%8u;
 if(row>1u)return 0;
 if(local==7u)return pat.keyboard_selected==row?RGB(255,255,255):RGB(5,5,5);
 const keyboard_cfg_t*k=keyboard_row(&pat,row);
 if(local<4u){if(obv_patch_engine(&live_p[local])!=OBV_ANALOG)return smk_dim(C_DIMT,10);
  unsigned sel=k->mode&&(local==k->part[0]||local==k->part[1]);return smk_dim(C_TRKC[local],row==kb_edit_row&&(kb_pad_down>>local&1u)?255u:sel?180u:local==seq_trk?90u:18u);}
 return smk_dim(local==4?C_CREAM:local==5?PF_SLOT_BLUE:C_RED,k->mode==local-4u?220u:25u);
}
static uint32_t pf_page_request;
static void pf_nav(uint32_t bank){
 pf_seq_mode=0;pf_manual_seq=bank;pb_cancel();kb_pad_down=0;
 layer=L_NONE;layer_lock=0;step_held=-1;audition_off();entry_reset();
 pf_page_request=(bank==3u?PG_KEYBOARD:bank==4u?PG_DSOUND:PG_PERFORM)+1u;
 pf_redraw();dirty_hdr=1;
}
static void pf_bank_poll(uint32_t b,uint32_t rel){
 if(b>>BTN_KNOB_BANK&1u){pf_bank_down=1;pf_bank_t=fm1_ms;pf_bank_held=0;}
 if(pf_bank_down&&(fm1_in.buttons>>BTN_KNOB_BANK&1u)&&fm1_ms-pf_bank_t>=700u&&!pf_bank_held){pf_nav(0);pf_bank_held=1;}
 if(rel>>BTN_KNOB_BANK&1u){
  if(pf_bank_down&&!pf_bank_held){unsigned next=pf_seq_mode?0u:pf_manual_seq==0?3u:pf_manual_seq==3?2u:pf_manual_seq==2u?(obv_drums_selected()?4u:5u):pf_manual_seq==4u?5u:0u;pf_nav(next);if(next==5u)mch_overlay();say(next==5u?"PADS CANAIS MIDI":next==4u?"PADS DRUM PADS":next==3u?"PADS SPLIT / LAYER":next==2u?"PADS PATTERNS / SAVE":"PADS PERFORMANCE / FAV");}
  pf_bank_down=0;pf_bank_held=0;
 }
}
static uint32_t pf_home_down,pf_home_t,pf_home_long;
static uint32_t pf_home_poll(uint32_t b,uint32_t rel){
 if(b>>BTN_HOME&1u){pf_home_down=1;pf_home_t=fm1_ms;pf_home_long=preset_home_cancel?2u:layer?2u:0u;if(preset_home_cancel){preset_home_cancel=0;b&=~(1u<<BTN_HOME);}if(!pf_home_long)b&=~(1u<<BTN_HOME);}
 if(pf_home_down&&(fm1_in.buttons>>BTN_HOME&1u)&&fm1_ms-pf_home_t>=700u&&pf_home_long!=1u){
  pf_home_long=1;pf_seq_mode=0;layer=L_NONE;layer_lock=0;step_held=-1;audition_off();entry_reset();pf_page_request=PG_PERFORM+1u;pf_redraw();dirty_hdr=1;
 }
 if(rel>>BTN_HOME&1u){if(pf_home_down&&!pf_home_long)b|=1u<<BTN_HOME;pf_home_down=0;}
 return b;
}
static uint32_t pf_matches(uint32_t i,uint32_t t) {
    const pf_slot_t *v=&pf_store.slot[i];
    return trk_prog[t]==v->prog && obv_patch_engine(&live_p[t])==obv_patch_engine(&v->patch) && !memcmp(live_p[t].v,v->patch.v,P_CHO_RATE) &&
           !memcmp(live_p[t].v+NPARAM,v->patch.v+NPARAM,PATCH_NAME) && (v->cho==128u||set.mix_cho[t]==v->cho) && set.mix_dly[t]==v->dly && set.mix_rev[t]==v->rev;
}
static uint16_t pf_led(uint32_t p) {
    uint32_t t=seq_trk;
    if(p==6u||p==7u)return smk_dim(p==6u?C_RED:PF_SLOT_BLUE,pf_role[p]==2u?PF_NAV_PRESSED:PF_NAV_IDLE);
    if(pf_flash[p] && (int32_t)(pf_flash[p]-fm1_ms)>0)return C_CREAM;
    if(p<4u)return smk_dim(C_TRKC[p],p==t?240u:18u);
    if(p<8u){
        uint32_t on=(p==4u?set.mix_dly[t]&&live_p[0].v[P_DLY_MIX]:set.mix_rev[t]&&live_p[0].v[P_REV_MIX]);
        if(!on)return smk_dim(p==4u?C_CREAM:RGB(90,195,255),10);
        return smk_dim(p==4u?C_CREAM:RGB(90,195,255),PF_FX_ACTIVE); /* static: no periodic LED transfers */
    }
    uint32_t i=p-8u;
    if(!(pf_store.used>>i&1u))return smk_dim(PF_SLOT_BLUE,PF_SLOT_EMPTY);
    if(pf_pending>>i&1u)return smk_dim(RGB(255,180,35),(fm1_ms/250u)&1u?200u:30u);
    if(pf_active[t]==i&&trk_prog[t]==pf_store.slot[i].prog)
        return smk_dim(C_RED,pf_matches(i,t)?PF_SLOT_SELECTED:PF_SLOT_EDITED);
    return smk_dim(PF_SLOT_BLUE,PF_SLOT_STORED);
}

/* Show the favourite label only while the sound still matches its snapshot. */
static uint32_t perf_header_slot(void) {
    uint32_t t=seq_trk%NTRK,i=pf_active[t];
    return i<8u&&(pf_store.used>>i&1u)&&pf_matches(i,t)?i+1u:0u;
}

