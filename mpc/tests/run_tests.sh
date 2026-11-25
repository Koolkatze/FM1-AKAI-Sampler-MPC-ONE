#!/usr/bin/env bash
# Tests of the FM1 Sampler MPC plugin. Run from the repository root after `python3 mpc/build_sdcard.py --sdk <sdk>`:
#   bash mpc/tests/run_tests.sh <path to mpc-plugin-builder-sdk>
#  1. fm1_test: every algorithm through the built-in programs, CPU per block, pitch, SPEED (offline WSOLA), state and
#     restore by path, user / LooperX folders, KIT slices, FM-1 control surface, block sizes 32..512.
#  2. fidelity: the port against the untouched phone engine of FM1 Sampler 0.42.7 (mpc/tests/reference/, copied from
#     engine/dsp) on the factory samples, sample by sample, with 3 notes and with full polyphony / voice stealing.
set -u
SDK="${1:-${MPC_SDK:-mpc/.sdk/mpc-plugin-builder-sdk}}"
ROOT="$(pwd)"; T="$(mktemp -d)"; FAILS=0
SO="$SDK/plugins/FM1Sampler/build/native/fm1-sampler.so"
[ -f "$SO" ] || { echo "native build not found: $SO (run mpc/build_sdcard.py first)"; exit 1; }

c++ -std=c++17 -O2 -I"$SDK/framework" mpc/tests/fm1_test.cpp -ldl -lpthread -o "$T/fm1_test" 2>/dev/null
FM1_SAMPLES_ROOT="$T/drive" "$T/fm1_test" "$SO" || FAILS=$((FAILS+1))

echo; echo "== fidelity against the phone engine (FM1 Sampler 0.42.7) =="
E=mpc/tests/reference; K=engine/3rdparty/kissfft; P=mpc/FM1Sampler/src
c++ -std=c++17 -O2 -DSAMPLERATE=44100 -I$E -I$K -Impc/tests mpc/tests/equivalence_ref.cpp $E/Sampler.cpp $E/Paul.cpp $E/Wav.cpp $K/kiss_fft.c $K/kiss_fftr.c -o "$T/ref" 2>/dev/null
c++ -std=c++17 -O2 -I$P -Impc/tests mpc/tests/equivalence_port.cpp $P/fm1_engine.cpp -lpthread -o "$T/port" 2>/dev/null
for m in 0:0:Keys/Glow 1:0:Keys/Koto 2:0:Pads/Light 3:0:Drums/TrapKit 4:0:Keys/Glow 0:1:Keys/Tape\ Key 1:1:Keys/Koto 2:1:Pads/Light 3:1:Drums/TrapKit 4:1:Keys/Glow; do
  mode=${m%%:*}; rest=${m#*:}; poly=${rest%%:*}; s=${rest#*:}
  "$T/ref" "samples/$s.wav" $mode $poly "$T/r" "$T/d" && "$T/port" $mode $poly "$T/p" "$T/d" || { FAILS=$((FAILS+1)); continue; }
  python3 -I - "$T/r" "$T/p" $mode "$s" $poly <<'PY' || FAILS=$((FAILS+1))
import sys, array, math
a = array.array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); b = array.array('f'); b.frombytes(open(sys.argv[2], 'rb').read())
mode = int(sys.argv[3]); name = ['MELODIC', 'ONE-SHOT', 'LOOPED', 'KIT', 'GRANULAR'][mode] + (' x' + ['70', '70', '70', '40', '20'][mode] if sys.argv[5] == '1' else '')
ea = sum(x * x for x in a); ed = sum((x - y) ** 2 for x, y in zip(a, b))
rel = 10 * math.log10(ed / max(ea, 1e-30) + 1e-30)
ok = len(a) == len(b) and rel < -100
print('%s  %-13s %-14s difference %7.1f dB relative to the phone engine' % ('PASS' if ok else 'FAIL', name, sys.argv[4], rel))
sys.exit(0 if ok else 1)
PY
done
rm -rf "$T"
echo; [ $FAILS -eq 0 ] && echo "ALL TESTS PASSED" || echo "$FAILS TEST GROUP(S) FAILED"
exit $FAILS
