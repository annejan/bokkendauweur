#!/usr/bin/env python3
# sid_to_sng.py  --  thin CLI shim that drives ChiptuneSAK
#
# usage: sid_to_sng.py <input.{sid,mid,midi,mod}> <output.sng>
#                      [--subtune N] [--seconds S] [--no-compress]
#
# Replaces the old sid2sng path inside qt/MainWindow.cpp::loadSidFile.
# Dispatches by input extension:
#   .sid          -> ChiptuneSAK SID importer (6502 emulator capture)
#   .mid / .midi  -> ChiptuneSAK MIDI importer
#   .mod          -> in-process ProTracker MOD parser -> MIDI -> ChiptuneSAK
#
# ChiptuneSAK must be importable. If you don't have it system-wide, set
# PYTHONPATH=/path/to/ChiptuneSAK before invoking, e.g. in QProcess env.
#
# Exit status:
#   0  -> wrote .sng successfully
#   1  -> bad args
#   2  -> ChiptuneSAK import failed (with a hint about PYTHONPATH)
#   3  -> conversion / export failed (diagnostic on stderr)

import argparse
import os
import sys
import traceback


def _die(code: int, msg: str) -> "NoReturn":
    sys.stderr.write(msg.rstrip() + "\n")
    sys.exit(code)


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="sid_to_sng.py",
        description="Convert a C64 .sid to a GoatTracker 2 .sng via ChiptuneSAK.",
    )
    p.add_argument("input", help="input .sid file")
    p.add_argument("output", help="output .sng file")
    p.add_argument("--subtune", type=int, default=0,
                   help="subtune index to extract (zero-based, default 0)")
    p.add_argument("--seconds", type=float, default=60.0,
                   help="seconds of playback to capture (default 60)")
    p.add_argument("--no-compress", action="store_true",
                   help="skip OnePassLeftToRight pattern compression")
    p.add_argument("--min-pattern", type=int, default=8,
                   help="minimum pattern length for the compressor (default 8)")

    # ChiptuneSAK SID importer knobs. Defaults chosen for tracker-
    # friendly output (long patterns, real per-frame timing) rather
    # than the ChiptuneSAK defaults which assume MIDI-style export.
    #
    # gcf-reduce: ChiptuneSAK's default True scales every row count by
    # the GCF of inter-event frame gaps. Tunes that only touch SID
    # regs every N frames collapse to 1/N the rows -> 64-row patterns
    # turn into 8-row stubs. We default OFF so each engine frame stays
    # a tracker row.
    p.add_argument("--gcf-reduce", dest="gcf_reduce",
                   action="store_true", default=False,
                   help="ChiptuneSAK: divide row counts by GCF of "
                        "inter-event gaps (default OFF — keeps "
                        "per-frame timing so patterns aren't collapsed)")
    p.add_argument("--no-gate-off-notes", dest="gate_off_notes",
                   action="store_false", default=True,
                   help="ChiptuneSAK: skip allowing new note starts "
                        "while the gate is off (default ON)")
    p.add_argument("--no-gate-on-assert", dest="assert_gate_on",
                   action="store_false", default=True,
                   help="ChiptuneSAK: skip asserting gate-on for "
                        "every new note (default ON)")
    p.add_argument("--always-include-freq", action="store_true",
                   default=False,
                   help="ChiptuneSAK: emit a freq write on every "
                        "delta row, not just at note starts")
    p.add_argument("--vibrato-margin", type=int, default=0,
                   help="ChiptuneSAK: cents margin for snapping a "
                        "vibrato'd note back to its previous pitch")
    return p.parse_args()


def main() -> int:
    args = _parse_args()

    if not os.path.isfile(args.input):
        _die(1, f"sid_to_sng: input file not found: {args.input}")

    try:
        import chiptunesak  # noqa: F401
        from chiptunesak import (SID, GoatTracker, RChirpSong,
                                 OnePassLeftToRight, MIDI)
    except ImportError as e:
        _die(2,
             "sid_to_sng: ChiptuneSAK is not importable.\n"
             f"  ({e})\n"
             "  Install it (pip install --editable .) inside a venv whose\n"
             "  python is on PATH, OR point PYTHONPATH at the ChiptuneSAK\n"
             "  source tree, e.g.:\n"
             "    PYTHONPATH=/path/to/ChiptuneSAK python3 sid_to_sng.py ...")

    def _normalize_rows(song):
        # ChiptuneSAK's SID importer leaves invariants broken from the
        # perspective of the GoatTracker writer:
        #   1. instr_num is only set on note-on rows (sid.py ~L199), so
        #      KEY_OFF / transition rows trip GtPatternRow.to_bytes()
        #      which refuses instr_num=None.
        #   2. gate_on can fire without a note_num attached, which trips
        #      midi_note_to_pattern_note(None) in the GT writer.
        # Walk each voice (and any compressor-produced patterns) and
        # forward-fill so the export survives.
        DEFAULT_INSTR = 1

        # GoatTracker's note range is C0..G#7 (midi 24..95 inclusive).
        # Notes outside that range raise in midi_note_to_pattern_note,
        # so clamp by transposing octaves until we fit.
        GT_MIDI_MIN = 24
        GT_MIDI_MAX = 95

        def _clamp_to_gt_range(n):
            if n is None:
                return None
            while n < GT_MIDI_MIN:
                n += 12
            while n > GT_MIDI_MAX:
                n -= 12
            return n

        def _fix(rows_iter):
            prev_instr = DEFAULT_INSTR
            prev_note = None
            for r in rows_iter:
                if getattr(r, "instr_num", None) is not None:
                    prev_instr = r.instr_num
                else:
                    r.instr_num = prev_instr
                if getattr(r, "note_num", None) is not None:
                    r.note_num = _clamp_to_gt_range(r.note_num)
                    prev_note = r.note_num
                if getattr(r, "gate", None) and r.note_num is None:
                    if prev_note is not None:
                        r.note_num = prev_note
                    else:
                        # No note has ever been seen — drop the gate so
                        # the writer doesn't see a noteless gate-on.
                        r.gate = None

        for v in getattr(song, "voices", []):
            rows = getattr(v, "rows", {})
            _fix(rows.values() if isinstance(rows, dict) else rows)

        for p in getattr(song, "patterns", []) or []:
            _fix(getattr(p, "rows", []))

    def _import_sid():
        sid = SID()
        sid.set_options(
            verbose=False,
            gcf_row_reduce=args.gcf_reduce,
            create_gate_off_notes=args.gate_off_notes,
            assert_gate_on_new_note=args.assert_gate_on,
            always_include_freq=args.always_include_freq,
            vibrato_cents_margin=args.vibrato_margin,
        )
        song = sid.to_rchirp(
            args.input,
            subtune=args.subtune,
            seconds=args.seconds,
        )
        _normalize_rows(song)
        return song

    def _import_midi(path):
        # ChiptuneSAK MIDI path imports straight into chirp, then we
        # raise to rchirp via the standard ChirpSong.to_rchirp() so the
        # same downstream pipeline (compression + GoatTracker writer)
        # works.
        #
        # ChirpSong.to_rchirp() refuses to run on an unquantised song
        # (raises ChiptuneSAKQuantizationError). Quantise to 16th notes
        # so freshly-imported MIDI / converted-from-MOD files round to
        # a tracker grid that fits GoatTracker's row-based layout.
        midi = MIDI()
        chirp_song = midi.to_chirp(path)
        chirp_song.quantize_from_note_name('16')
        rchirp = chirp_song.to_rchirp()
        _normalize_rows(rchirp)
        return rchirp

    def _import_mod():
        # ChiptuneSAK has no MOD importer, so we parse the ProTracker
        # / NoiseTracker MOD format inline and emit a MIDI file via
        # mido (already a ChiptuneSAK runtime dep). Then feed it
        # through the regular MIDI pipeline.
        #
        # We only honour: note + sample (-> MIDI channel program),
        # row-by-row timing at the default tempo. Effects beyond
        # Fxx (set speed/tempo) are ignored — good enough for a
        # tracker-grid import, not for sample-accurate playback.
        import tempfile
        import mido

        with open(args.input, "rb") as f:
            data = f.read()
        if len(data) < 1084:
            raise ValueError("MOD file too short (< 1084 bytes)")

        magic = data[1080:1084]
        n_channels = {
            b"M.K.": 4, b"M!K!": 4, b"FLT4": 4, b"4CHN": 4,
            b"6CHN": 6, b"8CHN": 8, b"FLT8": 8,
        }.get(magic)
        if n_channels is None:
            # 15-sample SoundTracker (no magic) — file is too short
            # to contain a magic ID after 31 samples. If we got here,
            # treat as unsupported.
            raise ValueError(
                f"unsupported MOD magic {magic!r} "
                "(only M.K. / FLT4 / 4-8CHN supported)")

        song_len = data[950]
        order = list(data[952:952 + song_len])
        n_patterns = max(order) + 1 if order else 0
        pattern_data = data[1084:1084 + n_patterns * 64 * n_channels * 4]

        # ProTracker period table — Amiga periods for C-1..B-3 (3 octaves).
        PERIODS = [
            856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,  # oct 1
            428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,  # oct 2
            214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,  # oct 3
        ]
        # MOD C-1 → MIDI 36 (C2). Standard ProTracker convention.
        MIDI_BASE = 36

        def period_to_midi(period):
            if period == 0:
                return None
            # Nearest index in PERIODS.
            best_i, best_d = 0, abs(PERIODS[0] - period)
            for i, p in enumerate(PERIODS[1:], 1):
                d = abs(p - period)
                if d < best_d:
                    best_i, best_d = i, d
            return MIDI_BASE + best_i

        # Build a multi-track MIDI file: one MIDI track per MOD channel.
        # Default MOD tempo: speed=6 ticks/row at 50 Hz (CIA timer),
        # giving ~120 ms / row. PPQ=24 and tempo 500000 us/qn (120 BPM)
        # gives 6 ticks/16th = one MOD row per 6 ticks.
        PPQ = 24
        TICKS_PER_ROW = 6
        mf = mido.MidiFile(ticks_per_beat=PPQ)
        # Per-channel state: list of (track, last_note, last_tick).
        tracks = []
        for c in range(n_channels):
            t = mido.MidiTrack()
            t.append(mido.MetaMessage('set_tempo', tempo=500_000, time=0))
            t.append(mido.Message('program_change', program=0,
                                  channel=c % 16, time=0))
            mf.tracks.append(t)
            tracks.append({"track": t, "note": None, "last": 0})

        global_row = 0
        for pat_idx in order:
            pat_off = pat_idx * 64 * n_channels * 4
            for row in range(64):
                row_off = pat_off + row * n_channels * 4
                for c in range(n_channels):
                    co = row_off + c * 4
                    b0, b1, b2, b3 = pattern_data[co:co + 4]
                    period = ((b0 & 0x0F) << 8) | b1
                    midi_note = period_to_midi(period)
                    if midi_note is None:
                        continue
                    state = tracks[c]
                    tick_now = global_row * TICKS_PER_ROW
                    # Stop previous note on this channel.
                    if state["note"] is not None:
                        delta = tick_now - state["last"]
                        state["track"].append(mido.Message(
                            'note_off', note=state["note"],
                            velocity=0, channel=c % 16, time=delta))
                        state["last"] = tick_now
                    delta = tick_now - state["last"]
                    state["track"].append(mido.Message(
                        'note_on', note=midi_note, velocity=96,
                        channel=c % 16, time=delta))
                    state["note"] = midi_note
                    state["last"] = tick_now
                global_row += 1

        # Final note-offs at song end.
        end_tick = global_row * TICKS_PER_ROW
        for c in range(n_channels):
            state = tracks[c]
            if state["note"] is not None:
                delta = end_tick - state["last"]
                state["track"].append(mido.Message(
                    'note_off', note=state["note"],
                    velocity=0, channel=c % 16, time=delta))

        with tempfile.NamedTemporaryFile(
                suffix=".mid", delete=False) as tf:
            tmp_mid = tf.name
        mf.save(tmp_mid)
        try:
            return _import_midi(tmp_mid)
        finally:
            try:
                os.unlink(tmp_mid)
            except OSError:
                pass

    ext = os.path.splitext(args.input)[1].lower()
    try:
        if ext in (".mid", ".midi"):
            rchirp_song = _import_midi(args.input)
        elif ext == ".mod":
            rchirp_song = _import_mod()
        else:
            rchirp_song = _import_sid()
    except Exception:
        if ext in (".mid", ".midi"):
            kind = "MIDI"
        elif ext == ".mod":
            kind = "MOD"
        else:
            kind = "SID"
        sys.stderr.write(
            f"sid_to_sng: {kind} import failed for {args.input}\n"
            f"  subtune={args.subtune} seconds={args.seconds}\n"
        )
        traceback.print_exc(file=sys.stderr)
        return 3

    # Pattern compression is what makes the .sng tracker-friendly. For
    # SIDs whose play routine is wildly irregular the compressor (or the
    # GT writer fed the compressed rchirp) can fail mid-export — when
    # that happens, fall back to the uncompressed rchirp so the user
    # still gets a playable .sng instead of an error dialog.
    compressed = None
    if not args.no_compress:
        try:
            compressor = OnePassLeftToRight()
            compressed = compressor.compress(
                rchirp_song, min_length=max(1, args.min_pattern))
            _normalize_rows(compressed)
        except Exception as e:
            sys.stderr.write(
                f"sid_to_sng: compression failed ({e}); writing uncompressed.\n")

    def _try_export(song) -> bool:
        try:
            GT = GoatTracker()
            GT.to_file(song, args.output)
            return True
        except Exception as e:
            sys.stderr.write(
                f"sid_to_sng: GoatTracker .sng export attempt failed ({e}).\n")
            return False

    exported = False
    if compressed is not None:
        exported = _try_export(compressed)
        if not exported:
            sys.stderr.write(
                "sid_to_sng: retrying export with uncompressed rchirp.\n")
            # Re-import so the un-mutated rchirp object is fresh; some
            # exporters poke state into the song during a failed run.
            try:
                rchirp_song = _import_sid()
            except Exception:
                traceback.print_exc(file=sys.stderr)
                return 3
    if not exported:
        exported = _try_export(rchirp_song)
    if not exported:
        sys.stderr.write(
            f"sid_to_sng: GoatTracker .sng export failed for {args.output}\n")
        return 3

    if not (os.path.isfile(args.output) and os.path.getsize(args.output) > 0):
        _die(3, f"sid_to_sng: output {args.output} is missing or empty.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
