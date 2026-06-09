#!/usr/bin/env python3
# sid_to_sng.py  --  thin CLI shim that drives ChiptuneSAK
#
# usage: sid_to_sng.py <input.sid> <output.sng> [--subtune N] [--seconds S]
#                                               [--no-compress]
#
# Replaces the old sid2sng path inside qt/MainWindow.cpp::loadSidFile.
# Chiptunesak imports PSID + many RSID files (sid2sng only handled
# GoatTracker-generated .sids), so this is a strict superset of what
# the editor could load before.
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

    def _import_midi():
        # ChiptuneSAK MIDI path imports straight into chirp, then we
        # raise to rchirp via the standard ChirpSong.to_rchirp() so the
        # same downstream pipeline (compression + GoatTracker writer)
        # works.
        midi = MIDI()
        chirp_song = midi.to_chirp(args.input)
        rchirp = chirp_song.to_rchirp()
        _normalize_rows(rchirp)
        return rchirp

    ext = os.path.splitext(args.input)[1].lower()
    try:
        if ext in (".mid", ".midi"):
            rchirp_song = _import_midi()
        else:
            rchirp_song = _import_sid()
    except Exception:
        kind = "MIDI" if ext in (".mid", ".midi") else "SID"
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
