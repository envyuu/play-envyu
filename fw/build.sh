#!/bin/bash
# Baut PolyendPlay_v1.6.0_mod.ppf (Patch 045 = Release-Stand play-patcher 3d121e8a (Spurauswahl) + Slicer aus 044, Granular ausgeblendet)
# Vorgeschichte 044: Patch 029 = 028 + schlanke Slicer-Vorbereitung in View …
# 022 = 020 USB-Eingang + 021 Slicer/Granular, beide auf 019
# 019 = 017 Menue/Config/Classic-Creative/GET + 018 Randomize "Note In Scale"
# 016 = 015 USB-Multitrack + 014 Arp/Loop-Fix auf Basis von Patch 012
# (FX-Suite + USB-Upload + FLASH/DIAG/PEEK aus 011, Stereo-USB-Audio aus 012).
set -e
cd "$(dirname "$0")"
R=..
OUT=PolyendPlay_v1.6.0_mod.ppf
CF="-mcpu=cortex-m4 -mthumb -Os -ffunction-sections -fdata-sections -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -nostdlib -fno-exceptions -fno-unwind-tables -Wall -Wextra -Wno-type-limits"
arm-none-eabi-gcc $CF -c fx.c -o fx.o
arm-none-eabi-gcc $CF -c usbup.c -o usbup.o
arm-none-eabi-gcc $CF -c usbaudio.c -o usbaudio.o
arm-none-eabi-gcc $CF -c multitrack.c -o multitrack.o
arm-none-eabi-gcc $CF -c arp.c -o arp.o
arm-none-eabi-gcc $CF -c usbmenu.c -o usbmenu.o
arm-none-eabi-gcc $CF -c nis.c -o nis.o            # Patch 018
arm-none-eabi-gcc $CF -c usbin.c -o usbin.o        # Patch 020
arm-none-eabi-gcc $CF -c slicer.c -o slicer.o      # Patch 021
arm-none-eabi-gcc $CF -c granular.c -o granular.o  # Patch 021
arm-none-eabi-gcc $CF -c usbrec.c -o usbrec.o      # Patch 024/025
arm-none-eabi-gcc $CF -c slview.c -o slview.o        # Patch 026 (View-Slicer)
arm-none-eabi-gcc $CF -c wave.c -o wave.o
arm-none-eabi-gcc $CF -c tap.c -o tap.o            # Patch 045 (Tap Tempo)
arm-none-eabi-gcc $CF -c hdr.c -o hdr.o            # Patch 045 (Kopfzeile: Engine der Spur)
arm-none-eabi-gcc $CF -c guard.c -o guard.o        # 13.11.: Sicherheitsnetz (Kopfblock)
arm-none-eabi-gcc $CF -c pf.c -o pf.o              # 045 (12.11.): Perform behaelt die Spurauswahl
arm-none-eabi-gcc $CF -c cb.c -o cb.o              # Patch 045 (Color Mode)
arm-none-eabi-gcc $CF -c tapw.c -o tapw.o          # Patch 045 (Tempo-Anzeige, feste Ziffernfelder)
arm-none-eabi-gcc $CF -c boot.c -o boot.o          # 046 (14.11.): envyu-Startbild + About
MK=""; if [ -n "$MARKER" ]; then arm-none-eabi-gcc $CF -c marker.c -o marker.o; MK="marker.o -u BL_MARKER"; fi   # nur Test: 8 KB Muster (Bootloader > 0xC0000)
arm-none-eabi-gcc $CF -c lrec.c -o lrec.o          # Patch 023
arm-none-eabi-ld --gc-sections -u sn_guard -u SN_HDR -u SN_VENEERS -u NIS_LIST -u AUD_LIST -u rz_value_bake -u rz_value_prev -u rz_bake -u UI_DESC_IN -u arp_chord_hook -u arp_init -u audio_tap -u cb_pixel -u bs_boot -u ld_create -u ab_init -u din_send_hook -u ep_hook -u gr_cfg -u gr_set_playing -u gr_state -u gr_update -u hdr_hook -u pf_reset_keep -u pf_reset_keep_enter -u pf_trigger_hook -u pf_poll -u rz_pad_draw -u rz_poll -u loop_hook -u lr_button_hook -u lr_poll -u mt_fx_hook -u mt_trk_hook -u nis_gen_entry -u nis_name_copy -u rec_capture -u rec_silence -u um_rec_wait -u um_rec_pause -u rec_init -u rec_poll -u rec_request -u setup_hook -u sl_begin -u sl_build -u sl_format -u sl_init -u sl_kind_apply -u sl_load_hook -u sl_new_hook -u sl_param_hook -u sl_play_hook -u sl_reset -u sl_save_hook -u sl_transport -u sv_assign_sample -u sv_cfg_get -u sv_cfg_set -u sv_chg_hook -u sv_enc_hook -u sv_grid_hook -u sv_kb_hook -u sv_knob_hook -u sv_led_hook -u sv_poll -u sv_touch_hook -u tick_hook -u tt_menu_draw -u tt_poll -u tw_set -u ua_cfg_desc -u ui_cb_hook -u ui_in_build -u ui_send_hook -u um_build -u um_menu_hook -u um_poll -u usb_send_hook -u usbinit_hook -u wv_kb -u wv_poll -u wv_touched_param -u wv_view_hook -T link.ld guard.o fx.o usbup.o usbaudio.o multitrack.o arp.o usbmenu.o nis.o slicer.o granular.o usbin.o usbrec.o slview.o wave.o lrec.o tap.o hdr.o pf.o cb.o tapw.o boot.o $MK -o fx.elf
arm-none-eabi-objcopy -O binary fx.elf fx.bin
if arm-none-eabi-objdump -h guard.o fx.o usbup.o usbaudio.o multitrack.o arp.o usbmenu.o nis.o slicer.o granular.o usbin.o usbrec.o slview.o wave.o lrec.o tap.o hdr.o pf.o cb.o tapw.o | grep -E ' \.(data|bss) ' | awk '$3!="00000000"' | grep -q .; then echo "FEHLER: .data/.bss belegt"; exit 1; fi
# schwache Symbole (rec_request/rec_status aus Patch 024) duerfen fehlen → 0
if arm-none-eabi-nm -u fx.elf | grep -v ' w ' | grep -q .; then echo "FEHLER: unaufgeloeste Symbole:"; arm-none-eabi-nm -u fx.elf; exit 1; fi
# keine Bibliotheksaufrufe (memcpy/memset/__aeabi_*) im eigenen Code
if arm-none-eabi-objdump -d fx.elf | grep -E 'bl\s.*<(mem|__aeabi|__gnu)' | grep -q .; then echo "FEHLER: Bibliotheksaufruf im Patch-Code"; exit 1; fi
sym() { arm-none-eabi-nm fx.elf | awk -v s="$1" '$3==s{print $1}'; }
le32() { python3 -I -c "import sys;print(int(sys.argv[1],16).to_bytes(4,'little').hex())" "$1"; }
le16() { python3 -I -c "import sys;print(int(sys.argv[1],0).to_bytes(2,'little').hex())" "$1"; }
expect() { python3 -I -c "import sys;d=open(sys.argv[1],'rb').read();a=int(sys.argv[2],16);h=sys.argv[3];assert d[a:a+len(h)//2].hex()==h,(sys.argv[2],d[a:a+len(h)//2].hex())" $R/stock/firmware.bin "$1" "$2"; }

# ---- Patch 011 (unveraendert) ----
USB=$(printf '%x' $((0x$(sym usb_send_hook)|1))); DIN=$(printf '%x' $((0x$(sym din_send_hook)|1))); TICK=$(sym tick_hook); LOOP=$(sym loop_hook)
ARGS="9ad40=$(le32 $USB) 9aa8c=$(le32 $DIN)"
for a in 46ffe 47010 47054; do ARGS="$ARGS $a=$(python3 -I $R/tools/thumb_bl.py $a $TICK)"; done
expect bd70 f7f7d4fc
ARGS="$ARGS bd70=$(python3 -I $R/tools/thumb_bl.py bd70 $LOOP)"
# 017: Namenszeiger → RAM-Puffer 0x1FFFB400 + 16*i (usbmenu.c fuellt sie je nach Classic/Creative).
# Die Original-Zeiger muessen den Tabellen ORIG_DELAY/ORIG_REVERB in usbmenu.c entsprechen:
ORIGD="a6078 a6080 a6090 a60a0 a60ac a60bc a60c4 a60d4 9d3fc 9d404 9d410 9d41c 9d428 9d434 9d43c 9d44c"
ORIGR="a5fbc a5fc8 a5fd0 a5fdc a5fec a5ff8 a6004 a6010 a6018 a6020 a602c a603c a6044 a6054 a605c a606c"
k=0; for o in $ORIGD; do expect $(printf '%x' $((0xA86B4 + 20*k + 8))) $(le32 $o); k=$((k+1)); done
k=0; for o in $ORIGR; do expect $(printf '%x' $((0xA8E34 + 20*k + 8))) $(le32 $o); k=$((k+1)); done
for k in $(seq 0 15); do
  ARGS="$ARGS $(printf '%x' $((0xA86B4 + 20*k + 8)))=$(le32 $(printf '%x' $((0x1FFFB400 + 16*k))))"        # Delay (MIDI)
  ARGS="$ARGS $(printf '%x' $((0xA8E34 + 20*k + 8)))=$(le32 $(printf '%x' $((0x1FFFB400 + 16*(k+16)))))"   # Reverb (MIDI)
done

# ---- Patch 012: USB-Audio (Deskriptor in 015 laenger und an anderer RAM-Adresse) ----
TAP=$(printf '%x' $((0x$(sym audio_tap)|1))); UINIT=$(sym usbinit_hook); EPH=$(sym ep_hook); SETH=$(sym setup_hook)
DESC_LEN=390                          # 049: 365 → 390
DESC_RAM=1fffd4a0                     # 049: D4C0 → D4A0
expect bd58 19ba0000                  # setup(): Literal audio_callback 0xBA19 → audio_tap
expect 13810 fff78afe                 # _init_Teensyduino_internal_: bl usb_init 0x13528 → usbinit_hook
expect 12bac b24d9e08                 # usbotg_isr: ldr r5,=BDT; lsrs r6,r3,#2 (Token EP != 0) → bl ep_hook
expect 12dec 284b0f22                 # usbotg_isr: ldr r3,=USB0; movs r2,#15 (STALL) → bl setup_hook
expect 9bacc 00020000d81dff1fb500     # usb_descriptor_list[1]: {0x0200, 0, 0x1FFF1DD8, 181}
ARGS="$ARGS bd58=$(le32 $TAP)"
ARGS="$ARGS 13810=$(python3 -I $R/tools/thumb_bl.py 13810 $UINIT)"
ARGS="$ARGS 12bac=$(python3 -I $R/tools/thumb_bl.py 12bac $EPH)"
ARGS="$ARGS 12dec=$(python3 -I $R/tools/thumb_bl.py 12dec $SETH)"
ARGS="$ARGS 9bad0=$(le32 $DESC_RAM) 9bad4=$(le16 $DESC_LEN)"

# ---- Patch 015: Multitrack-Abgriffe im Mixer-Update 0x15884 ----
TRK=$(sym mt_trk_hook); FXH=$(sym mt_fx_hook)
expect 158d6 029b089f                 # ldr r3,[sp,#8]; ldr r7,[sp,#32]  (Spureingang r6 gewaehlt) → bl mt_trk_hook
expect 15cbc daf85471                 # ldr.w r7,[sl,#0x154]  (nach Delay/Reverb-Aufruf)          → bl mt_fx_hook
ARGS="$ARGS 158d6=$(python3 -I $R/tools/thumb_bl.py 158d6 $TRK)"
ARGS="$ARGS 15cbc=$(python3 -I $R/tools/thumb_bl.py 15cbc $FXH)"

# ---- Patch 014: Arp + Loop-Fix (Loop-Fix steckt in fx.c) ----
expect aa06e 0000                     # MIDI-Grenzwerttabelle 0xA9FB4, Platz 11: max 0 → 0x011C
expect ae80c d1710200                 # init_array: ParamDefs_ctor 0x271D1 → arp_init
expect aa324 59910400                 # vtable MidiTrack +0x54: MidiTrack_playChord 0x49159 → arp_chord_hook
# 021: init_array zeigt auf sl_init (ruft arp_init → ParamDefs_ctor, dann Slicer-Formatter)
ARGS="$ARGS aa06e=1c01 ae80c=$(le32 $(printf '%x' $((0x$(sym sn_guard)|1)))) aa324=$(le32 $(printf '%x' $((0x$(sym arp_chord_hook)|1))))"

# ---- Patch 018: Randomize-Typ „Note In Scale“ (nis.c) ----
# MIDI-Typenliste 0x1FFF2190 (RAM, 4 Eintraege) → NIS_LIST (Flash, 6 Eintraege). Literal in Randomize_initTypes
# (0x3921C) bleibt: der Konstruktor beschreibt weiter die alte, nun unbenutzte Liste.
NISL=$(sym NIS_LIST); NISN=$(sym nis_name_copy)
for a in 38768 38918 38af8 38c04 38d68 38e64; do   # Literale in FUN_00038698/894/a74/b00/c10/d74
  expect $a 9021ff1f
  ARGS="$ARGS $a=$(le32 $NISL)"
done
expect 3894e 0322                     # FUN_0003893c (Typ wechseln): movne r2,#3 → #14 (hoechster MIDI-Typindex, 12.11.: + Scatter Steps)
expect 38de6 0323                     # FUN_00038d74 (Anzeige):      movne r3,#3 → #13
expect 38dc0 fdf71cfe                 # FUN_00038d74: bl 0x369fc (Name kopieren) → bl nis_name_copy
ARGS="$ARGS 3894e=0e22 38de6=0e23 38dc0=$(python3 -I $R/tools/thumb_bl.py 38dc0 $NISN)"
# 045 (11.11.): Audio-Typenliste 0x1FFF28FC (RAM, 10) → AUD_LIST (Flash, 17); Literal im Konstruktor (0x391F0) bleibt
AUDL=$(sym AUD_LIST)
for a in 3891c 38afc 38c0c 38d64 38e70; do
  expect $a fc28ff1f
  ARGS="$ARGS $a=$(le32 $AUDL)"
done
expect 38950 0922                     # FUN_0003893c: moveq r2,#9 → #17 (hoechster Audio-Typindex, 12.11.)
expect 38de8 0923                     # FUN_00038d74: moveq r3,#9 → #16
ARGS="$ARGS 38950=1122 38de8=1123"
# Kontext fuer Shuffle/Walk/Call & Response: Aufrufe von Randomizer_valueFor und Randomizer_bake umleiten
expect 38b60 fff788ff                 # Randomizer_bake:   bl Randomizer_valueFor → rz_value_bake
expect 38d5c fff78afe                 # Randomizer_modify: bl Randomizer_valueFor → rz_value_prev
expect 21b26 16f0ebff                 # SeqEdit: bl Randomizer_bake → rz_bake
ARGS="$ARGS 38b60=$(python3 -I $R/tools/thumb_bl.py 38b60 $(sym rz_value_bake)) 38d5c=$(python3 -I $R/tools/thumb_bl.py 38d5c $(sym rz_value_prev)) 21b26=$(python3 -I $R/tools/thumb_bl.py 21b26 $(sym rz_bake))"
echo "NIS_LIST=$NISL nis_name_copy=$NISN nis_gen_entry=$(sym nis_gen_entry) nis.o=$(arm-none-eabi-size -B nis.o | awk 'NR==2{print $1+$2}') Byte"

# ---- Patch 021: Slicer / Granular (slicer.c, granular.c) ----
SLP=$(sym sl_param_hook); SLB=$(sym sl_begin); GRU=$(sym gr_update)
expect 489a4 fff734ff                 # FUN_00048910 (Audio-Step applyParam): bl MidiTrack_getParam 0x48810 → sl_param_hook
expect aa384 05810400                 # vtable 0xAA338 +0x4C (Audio-Step, Aufruf vor der Parameterschleife): 0x48105 (bx lr) → sl_begin
expect 9ced0 d18d0100                 # vtable Sampler-Stimme 0x9CED0 [0]: TrackVoice_update 0x18DD1 → gr_update
expect 14c5c d0ce0900                 # (nur Pruefung) Literal in Hal_createTrackNode = 0x9CED0, einzige Verwendung
ARGS="$ARGS 489a4=$(python3 -I $R/tools/thumb_bl.py 489a4 $SLP)"
ARGS="$ARGS aa384=$(le32 $(printf '%x' $((0x$SLB|1)))) 9ced0=$(le32 $(printf '%x' $((0x$GRU|1))))"
# ---- Patch 024: Engines pro Projekt, Granular-Stopp ----
SLL=$(sym sl_load_hook); SLS=$(sym sl_save_hook); SLN=$(sym sl_new_hook); SLPL=$(sym sl_play_hook)
expect 1e0d2 fff717fa                 # FUN_0001e044 (Projekt oeffnen): bl FUN_0001d504(ps, pfad) → sl_load_hook
expect 1e206 fff77df9                 # FUN_0001e250 (Start, "workspace"): bl FUN_0001d504(ps, pfad) → sl_load_hook
expect 1dff4 fff71afe                 # FUN_0001def0 (Projekt speichern): bl SamplePool_save 0x1DC2C (letzter Schritt) → sl_save_hook
expect 41a92 dcf7e9fb                 # Project_createNew: bl FUN_0001e268(ps) → sl_new_hook
expect a99d4 e55a0400                 # vtable Engine 0xA99B4 +0x20: setPlaying 0x45AE5 → sl_play_hook
ARGS="$ARGS 1e0d2=$(python3 -I $R/tools/thumb_bl.py 1e0d2 $SLL) 1e206=$(python3 -I $R/tools/thumb_bl.py 1e206 $SLL)"
ARGS="$ARGS 1dff4=$(python3 -I $R/tools/thumb_bl.py 1dff4 $SLS) 41a92=$(python3 -I $R/tools/thumb_bl.py 41a92 $SLN)"
ARGS="$ARGS a99d4=$(le32 $(printf '%x' $((0x$SLPL|1))))"
echo "sl_load_hook=$SLL sl_save_hook=$SLS sl_new_hook=$SLN sl_play_hook=$SLPL rec_poll=$(sym rec_poll) usbrec.o=$(arm-none-eabi-size -B usbrec.o | awk 'NR==2{print $1+$2}') Byte"
echo "sl_init=$(sym sl_init) sl_param_hook=$SLP sl_begin=$SLB gr_update=$GRU slicer+granular=$(( $(arm-none-eabi-size -B slicer.o | awk 'NR==2{print $1+$2}') + $(arm-none-eabi-size -B granular.o | awk 'NR==2{print $1+$2}') )) Byte"

# ---- Patch 023: Waveform-Ansicht (wave.c) ----
WVH=$(sym wv_view_hook)
expect 2fec8 fff750fa                 # FUN_0002FD54 (Step-Parameter-Seite): bl FUN_0002F36C (Seite fuellen) → wv_view_hook
ARGS="$ARGS 2fec8=$(python3 -I $R/tools/thumb_bl.py 2fec8 $WVH)"
LRB=$(sym lr_button_hook)
expect a015c 75a70200                 # vtable UI-Objekt 0xA0158 [1]: Tasten-Verteiler FUN_0002A774 → lr_button_hook
ARGS="$ARGS a015c=$(le32 $(printf '%x' $((0x$LRB|1))))"
echo "lr_button_hook=$LRB lrec.o=$(arm-none-eabi-size -B lrec.o | awk 'NR==2{print $1+$2}') Byte"
echo "wv_view_hook=$WVH wv_poll=$(sym wv_poll) wave.o=$(arm-none-eabi-size -B wave.o | awk 'NR==2{print $1+$2}') Byte"

# ---- Patch 017: Menueseite "USB Audio", Deskriptorliste im RAM ----
MENUH=$(sym um_menu_hook)
expect 33676 fff749ff                 # MainMenu_build: bl MenuItem_ctor 0x3350C ("Settings") → um_menu_hook
expect 131ac c0ba0900                 # usbotg_isr GET_DESCRIPTOR: Literal usb_descriptor_list 0x9BAC0 → um_t.list (RAM)
ARGS="$ARGS 33676=$(python3 -I $R/tools/thumb_bl.py 33676 $MENUH) 131ac=$(le32 1fffd370)"
# ---- Patch 020: USB-Eingang (usbin.c), Einspeisung im Mixer-Update 0x15884 ----
UICB=$(sym ui_cb_hook); UISEND=$(sym ui_send_hook)
expect 15a3e daf85031                 # ldr.w r3,[sl,#0x150] (Zusatzeingang des Mixers)        → bl ui_cb_hook
expect 15cb6 32462346                 # mov r2,r6; mov r3,r4 (vor blx r7 = Delay/Reverb)       → bl ui_send_hook
ARGS="$ARGS 15a3e=$(python3 -I $R/tools/thumb_bl.py 15a3e $UICB) 15cb6=$(python3 -I $R/tools/thumb_bl.py 15cb6 $UISEND)"
echo "ui_cb_hook=$UICB ui_send_hook=$UISEND UI_DESC_IN=$(sym UI_DESC_IN) usbin.o=$(arm-none-eabi-size -B usbin.o | awk 'NR==2{print $1+$2}') Byte"
# ---- Patch 045: Tap Tempo im Menue (tap.c) ----
TTD=$(sym tt_menu_draw)
expect a97ec 39460400                 # Menue-Bildschirm vtable 0xA97E0 +0x0C: Zeichnen 0x44639 → tt_menu_draw
ARGS="$ARGS a97ec=$(le32 $(printf '%x' $((0x$TTD|1))))"
ENCH=$(sym sv_enc_hook)
expect a0168 49a80200                 # UI-vtable 0xA0158 [4]: Ui_onEncoder 0x2A849 → sv_enc_hook (Slice per Encoder)
ARGS="$ARGS a0168=$(le32 $(printf '%x' $((0x$ENCH|1))))"
HDH=$(sym hdr_hook)
PFK=$(sym pf_reset_keep)
expect 223b6 18f0ddfa                 # Perform betreten FUN_00022398: bl FUN_0003a974 (loescht die Spurauswahl) → pf_reset_keep
expect 222d4 18f04efb                 # Perform verlassen FUN_0002229c: dito
PFE=$(sym pf_reset_keep_enter)
ARGS="$ARGS 223b6=$(python3 -I $R/tools/thumb_bl.py 223b6 $PFE) 222d4=$(python3 -I $R/tools/thumb_bl.py 222d4 $PFK)"
PFT=$(sym pf_trigger_hook)
RZPD=$(sym rz_pad_draw)
expect a5a80 d5a30300                 # Editor-vtable +0x2C: FUN_0003a3d4 (Step-Pad zeichnen) → rz_pad_draw (sichtbare Randomize-Vorschau)
ARGS="$ARGS a5a80=$(le32 $(printf '%x' $((0x$RZPD|1))))"
expect 49d64 fff772f9                 # drei Aufrufer von MidiTrack_triggerStep 0x4904C → pf_trigger_hook (Anschlag-Blinken in Perform)
expect 49eae fff7cdf8
expect 49fbe fff745f8
for a in 49d64 49eae 49fbe; do ARGS="$ARGS $a=$(python3 -I $R/tools/thumb_bl.py $a $PFT)"; done
expect 30840 f9f79cfb                 # FUN_00030814: bl FUN_00029f7c (Statuszeile: Projektname / Pattern) → hdr_hook
ARGS="$ARGS 30840=$(python3 -I $R/tools/thumb_bl.py 30840 $HDH)"
CBPX=$(sym cb_pixel)
for a in 52c4 6e20 6f28 6fae 711e 7176 722e; do   # alle bl Leds_setPixel 0x6CD4 → cb_pixel (Color Mode)
  python3 -I -c "import sys;d=open(sys.argv[1],'rb').read();a=int(sys.argv[2],16);h1,h2=int.from_bytes(d[a:a+2],'little'),int.from_bytes(d[a+2:a+4],'little');S=(h1>>10)&1;o=(S<<24)|((1^(((h2>>13)&1)^S))<<23)|((1^(((h2>>11)&1)^S))<<22)|((h1&0x3FF)<<12)|((h2&0x7FF)<<1);o-=(1<<25) if S else 0;assert (h1&0xF800)==0xF000 and (h2&0xD000)==0xD000 and a+4+o==0x6CD4,sys.argv[2]" $R/stock/firmware.bin $a
  ARGS="$ARGS $a=$(python3 -I $R/tools/thumb_bl.py $a $CBPX)"
done
expect 4ebc 01f00abf                  # Leds_setColor: b.w Leds_setPixel (Endsprung) → b.w cb_pixel
CBW=$(python3 -I -c "
import sys
a=int(sys.argv[1],16);t=int(sys.argv[2],16)&~1;o=t-(a+4);S=1 if o<0 else 0;o&=(1<<25)-1
i1=(o>>23)&1;i2=(o>>22)&1;j1=(1-i1)^S;j2=(1-i2)^S
h1=0xF000|(S<<10)|((o>>12)&0x3FF);h2=0x9000|(j1<<13)|(j2<<11)|((o>>1)&0x7FF)
print(h1.to_bytes(2,'little').hex()+h2.to_bytes(2,'little').hex())" 4ebc $CBPX)
ARGS="$ARGS 4ebc=$CBW"
echo "tt_menu_draw=$TTD tap.o=$(arm-none-eabi-size -B tap.o | awk 'NR==2{print $1+$2}') Byte"
# ---- Patch 026: Slicer in View ----
SVKB=$(sym sv_kb_hook); SVCHG=$(sym sv_chg_hook); SVKNOB=$(sym sv_knob_hook); SVTOUCH=$(sym sv_touch_hook)
SVGRID=$(sym sv_grid_hook)
expect a5a68 21bc0300                 # Editor-vtable +0x14: FUN_0003bc20 (Grid-Pad) → sv_grid_hook (Patch 027)
ARGS="$ARGS a5a68=$(le32 $(printf '%x' $((0x$SVGRID|1))))"
SVLED=$(sym sv_led_hook)
expect 3a75a e8f7f9fc                 # FUN_0003a6fc: bl FUN_00023150 (Pad-Farbe) → bl sv_led_hook (Patch 027)
expect 3a784 e8f7e4fc
ARGS="$ARGS 3a75a=$(python3 -I $R/tools/thumb_bl.py 3a75a $SVLED) 3a784=$(python3 -I $R/tools/thumb_bl.py 3a784 $SVLED)"
expect 3b77e fef713bd                 # FUN_0003b6dc: b.w FUN_0003a1a8 (Tastatur-Pad in View) → b.w sv_kb_hook
expect a5a70 89b00300                 # Editor-vtable 0xA5A54 +0x1C: FUN_0003b088 (Parameter aendern) → sv_chg_hook
expect a0164 4da70200                 # UI-vtable [3]: Ui_onKnobTouch → sv_touch_hook (Patch 030)
expect a0160 1da70200                 # UI-vtable 0xA0158 [2]: Ui_onKnobTurn 0x2A71C → sv_knob_hook (Patch 029: Track Length = Slice-Zahl)
BW=$(python3 -I -c "
import sys
a=int(sys.argv[1],16);t=int(sys.argv[2],16)&~1;o=t-(a+4);S=1 if o<0 else 0;o&=(1<<25)-1
i1=(o>>23)&1;i2=(o>>22)&1;j1=(1-i1)^S;j2=(1-i2)^S
h1=0xF000|(S<<10)|((o>>12)&0x3FF);h2=0x9000|(j1<<13)|(j2<<11)|((o>>1)&0x7FF)
print(h1.to_bytes(2,'little').hex()+h2.to_bytes(2,'little').hex())" 3b77e $SVKB)
ARGS="$ARGS 3b77e=$BW a5a70=$(le32 $(printf '%x' $((0x$SVCHG|1)))) a0160=$(le32 $(printf '%x' $((0x$SVKNOB|1)))) a0164=$(le32 $(printf '%x' $((0x$SVTOUCH|1))))"
echo "sv_kb_hook=$SVKB (b.w $BW) sv_chg_hook=$SVCHG sv_knob_hook=$SVKNOB slview.o=$(arm-none-eabi-size -B slview.o | awk 'NR==2{print $1+$2}') Byte"
echo "um_menu_hook=$MENUH um_build=$(sym um_build)"
echo "arp_init=$(sym arp_init) arp_chord_hook=$(sym arp_chord_hook)"
echo "audio_tap=$TAP usbinit_hook=$UINIT ep_hook=$EPH setup_hook=$SETH mt_trk_hook=$TRK mt_fx_hook=$FXH ua_cfg_desc=$(sym ua_cfg_desc) Code $(wc -c < fx.bin | tr -d " ") Byte"
# ---- 046 (14.11.): Startbild – Gfx_init-Endsprung b.w FUN_00003e8c → b.w bs_boot ----
expect 405c fff716bf
BSB=$(python3 -I -c "
import sys
a=int(sys.argv[1],16);t=int(sys.argv[2],16)&~1;o=t-(a+4);S=1 if o<0 else 0;o&=(1<<25)-1
i1=(o>>23)&1;i2=(o>>22)&1;j1=(1-i1)^S;j2=(1-i2)^S
h1=0xF000|(S<<10)|((o>>12)&0x3FF);h2=0x9000|(j1<<13)|(j2<<11)|((o>>1)&0x7FF)
print(h1.to_bytes(2,'little').hex()+h2.to_bytes(2,'little').hex())" 405c $(sym bs_boot))
ARGS="$ARGS 405c=$BSB"
# ---- 17.11.: Ladebildschirm – vtable 0xA05F8 [1] FUN_0002ed78 (P-Logo + Balken) → ld_create (Endbild der Animation + Balken) ----
expect a05fc 79ed0200
ARGS="$ARGS a05fc=$(le32 $(printf '%x' $((0x$(sym ld_create)|1))))"
echo "ld_create=$(sym ld_create)"
echo "bs_boot=$(sym bs_boot) (b.w $BSB) ab_build=$(sym ab_build) boot.o=$(arm-none-eabi-size -B boot.o | awk 'NR==2{print $1+$2}') Byte"
# 13.11.: Sicherheitsnetz – Weichen fuer alle Code-Patchstellen, Kopf mit Laenge/CRC (veneer.py schreibt fx.bin neu)
ARGS=$(python3 -I veneer.py fx.bin $(sym SN_VENEERS) $(sym SN_HDR) $R/stock/firmware.bin $ARGS)
END=$((0xB069C + $(wc -c < fx.bin | tr -d " ")))
if [ $END -gt $((0xC3000)) ]; then printf "FEHLER: Code endet bei 0x%x > 0xC3000 – der Bootloader schreibt darueber nicht (17.11.2026: Notfallmodus nach dem Flash)\n" $END; exit 1; fi
printf "Code-Ende 0x%x (Grenze 0xC3000, frei %d Byte)\n" $END $((0xC3000 - END))
python3 -I $R/tools/patch_ppf.py $R/stock/PolyendPlay_v1.6.0.ppf $OUT --append b069c=fx.bin $ARGS
# Deskriptor aus dem gebauten Binary pruefen (genau die Bytes, die ins Geraet kaemen)
python3 -I -c "import sys;b=open('fx.bin','rb').read();o=int(sys.argv[1],16)-0xB069C;open('desc_fw.bin','wb').write(b[o:o+int(sys.argv[2])])" $(sym ua_cfg_desc) $DESC_LEN
python3 -I check_desc.py desc_fw.bin --orig $R/stock/firmware.bin
python3 -I -c "import sys;b=open('fx.bin','rb').read();o=int(sys.argv[1],16)-0xB069C;open('desc_in_fw.bin','wb').write(b[o:o+331])" $(sym UI_DESC_IN)
# Nur fuer den Emulator-Test: gleiches Image + Attrappe von usbrec.c (rec_request/rec_status), NICHT flashen
arm-none-eabi-gcc $CF -c rec_stub.c -o rec_stub.o
arm-none-eabi-ld --gc-sections -u NIS_LIST -u AUD_LIST -u rz_value_bake -u rz_value_prev -u rz_bake -u UI_DESC_IN -u arp_chord_hook -u arp_init -u audio_tap -u cb_pixel -u bs_boot -u ld_create -u ab_init -u din_send_hook -u ep_hook -u gr_cfg -u gr_set_playing -u gr_state -u gr_update -u hdr_hook -u loop_hook -u lr_button_hook -u lr_poll -u mt_fx_hook -u mt_trk_hook -u nis_gen_entry -u nis_name_copy -u setup_hook -u sl_begin -u sl_build -u sl_format -u sl_init -u sl_kind_apply -u sl_load_hook -u sl_new_hook -u sl_param_hook -u sl_play_hook -u sl_reset -u sl_save_hook -u sl_transport -u sv_assign_sample -u sv_cfg_get -u sv_cfg_set -u sv_chg_hook -u sv_enc_hook -u sv_grid_hook -u sv_kb_hook -u sv_knob_hook -u sv_led_hook -u sv_poll -u sv_touch_hook -u tick_hook -u tt_menu_draw -u tt_poll -u tw_set -u ua_cfg_desc -u ui_cb_hook -u ui_in_build -u ui_send_hook -u um_build -u um_menu_hook -u um_poll -u usb_send_hook -u usbinit_hook -u wv_kb -u wv_poll -u wv_touched_param -u wv_view_hook -u lr_button_hook -T link.ld guard.o fx.o usbup.o usbaudio.o multitrack.o arp.o usbmenu.o nis.o slicer.o granular.o usbin.o slview.o wave.o lrec.o tap.o hdr.o cb.o tapw.o boot.o rec_stub.o -o fx_rectest.elf
arm-none-eabi-objcopy -O binary fx_rectest.elf fx_rectest.bin
# 13.11.: Test-Binary hat denselben Kopf → Weichen aus fx.bin uebernehmen (nur wenn Kopf und Einstiege gleich liegen)
python3 -I - <<'PYEOF'
import subprocess
def nm(f): return {p[2]: int(p[0], 16) for p in (l.split() for l in subprocess.run(["arm-none-eabi-nm", f], capture_output=True, text=True).stdout.splitlines()) if len(p) == 3}
a, b = nm("fx.elf"), nm("fx_rectest.elf")
keys = ["SN_VENEERS", "__head_end"] + [k for k in a if k.endswith("_hook") and k in b]
same = all(a[k] == b[k] for k in ["SN_VENEERS", "__head_end"])
import struct, sys
sys.path.insert(0, ".")
import veneer as V
fx_ = open("fx.bin", "rb").read(); bad = []
for i in range(80):                                  # jedes Weichenziel: dieselbe Funktion an derselben Adresse im Test-Binary?
    va = a["SN_VENEERS"] + 24 * i; o = va - 0xB069C
    if fx_[o:o + 4] != bytes.fromhex("dff810c0"): continue
    d = V.dec_bl(*struct.unpack_from("<HH", fx_, o + 16)); t = va + 20 + d[1]
    names = [k for k, v in a.items() if v == t and not k.startswith("$")]
    if not any(b.get(k) == t for k in names): bad.append((hex(t), names))
if bad: print("FEHLER: Weichenziele im Test-Binary verschoben:", bad); sys.exit(1)
if same:
    fx = open("fx.bin", "rb").read(); t = bytearray(open("fx_rectest.bin", "rb").read())
    o = a["SN_VENEERS"] - 0xB069C; t[o:o + 80 * 24] = fx[o:o + 80 * 24]
    open("fx_rectest.bin", "wb").write(t)
    print("fx_rectest.bin: Weichen uebernommen")
else: print("WARNUNG: fx_rectest.bin hat anderen Kopf – Weichen nicht uebernommen")
PYEOF
python3 -I -c "import hashlib,sys;print(hashlib.md5(open(sys.argv[1],'rb').read()).hexdigest()+'  '+sys.argv[1])" $OUT
