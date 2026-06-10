# Accessibility plan — making the editor usable for blind musicians

Status: proposal. This document describes how to make the Qt frontend
usable with a screen reader / by ear, and the staged PRs to get there.

## Why this is worth doing

Trackers have a long tradition of blind users — the rigid keyboard-driven
grid maps well to speech, and a SID tracker that talks is essentially
unheard of. The engine is already fully keyboard-operable, so most of the
work is *exposing state to assistive technology*, not reworking input.

## Current state

The app does **not** use Qt's accessibility layer at all (no
`QAccessible*`, no `setAccessibleName`). What that means today:

| Area | Widget kind | Screen-reader status |
| ---- | ----------- | -------------------- |
| Menus, dialogs | native Qt | works out of the box |
| Order / song editor | `QTableView` | mostly works (cells are read) |
| Song name fields | `QLineEdit` | works |
| Instrument editor | spin boxes / combos + custom ADSR graph | partial |
| **Pattern editor** | `QAbstractScrollArea`, hand-painted | **opaque — nothing is exposed** |
| Order map, VU/scope strips, status strip, tables decoder | custom paint | opaque |

The heart of the app — the pattern grid — is invisible to assistive tech
because it is drawn manually onto a canvas with no accessible children.

## Background: how Qt accessibility works

Qt bridges widgets to the platform screen reader: **AT-SPI2 → Orca**
(Linux), **UI Automation → NVDA / JAWS / Narrator** (Windows),
**NSAccessibility → VoiceOver** (macOS). Standard widgets ship a
`QAccessibleInterface` and are exposed automatically. Custom-painted
widgets expose nothing unless we provide one.

There are two ways to give the grid a voice:

- **A. `QAccessibleInterface` (+ table interface)** — the "proper" route.
  Expose the pattern as an accessible table so the user's own screen
  reader reads cells as they arrow around. Maximum interoperability,
  fiddly to implement, and the user must run (and tune) a screen reader.
- **B. Self-voicing via `QTextToSpeech`** — the app speaks the cell under
  the cursor itself (e.g. *"row 16, channel 1, C-4, instrument 0A"*) using
  the platform TTS (speech-dispatcher / SAPI / NSSpeechSynthesizer). We
  control verbosity and timing, it needs no screen reader running, and it
  sidesteps the custom-paint problem.

For a tracker, **B is the pragmatic primary path** — it is how accessible
trackers (Schism, OpenMPT add-ons) work in practice. A can be added later
for users who prefer their native screen reader.

## Design

### Speech service

A small singleton wrapping `QTextToSpeech`:

- `say(text, Priority)` — `Priority::Cursor` interrupts the previous
  utterance (so rapid arrowing doesn't queue a backlog); `Priority::Status`
  is queued.
- Built only when Qt's TextToSpeech module is present — see *Optional
  dependency* below — so it never becomes a hard build requirement.

### What gets spoken

Pattern editor (the focus of v1):

- **On cursor move** — speak the cell under the cursor. Two verbosity
  levels: *terse* (just the field that changed: `"C-4"`, `"instr 0A"`,
  `"cmd 4 21"`) and *full row* (`"row 16, channel 1, C-4, instrument 0A,
  no command"`). Vertical moves read the note; horizontal moves read the
  field entered.
- **On edit** — confirm the value written.
- **On demand** — a hotkey to re-read the current row, and a "where am I"
  hotkey (`pattern N, row R of L, channel C, octave O`).
- **Boundaries** — short spoken/earcon cue at pattern start/end and on
  channel mute/unmute.

Transport / global:

- Announce play / pause / stop and (optionally, low verbosity) the playing
  row during follow-play.

### Verbosity & control

- A `View ▸ Speak cursor (accessibility)` toggle, persisted via the
  existing `QSettings`.
- Verbosity setting (off / terse / full).
- Optional "announce playback position" toggle (off by default — it is
  chatty).

### Accessible names (cheap win, all widgets)

Independently of speech, set `accessibleName` / `accessibleDescription`
on the chrome so a native screen reader can navigate it: transport
buttons, dock toggles, the status-strip clickable segments (SID model,
PAL/NTSC), and a role/name on each main view. This helps route A users
immediately and is near-zero risk.

### Optional dependency

`QTextToSpeech` lives in a separate Qt module. To keep the editor building
everywhere:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt6 QUIET OPTIONAL_COMPONENTS TextToSpeech)
if(Qt6TextToSpeech_FOUND)
    target_link_libraries(goattrk2-qt PRIVATE Qt6::TextToSpeech)
    target_compile_definitions(goattrk2-qt PRIVATE GT2_HAVE_TTS=1)
endif()
```

All speech code sits behind `#ifdef GT2_HAVE_TTS`; without the module the
app builds and runs exactly as today, just silent. Linux needs
`speech-dispatcher` at runtime (with an `espeak-ng` / `flite` backend).

## Rollout (staged PRs)

1. **Foundations** — `accessibleName` / `accessibleDescription` on chrome,
   verified focus order and full keyboard reachability. No new deps.
2. **Self-voicing v1** — optional `QTextToSpeech` dep, the speech service,
   pattern-cursor + edit announcements behind the `View` toggle, "read
   row" / "where am I" hotkeys, settings persisted.
3. **Transport & boundaries** — play/pause/stop announcements, optional
   playback-row read-out, mute and pattern-edge cues.
4. **Native screen-reader route (A)** — a `QAccessibleInterface` table
   bridge for the pattern grid, for users who prefer Orca / NVDA /
   VoiceOver over self-voicing.
5. **Polish** — earcons, per-field verbosity, docs + a short "using the
   editor by ear" guide.

## Testing

- Linux: `speech-dispatcher` + `espeak-ng`, plus Orca for route A.
- Windows: Narrator / NVDA.
- macOS: VoiceOver.
- A `--speak-selftest` style smoke check so CI can at least confirm the
  speech path links and initialises (no audio assertion).

## Non-goals (for now)

- Re-skinning the visual UI. This is additive; sighted use is unchanged.
- Brailledisplay-specific formatting (comes naturally via route A later).
