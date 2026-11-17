#!/bin/bash
# 32-Bit-Hosttests (gcc -m32, ASan/UBSan). Laeuft auf Linux mit multilib direkt, auf dem Mac im Container
# (test_all.sh ruft: docker run play-test32, Image aus tools/docker32). Abschnittsnummern wie in test_all.sh.
set -eo pipefail
cd "$(dirname "$0")"
SAN="-O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all"
O=$(mktemp -d)
echo "== 8. Arp (32 Bit)";            gcc -m32 -O1 -DHOST_TEST -fsanitize=address,undefined -o $O/arp_test arp.c arp_test.c && $O/arp_test | tail -1
echo "== 11. Menue USB Audio (32 Bit)"; gcc -m32 $SAN -DNO_UI -o $O/um_test usbmenu.c cb.c um_test.c && for s in 1 2 3 4 5; do $O/um_test $s | tail -1; done
echo "== 13. Note In Scale (32 Bit)";  gcc -m32 -O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all -o $O/nis_test nis.c nis_test.c && $O/nis_test ../stock/firmware.bin | tail -1
echo "== 15. USB-Eingang (32 Bit)";    gcc -m32 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -o $O/ui_test ui_test.c -lm && for s in 1 2 3; do $O/ui_test $s | tail -1; done
echo "== 17. Slicer (32 Bit)";         gcc -m32 -O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all -o $O/sl_test -DWITH_GRANULAR -DSL_MAXSLMODE=8 slicer.c granular.c wave.c lrec.c slview.c tap.c hdr.c sl_test.c -lm && $O/sl_test | tail -1
echo "== 19. Aufnahme (32 Bit)";       gcc -m32 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -o $O/rec_test rec_test.c && for s in 1 2 3; do $O/rec_test $s | tail -1; done
echo "== 21. Waveform + Live Rec (32 Bit)"; gcc -m32 -O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all -o $O/wv_test -DWITH_GRANULAR -DSL_MAXSLMODE=8 wave.c lrec.c slicer.c granular.c slview.c tap.c hdr.c wv_test.c -lm && $O/wv_test | tail -1
echo "== 24. Beat-Erkennung (32 Bit)"; if [ -f ../stock/testbeat_120bpm.wav ] && [ -f test_data/Rec_001.wav ]; then gcc -m32 -O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all -o $O/beat_test beat_test.c slview.c -lm && $O/beat_test ../stock/testbeat_120bpm.wav test_data/Rec_001.wav | tail -1; else echo "   uebersprungen (Testaudio nur im privaten Arbeitsstand)"; fi
rm -rf $O
echo "32-BIT-TESTS: fertig"
