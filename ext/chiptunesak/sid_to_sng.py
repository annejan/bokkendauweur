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
    p.add_argument("--max-channels", type=int, default=0, metavar="N",
                   help="MIDI/MOD: fold the song down to at most N voices "
                        "(3 = mono SID, 6 = stereo / dual-SID). 0 (default) "
                        "keeps every voice, so a 4+ voice tune becomes a "
                        "6-channel stereo .sng.")
    p.add_argument("--reduce", choices=("merge", "drop"), default="merge",
                   help="MIDI/MOD: how --max-channels reduces extra voices — "
                        "'merge' (default) is pitch-aware: keeps the bass and "
                        "lead voices on their own channels and folds the "
                        "closest inner voices together (so a bass-only intro "
                        "still plays); 'drop' discards the quietest voices "
                        "(which can silence such an intro).")
    p.add_argument("--keep-midi-channels", default="", metavar="LIST",
                   help="MIDI: comma-separated 1-based MIDI channels to keep as "
                        "dedicated SID voices (e.g. '1,3,4'). Any other channel "
                        "is folded into the kept voice closest in pitch, so an "
                        "intro carried by one channel survives. Overrides the "
                        "automatic --reduce strategy.")
    return p.parse_args()


def _fit_channels(song, max_ch, mode, keep=""):
    """Fold a ChirpSong down to fit GoatTracker's voices.

    GoatTracker has 3 voices mono / 6 stereo. A MIDI/MOD with more voices
    would otherwise force a 6-channel stereo .sng; this brings it back to
    a chosen channel count.

    ``keep`` (e.g. "1,3,4") explicitly keeps those 1-based MIDI channels as
    dedicated voices and folds every other channel into the kept voice
    closest in pitch — use this when one channel carries an intro the
    others rest through. When ``keep`` is empty, ``--max-channels`` /
    ``mode`` decide: 'merge' is pitch-aware (keeps the bass + lead apart,
    folds the closest inner voices), 'drop' discards the quietest voices.
    Either way the caller still runs remove_polyphony() afterwards.
    """
    # Empty tracks (e.g. the meta/tempo track) never become a voice.
    song.tracks = [t for t in song.tracks if getattr(t, "notes", None)]

    def _avg_pitch(t):
        return sum(n.note_num for n in t.notes) / len(t.notes)

    # --- Explicit channel selection -------------------------------------
    wanted = {int(x) - 1 for x in keep.replace(" ", "").split(",") if x.strip()}
    if wanted:
        kept = [t for t in song.tracks if getattr(t, "channel", -1) in wanted]
        rest = [t for t in song.tracks if getattr(t, "channel", -1) not in wanted]
        if not kept:
            sys.stderr.write(
                f"sid_to_sng: --keep-midi-channels {keep} matched no tracks; "
                f"falling back to automatic reduction.\n")
        else:
            for t in rest:                       # fold each into nearest kept
                tgt = min(kept, key=lambda k: abs(_avg_pitch(k) - _avg_pitch(t)))
                tgt.notes.extend(t.notes)
                tgt.notes.sort(key=lambda n: (n.start_time, -n.note_num))
            song.tracks = kept
            sys.stderr.write(
                f"sid_to_sng: kept MIDI channels {sorted(c + 1 for c in wanted)} "
                f"as dedicated voices; folded {len(rest)} other voice(s) in.\n")
            return

    if max_ch <= 0 or len(song.tracks) <= max_ch:
        return

    if mode == "drop":
        song.tracks.sort(key=lambda t: len(t.notes), reverse=True)
        dropped = len(song.tracks) - max_ch
        del song.tracks[max_ch:]
        sys.stderr.write(
            f"sid_to_sng: dropped the {dropped} quietest voice(s) to fit "
            f"{max_ch} channel(s).\n")
    else:  # merge — keep bass + lead apart, fold the closest inner voices
        merged = 0
        while len(song.tracks) > max_ch:
            song.tracks.sort(key=_avg_pitch)
            # adjacent pair (in pitch) with the smallest gap = the two most
            # similar voices; merging those keeps the outer voices intact.
            gaps = [(_avg_pitch(song.tracks[i + 1]) - _avg_pitch(song.tracks[i]), i)
                    for i in range(len(song.tracks) - 1)]
            _, i = min(gaps, key=lambda g: g[0])
            a = song.tracks.pop(i + 1)
            b = song.tracks[i]
            b.notes.extend(a.notes)
            b.notes.sort(key=lambda n: (n.start_time, -n.note_num))
            merged += 1
        sys.stderr.write(
            f"sid_to_sng: merged {merged} voice(s) to fit {max_ch} "
            f"channel(s) (top note wins on overlap).\n")


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
        # Optionally fold the song down to a target voice count (e.g. 3 for a
        # mono SID). Default (--max-channels 0) keeps every voice, so a 4+
        # voice tune becomes a 6-channel stereo .sng.
        _fit_channels(chirp_song, args.max_channels, args.reduce,
                      args.keep_midi_channels)
        # Tracker voices are monophonic, but a MIDI track can hold chords /
        # overlapping notes (and merging above can add some). to_rchirp()
        # refuses polyphonic input (ChiptuneSAKPolyphonyError), so flatten
        # each track to a single voice — remove_polyphony() keeps the top
        # note of any overlap and leaves the track count unchanged.
        if chirp_song.is_polyphonic():
            chirp_song.remove_polyphony()
            sys.stderr.write(
                "sid_to_sng: flattened polyphony to monophonic voices "
                "(kept the top note of any chord).\n")
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
