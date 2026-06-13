# goattracker-qt MCP server

Model Context Protocol bridge that re-exposes the editor's JSON-line
RPC surface as LLM-driven tools. Drop it into Claude Desktop / Claude
Code / any MCP client and the model can load songs, read pattern data,
fire menu actions, take screenshots, etc.

## Tools

| Tool | What it does |
|------|-------------|
| `load_song(path)` | Open a `.sng / .sid / .mid / .mod`. |
| `save_song(path)` | Save the buffer to a `.sng`. |
| `get_state()` | Editor mode / transport / cursor / channel state. |
| `eval_expr(expr)` | Read `pattlen[N]`, `pattern[N][R][C]`, `songorder[S][C][R]`. |
| `get_pattern(num, from_row?, to_row?)` | Dump a pattern as `(note, instr, cmd, param)` rows. |
| `get_instrument(num)` | Instrument fields + name. |
| `get_table(name, from_row?, to_row?)` | Wave / pulse / filter / speed table rows. |
| `get_order(subtune?, channel?)` | Orderlist for a channel. |
| `press_key(key, modifiers?)` | Send a single keystroke (F1, Ctrl+Z, ...). |
| `trigger_action(name)` | Invoke a menu action by visible label. |
| `screenshot(widget?)` | Base64 PNG of the editor (or a named subwidget). |
| `tick(frames?)` | Advance the deterministic UI tick. |
| `shutdown_editor()` | Kill the wrapped subprocess (next call respawns). |

## Install

```sh
cd ext/mcp-server
uv venv venv
uv pip install --python venv/bin/python -e .
```

`build/qt/goattrk2-qt` is auto-discovered; set `GT2_BINARY` to override.

## Wire to a client

Claude Desktop (`~/.config/Claude/claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "goattracker-qt": {
      "command": "/path/to/ext/mcp-server/venv/bin/goattrk2-mcp",
      "env": {
        "GT2_BINARY": "/path/to/build/qt/goattrk2-qt"
      }
    }
  }
}
```

Claude Code (`.claude/mcp.json` in the repo or `~/.claude/mcp.json`):

```json
{
  "mcpServers": {
    "goattracker-qt": {
      "command": "uv",
      "args": ["run", "--directory", "ext/mcp-server", "server.py"]
    }
  }
}
```

Restart the client. The model can now call any of the tools above.

## Notes

- The wrapped editor runs with `--platform offscreen` by default so it
  doesn't pop a window. Set `GT2_OFFSCREEN=0` if you want to drive a
  visible session.
- One editor instance is shared across tool calls. Call `shutdown_editor`
  to force a fresh process.
- Audio is initialised by the editor (PortAudio), but no sound is heard
  in offscreen mode — analysis uses the engine state, not the audio out.
