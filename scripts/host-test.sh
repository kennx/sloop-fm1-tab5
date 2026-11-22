#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPSTREAM_DIR="$ROOT_DIR/third_party/sloop-fm1"
cd "$UPSTREAM_DIR"

OUT=build/host
mkdir -p build/gen "$OUT"

read -r -a CC_CMD <<< "${CC:-cc}"
CFLAGS=(-O2 -w -fwrapv -fno-strict-aliasing -ffp-contract=off -Ibuild/gen -Ifirmware/src)

compile() {
    local name="$1"
    local source="$2"
    shift 2
    "${CC_CMD[@]}" "${CFLAGS[@]}" -o "$OUT/$name" "$source" "$@"
}

run() {
    echo "== $1"
    shift
    "$@"
}

unset GOLDEN_UPDATE BUDGET_UPDATE
if [[ -z "${SKIP_CPU_VALGRIND:-}" ]] && command -v valgrind >/dev/null 2>&1; then
    export CPU_VALGRIND=1
fi

run "generate DSP tables" python3 tools/gen_tables.py build/gen/felucca_tables.h
run "generate embedded samples" python3 tools/gen_samples.py build/gen/felucca_samples.h
run "generate drum kits" python3 tools/gen_drumkits.py build/gen/felucca_drumkits.h
run "generate FM6 patches" python3 tools/gen_fm6_patches.py build/gen/felucca_fm6.h

UI_ASSETS="$OUT/ui-assets"
UPSTREAM_RAW=https://raw.githubusercontent.com/isod89/sloop-fm1/bac9c05ea7627449405aa10e48da21d75b6c8c56/assets
mkdir -p "$UI_ASSETS/fonts"
for asset in fonts/ter-u16b.bdf icons.json icons.png; do
    if [[ ! -f "$UI_ASSETS/$asset" ]]; then
        curl -fLsS --retry 3 --retry-all-errors "$UPSTREAM_RAW/$asset" -o "$UI_ASSETS/$asset"
    fi
done
run "generate screen font" python3 -c \
    'from pathlib import Path; import sys; import tools.gen_font as g; g.FONTS = Path(sys.argv[1]); g.main("build/gen/felucca_font.h")' \
    "$UI_ASSETS/fonts"
run "generate UI icons" python3 tools/gen_icons.py build/gen/felucca_icons.h "$UI_ASSETS"
run "generate splash logo" python3 tools/gen_logo.py build/gen/sloop_logo.h

compile regress tests/regress.c -lm
run "regression: golden renders, health, voices, CPU budget" \
    "$OUT/regress" tests/golden.txt tests/cpu_baseline.txt

compile hostsim tests/hostsim.c -lm
run "DSP render (ANALOG preset 0)" "$OUT/hostsim" 0 0 1 "$OUT/render.wav"

compile seq2_test tests/seq2_test.c -lm
run "sequencer 2.0" "$OUT/seq2_test"

compile len_test tests/len_test.c -lm
run "note length: POLY spans, MONO / STEP FULL limits, shrink and delete" "$OUT/len_test"

compile hold_rec_test tests/hold_rec_test.c -lm
run "held-note recording: live, overdub, free take, MIDI overflow" "$OUT/hold_rec_test"

compile studio_drums_test tests/studio_drums_test.c -lm
run "drum lanes, kit audio, metronome, record arm, free take" "$OUT/studio_drums_test"

compile arranger_test tests/arranger_test.c -lm
run "arranger: order, repeats, timing, loop, tempo change, scene validation" "$OUT/arranger_test"

compile song_audio_test tests/song_audio_test.c -lm
run "song audio: four tracks, scene transitions, SONG REC, clean stop" "$OUT/song_audio_test"

compile project_test tests/project_test.c -lm
run "project formats, capture / apply, autosave" "$OUT/project_test"

run "old-format project fixtures (FUN5/FUN4/FUN1) import to the generated fields" \
    python3 "$ROOT_DIR/scripts/gen_project_fixtures.py" --out "$OUT/fixtures" --host-check

compile storage_test tests/storage_test.c
run "flash storage (A/B, torn writes)" "$OUT/storage_test"

compile upreset_test tests/upreset_test.c
run "user presets" "$OUT/upreset_test"

compile dsyn_test tests/dsyn_test.c -lm
run "drum synth and editor commands" "$OUT/dsyn_test"

compile fm6_test tests/fm6_test.c -lm
mkdir -p "$OUT/fm6-demo"
run "FM6: algorithms, envelopes, retrigger, DC, clipping, macros, patch formats, voices, the bank, demos" \
    "$OUT/fm6_test" "$OUT/fm6-demo"

compile drumkit_test tests/drumkit_test.c -lm
run "synthesised drum kits" \
    "$OUT/drumkit_test" "$OUT/drum-kits.wav" "$OUT/drum-kits.txt"

compile punch_test tests/punch_test.c -lm
run "punch-in FX" "$OUT/punch_test" "$OUT/punch-fx.wav"

compile soak_test tests/soak_test.c -lm
run "soak: ${SOAK_MIN:-1} minute(s) of random live use" \
    "$OUT/soak_test" "${SOAK_MIN:-1}"

echo "ALL HOST TESTS PASSED (subset)"
