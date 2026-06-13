#!/usr/bin/env python3
# GoatTracker Qt MCP server
# ------------------------------------------------------------------
# Wraps a long-running `goattrk2-qt --rpc --platform offscreen` child
# and re-exposes its JSON-line RPC surface as MCP tools so an LLM
# (Claude Desktop, Claude Code, etc.) can drive the tracker:
#   - load / save songs
#   - import .sid / .mid / .mod via the ChiptuneSAK wrapper
#   - read pattern / order / instrument / table data
#   - write pattern cells, transpose, set instrument fields
#   - take screenshots of the editor
#   - send keyboard / action input
#
# stdio transport so it slots into the standard MCP client config
# (claude_desktop_config.json / .claude/mcp.json).
#
# Env vars:
#   GT2_BINARY  - path to goattrk2-qt (default: searches PATH)
#   GT2_OFFSCREEN - '1' to force --platform offscreen (default: '1')
#
# Run directly:
#   uv run server.py
# Or install:
#   uv pip install -e .
#   goattrk2-mcp

from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess
from contextlib import asynccontextmanager
from dataclasses import dataclass
from typing import Any

from mcp.server.fastmcp import FastMCP


# ----- child-process plumbing --------------------------------------

@dataclass
class RpcChild:
    proc: subprocess.Popen
    lock: asyncio.Lock

    async def call(self, cmd: dict) -> dict:
        """Send a JSON command, return the JSON reply."""
        async with self.lock:
            line = json.dumps(cmd) + "\n"
            assert self.proc.stdin is not None
            assert self.proc.stdout is not None
            self.proc.stdin.write(line.encode())
            self.proc.stdin.flush()
            while True:
                raw = self.proc.stdout.readline()
                if not raw:
                    raise RuntimeError("goattrk2-qt RPC pipe closed")
                try:
                    obj = json.loads(raw.decode())
                except json.JSONDecodeError:
                    # 'ready' banner or qInfo log line — skip until we
                    # see a JSON reply for our command.
                    continue
                if obj.get("event") == "ready":
                    continue
                return obj


def _find_binary() -> str:
    env = os.environ.get("GT2_BINARY")
    if env:
        if not os.path.isfile(env):
            raise FileNotFoundError(f"GT2_BINARY set but not a file: {env}")
        return env
    found = shutil.which("goattrk2-qt")
    if found:
        return found
    # Fall back to the in-tree build location.
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.normpath(os.path.join(here, "..", "..", "build", "qt", "goattrk2-qt"))
    if os.path.isfile(cand):
        return cand
    raise FileNotFoundError(
        "Could not locate goattrk2-qt. Set $GT2_BINARY to the editor path "
        "or build it (cmake --build build) so build/qt/goattrk2-qt exists.")


def _spawn() -> RpcChild:
    binary = _find_binary()
    args = [binary, "--rpc"]
    if os.environ.get("GT2_OFFSCREEN", "1") == "1":
        args += ["--platform", "offscreen"]
    proc = subprocess.Popen(
        args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        bufsize=0,
    )
    return RpcChild(proc=proc, lock=asyncio.Lock())


# ----- MCP server ---------------------------------------------------

mcp = FastMCP("goattracker-qt")
_child: RpcChild | None = None


def _rpc() -> RpcChild:
    global _child
    if _child is None or _child.proc.poll() is not None:
        _child = _spawn()
    return _child


@mcp.tool()
async def load_song(path: str) -> dict:
    """Load a .sng / .sid / .mid / .mod into the editor.

    SID / MIDI / MOD imports require ChiptuneSAK to be importable by
    the editor's Python (see README). Returns {ok, loaded} on success.
    """
    return await _rpc().call({"cmd": "load", "path": path})


@mcp.tool()
async def save_song(path: str) -> dict:
    """Save the in-memory song to a .sng file."""
    return await _rpc().call({"cmd": "save", "path": path})


@mcp.tool()
async def get_state() -> dict:
    """Return editor state: editmode, transport, cursor, channels, etc."""
    return await _rpc().call({"cmd": "state"})


@mcp.tool()
async def eval_expr(expr: str) -> dict:
    """Read a song value by expression. Supported forms:
       - 'pattlen[N]'           -> rows in pattern N
       - 'pattern[N][R][C]'     -> byte at pattern N row R column C
                                   (C: 0=note, 1=instr, 2=cmd, 3=param)
       - 'songorder[S][C][R]'   -> orderlist byte"""
    return await _rpc().call({"cmd": "eval", "expr": expr})


@mcp.tool()
async def get_pattern(num: int, from_row: int = 0, to_row: int = -1) -> dict:
    """Return rows of pattern `num` (0..207). to_row=-1 returns the full
    pattern. Each row is (note, instr, cmd, param) hex strings."""
    args: dict[str, Any] = {"cmd": "pattern", "num": num, "from": from_row}
    if to_row >= 0:
        args["to"] = to_row
    return await _rpc().call(args)


@mcp.tool()
async def get_instrument(num: int) -> dict:
    """Return instrument fields: ad, sr, wt/pt/ft/st pointers, vibdelay,
    gatetimer, firstwave, name."""
    return await _rpc().call({"cmd": "instr", "num": num})


@mcp.tool()
async def get_table(name: str, from_row: int = 1, to_row: int = -1) -> dict:
    """Return rows from a side table. name: 'wave' | 'pulse' | 'filter' |
    'speed'. Each row is (L, R) hex bytes."""
    args: dict[str, Any] = {"cmd": "table", "name": name, "from": from_row}
    if to_row >= 0:
        args["to"] = to_row
    return await _rpc().call(args)


@mcp.tool()
async def get_order(subtune: int = 0, channel: int = 0) -> dict:
    """Return the orderlist for subtune (0..31) channel (0..5).
    Includes the trailing LOOPSONG marker + restart pos."""
    return await _rpc().call({"cmd": "order", "subtune": subtune, "channel": channel})


@mcp.tool()
async def press_key(key: str, modifiers: list[str] | None = None) -> dict:
    """Send a single keystroke to the focused editor widget. Examples:
       press_key('F1')                       -> play from beginning
       press_key('z', modifiers=['ctrl'])    -> undo
       press_key('Insert', modifiers=['shift']) -> Order > insert row
                                                  across all channels"""
    args: dict[str, Any] = {"cmd": "key", "key": key}
    if modifiers:
        args["modifiers"] = modifiers
    return await _rpc().call(args)


@mcp.tool()
async def trigger_action(name: str) -> dict:
    """Trigger a menu action by visible label (case-insensitive).
    Examples: 'Play from beginning', 'Pattern editor',
    'Order/song editor', 'Tables editor', 'Instrument editor'."""
    return await _rpc().call({"cmd": "action", "name": name})


@mcp.tool()
async def screenshot(widget: str | None = None) -> dict:
    """Capture the editor (or a named widget) as a base64 PNG.
    Returns {png_b64, width, height}. The MCP client decodes the
    base64 string into an image for display."""
    args: dict[str, Any] = {"cmd": "screenshot"}
    if widget:
        args["widget"] = widget
    return await _rpc().call(args)


@mcp.tool()
async def tick(frames: int = 1) -> dict:
    """Advance the editor's deterministic UI tick by `frames` ticks.
    Useful for stepping playback in a test harness without sleeping."""
    return await _rpc().call({"cmd": "tick", "frames": frames})


@mcp.tool()
async def shutdown_editor() -> dict:
    """Stop the wrapped goattrk2-qt subprocess. The next tool call
    spawns a fresh editor instance."""
    global _child
    if _child is None:
        return {"ok": True, "note": "no editor running"}
    try:
        await _child.call({"cmd": "quit"})
    except Exception:
        pass
    try:
        _child.proc.terminate()
        _child.proc.wait(timeout=2)
    except Exception:
        try:
            _child.proc.kill()
        except Exception:
            pass
    _child = None
    return {"ok": True}


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
