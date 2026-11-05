#!/bin/bash
# Builds PolyendPlay_v1.6.0_mod.ppf from the sources and your own ../stock/PolyendPlay_v1.6.0.ppf.
set -e
cd "$(dirname "$0")"
R=..
OUT=PolyendPlay_v1.6.0_mod.ppf
CF="-mcpu=cortex-m4 -mthumb -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -nostdlib -fno-exceptions -fno-unwind-tables -Wall -Wextra -Wno-type-limits"
arm-none-eabi-gcc $CF -c fx.c -o fx.o
arm-none-eabi-gcc $CF -c usbup.c -o usbup.o
arm-none-eabi-gcc $CF -c usbaudio.c -o usbaudio.o
arm-none-eabi-gcc $CF -c multitrack.c -o multitrack.o
arm-none-eabi-gcc $CF -c arp.c -o arp.o
arm-none-eabi-gcc $CF -c usbmenu.c -o usbmenu.o
arm-none-eabi-gcc $CF -c nis.c -o nis.o            # Patch 018
arm-none-eabi-gcc $CF -c usbin.c -o usbin.o        # Patch 020
arm-none-eabi-gcc $CF -c usbrec.c -o usbrec.o      # Patch 024/025
arm-none-eabi-gcc $CF -c lrec.c -o lrec.o          # Patch 023
arm-none-eabi-gcc $CF -c modinit.c -o modinit.o
arm-none-eabi-ld -T link.ld fx.o usbup.o usbaudio.o multitrack.o arp.o usbmenu.o nis.o usbin.o usbrec.o lrec.o modinit.o -o fx.elf
arm-none-eabi-objcopy -O binary fx.elf fx.bin
if arm-none-eabi-objdump -h fx.o usbup.o usbaudio.o multitrack.o arp.o usbmenu.o nis.o usbin.o usbrec.o lrec.o modinit.o | grep -E '\.(data|bss)' | awk '$3!="00000000"' | grep -q .; then echo "FEHLER: .data/.bss belegt"; exit 1; fi
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
DESC_LEN=357
DESC_RAM=1fffd4c0
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
# init_array points to mod_init (arp_init → ParamDefs_ctor, then lr_reset)
ARGS="$ARGS aa06e=1c01 ae80c=$(le32 $(printf '%x' $((0x$(sym mod_init)|1)))) aa324=$(le32 $(printf '%x' $((0x$(sym arp_chord_hook)|1))))"

# ---- Patch 018: Randomize-Typ „Note In Scale“ (nis.c) ----
# MIDI-Typenliste 0x1FFF2190 (RAM, 4 Eintraege) → NIS_LIST (Flash, 6 Eintraege). Literal in Randomize_initTypes
# (0x3921C) bleibt: der Konstruktor beschreibt weiter die alte, nun unbenutzte Liste.
NISL=$(sym NIS_LIST); NISN=$(sym nis_name_copy)
for a in 38768 38918 38af8 38c04 38d68 38e64; do   # Literale in FUN_00038698/894/a74/b00/c10/d74
  expect $a 9021ff1f
  ARGS="$ARGS $a=$(le32 $NISL)"
done
expect 3894e 0322                     # FUN_0003893c (Typ wechseln): movne r2,#3 → #5 (hoechster MIDI-Typindex)
expect 38de6 0323                     # FUN_00038d74 (Anzeige):      movne r3,#3 → #5
expect 38dc0 fdf71cfe                 # FUN_00038d74: bl 0x369fc (Name kopieren) → bl nis_name_copy
ARGS="$ARGS 3894e=0522 38de6=0523 38dc0=$(python3 -I $R/tools/thumb_bl.py 38dc0 $NISN)"
echo "NIS_LIST=$NISL nis_name_copy=$NISN nis_gen_entry=$(sym nis_gen_entry) nis.o=$(arm-none-eabi-size -B nis.o | awk 'NR==2{print $1+$2}') Byte"

# ---- Live Rec long press (lrec.c) ----
LRB=$(sym lr_button_hook)
expect a015c 75a70200                 # vtable UI-Objekt 0xA0158 [1]: Tasten-Verteiler FUN_0002A774 → lr_button_hook
ARGS="$ARGS a015c=$(le32 $(printf '%x' $((0x$LRB|1))))"
echo "lr_button_hook=$LRB lrec.o=$(arm-none-eabi-size -B lrec.o | awk 'NR==2{print $1+$2}') Byte"

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
echo "um_menu_hook=$MENUH um_build=$(sym um_build)"
echo "arp_init=$(sym arp_init) arp_chord_hook=$(sym arp_chord_hook)"
echo "audio_tap=$TAP usbinit_hook=$UINIT ep_hook=$EPH setup_hook=$SETH mt_trk_hook=$TRK mt_fx_hook=$FXH ua_cfg_desc=$(sym ua_cfg_desc) Code $(stat -c %s fx.bin) Byte"
python3 -I $R/tools/patch_ppf.py $R/stock/PolyendPlay_v1.6.0.ppf $OUT --append b069c=fx.bin $ARGS
# Deskriptor aus dem gebauten Binary pruefen (genau die Bytes, die ins Geraet kaemen)
python3 -I -c "import sys;b=open('fx.bin','rb').read();o=int(sys.argv[1],16)-0xB069C;open('desc_fw.bin','wb').write(b[o:o+int(sys.argv[2])])" $(sym ua_cfg_desc) $DESC_LEN
python3 -I check_desc.py desc_fw.bin --orig $R/stock/firmware.bin
python3 -I -c "import sys;b=open('fx.bin','rb').read();o=int(sys.argv[1],16)-0xB069C;open('desc_in_fw.bin','wb').write(b[o:o+323])" $(sym UI_DESC_IN)
md5sum $OUT
