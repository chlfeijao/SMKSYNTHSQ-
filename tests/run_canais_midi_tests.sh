#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# (CANAIS MIDI) the unit test (sanitized), then the same feature end to end through the real main loop.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -O1 -g -fsanitize=address,undefined -Wno-unused-function -DOBV_HOST -DBOARD_SMK37=1 -Ifirmware/src -Ifirmware/gen \
   -o build/host/midi_channels tests/midi_channels_test.c
ASAN_OPTIONS=detect_leaks=0 build/host/midi_channels | grep -v "pattern slot"
BOARD=smk37 sh tests/run_host.sh tests/canais_midi_script.txt > build/host/canais_script.log 2>&1
got=$(grep "MIDI out" build/host/canais_script.log | sed 's/^ *//')
want="4800 ms  MIDI out 90 60 90
4800 ms  MIDI out 97 60 90
4800 ms  MIDI out 9E 60 90
5000 ms  MIDI out 87 60 0
5300 ms  MIDI out 80 60 0
5300 ms  MIDI out 8E 60 0"
if [ "$got" != "$want" ]; then echo "FAIL: end to end MIDI out:"; echo "$got"; exit 1; fi
echo "PASS: end to end: KNOB BANK x3, pads 1 8 15, a key on channels 1 8 15, pad 8 off while held, release on 1 15."
