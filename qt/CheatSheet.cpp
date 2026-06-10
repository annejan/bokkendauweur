#include "CheatSheet.h"

// Compact HTML cheat sheet rendered in the Help > Command chart dialog
// via QTextBrowser. Uses Qt's HTML 4 subset; no JS, no external CSS.
QString cheatSheetHtml() {
    return QStringLiteral(R"HTML(
<html><head><style>
  body  { background:#1B202B; color:#E1E5EA;
          font-family: Helvetica, Arial, sans-serif; font-size: 12px; }
  h1    { color:#FFC46E; font-size: 16px; margin-top:4px; }
  h2    { color:#9CDDF5; font-size: 14px; margin-top:14px; margin-bottom:4px;
          border-bottom: 1px solid #2A3340; padding-bottom: 2px; }
  table { border-collapse: collapse; margin: 4px 0 8px 0; }
  th    { background:#2A3340; color:#9CDDF5; padding: 3px 7px;
          text-align: center; font-weight: bold; border: 1px solid #1B202B; }
  td    { background:#222831; color:#E1E5EA; padding: 2px 7px;
          border: 1px solid #1B202B; }
  td.k  { color:#FFC46E; font-family: monospace; font-weight: bold;
          text-align: center; min-width: 32px; }
  td.h  { color:#9CFF9C; font-family: monospace; text-align: center; }
  td.n  { color:#E1E5EA; }
  td.sub{ background:#2A3340; color:#FFC46E; text-align:center; font-weight:bold; }
  .grid td { font-family: monospace; text-align: center; min-width: 28px; }
  .small td { font-size: 11px; padding: 1px 5px; }
  .qt   { color:#C9A0FF; font-family: monospace; font-weight: bold; }
  p.note{ color:#B0BCC8; font-style: italic; margin: 2px 0; }
</style></head><body>

<h1>GoatTracker Qt — Command chart</h1>
<p class="note">Values verified against gpattern.c / gsid.c (v2.77 base).
Qt-frontend keyboard bindings listed at the bottom.</p>

<h2>Track effects</h2>
<table>
<tr><th>Cmd</th><th>What it does</th></tr>
<tr><td class="k">000</td><td>Do nothing.</td></tr>
<tr><td class="k">1ST</td><td>Slide up. ST is an index in the speed table (left + right columns combined).</td></tr>
<tr><td class="k">2ST</td><td>Slide down.</td></tr>
<tr><td class="k">3ST</td><td>Slide to note. ST = 00 slides instantly.</td></tr>
<tr><td class="k">4ST</td><td>Vibrato. Left col of ST index = freq, right col = amplitude.</td></tr>
<tr><td class="k">5AD</td><td>Set attack / decay.</td></tr>
<tr><td class="k">6SR</td><td>Set sustain / release.</td></tr>
<tr><td class="k">7XY</td><td>Set waveform register to XY. Wavetable takes precedence.</td></tr>
<tr><td class="k">8WT</td><td>Set wavetable index.</td></tr>
<tr><td class="k">9PT</td><td>Set pulsetable index.</td></tr>
<tr><td class="k">AFT</td><td>Set filtertable index.</td></tr>
<tr><td class="k">BRM</td><td>Set resonance to R and channel bitmask to M.</td></tr>
<tr><td class="k">CC0</td><td>Set filter cutoff to C0.</td></tr>
<tr><td class="k">DXY</td><td>Set master volume to Y. If X is non-zero, copies XY to timing-mark location (player address + 3F).</td></tr>
<tr><td class="k">EST</td><td>Global funk tempo. Shuffles between tempos at speedtable index ST (left + right).</td></tr>
<tr><td class="k">FXY</td><td>Set tempo. 03-7F = global. 83-FF = channel tempo + 80. 00-01 use the funk tempos set by the E command.</td></tr>
</table>

<h2>Wavetable — left column (waveform / commands)</h2>
<table>
<tr><th>Value</th><th>Meaning</th></tr>
<tr><td class="k">00</td><td>Null command.</td></tr>
<tr><td class="k">01-0F</td><td>Delay step by 1-15 frames.</td></tr>
<tr><td class="k">E0-EF</td><td>Inaudible.</td></tr>
<tr><td class="k">F0-FE</td><td>Execute track effect 0-E with right side as data.</td></tr>
<tr><td class="k">FF</td><td>Jump to table pos on right side.</td></tr>
<tr><th colspan="2">Values from here are bitmasks on the SID control register:</th></tr>
<tr><td class="k">x1</td><td>Gate + initiate attack/decay (0 here initiates sustain/release).</td></tr>
<tr><td class="k">x2</td><td>Hardsync. Ch1 uses Ch3, Ch2 uses Ch1, Ch3 uses Ch2.</td></tr>
<tr><td class="k">x4</td><td>Ringmod, channels as above.</td></tr>
<tr><td class="k">x8</td><td>Test bit. Resets oscillator.</td></tr>
<tr><td class="k">1x</td><td>Use triangle.</td></tr>
<tr><td class="k">2x</td><td>Use sawtooth.</td></tr>
<tr><td class="k">4x</td><td>Use pulsewave.</td></tr>
<tr><td class="k">8x</td><td>Use noise.</td></tr>
</table>

<h2>Wavetable — right column (notes)</h2>
<table>
<tr><th>Value</th><th>Meaning</th></tr>
<tr><td class="k">00-5F</td><td>Relative notes <i>upward</i> (see table below).</td></tr>
<tr><td class="k">7F-60</td><td>Relative notes <i>downward</i>.</td></tr>
<tr><td class="k">80</td><td>Unchanged note.</td></tr>
<tr><td class="k">81-DF</td><td>Absolute notes C#0 to B-7 (see table below).</td></tr>
</table>

<h2>Relative notes (wavetable right) — octave shift / interval</h2>
<table class="grid small">
<tr><th></th><th>-3</th><th>-2</th><th>-1</th><th>+0</th><th>+1</th><th>+2</th><th>+3</th><th>+4</th><th>+5</th><th>+6</th><th>+7</th></tr>
<tr><td class="sub">r</td><td>-</td><td class="h">68</td><td class="h">74</td><td class="h">00</td><td class="h">0C</td><td class="h">18</td><td class="h">24</td><td class="h">30</td><td class="h">3C</td><td class="h">48</td><td class="h">54</td></tr>
<tr><td class="sub">b2</td><td>-</td><td class="h">69</td><td class="h">75</td><td class="h">01</td><td class="h">0D</td><td class="h">19</td><td class="h">25</td><td class="h">31</td><td class="h">3D</td><td class="h">49</td><td class="h">55</td></tr>
<tr><td class="sub">2</td><td>-</td><td class="h">6A</td><td class="h">76</td><td class="h">02</td><td class="h">0E</td><td class="h">1A</td><td class="h">26</td><td class="h">32</td><td class="h">3E</td><td class="h">4A</td><td class="h">56</td></tr>
<tr><td class="sub">b3</td><td>-</td><td class="h">6B</td><td class="h">77</td><td class="h">03</td><td class="h">0F</td><td class="h">1B</td><td class="h">27</td><td class="h">33</td><td class="h">3F</td><td class="h">4B</td><td class="h">57</td></tr>
<tr><td class="sub">3</td><td class="h">60</td><td class="h">6C</td><td class="h">78</td><td class="h">04</td><td class="h">10</td><td class="h">1C</td><td class="h">28</td><td class="h">34</td><td class="h">40</td><td class="h">4C</td><td class="h">58</td></tr>
<tr><td class="sub">4</td><td class="h">61</td><td class="h">6D</td><td class="h">79</td><td class="h">05</td><td class="h">11</td><td class="h">1D</td><td class="h">29</td><td class="h">35</td><td class="h">41</td><td class="h">4D</td><td class="h">59</td></tr>
<tr><td class="sub">b5</td><td class="h">62</td><td class="h">6E</td><td class="h">7A</td><td class="h">06</td><td class="h">12</td><td class="h">1E</td><td class="h">2A</td><td class="h">36</td><td class="h">42</td><td class="h">4E</td><td class="h">5A</td></tr>
<tr><td class="sub">5</td><td class="h">63</td><td class="h">6F</td><td class="h">7B</td><td class="h">07</td><td class="h">13</td><td class="h">1F</td><td class="h">2B</td><td class="h">37</td><td class="h">43</td><td class="h">4F</td><td class="h">5B</td></tr>
<tr><td class="sub">b6</td><td class="h">64</td><td class="h">70</td><td class="h">7C</td><td class="h">08</td><td class="h">14</td><td class="h">20</td><td class="h">2C</td><td class="h">38</td><td class="h">44</td><td class="h">50</td><td class="h">5C</td></tr>
<tr><td class="sub">6</td><td class="h">65</td><td class="h">71</td><td class="h">7D</td><td class="h">09</td><td class="h">15</td><td class="h">21</td><td class="h">2D</td><td class="h">39</td><td class="h">45</td><td class="h">51</td><td class="h">5D</td></tr>
<tr><td class="sub">b7</td><td class="h">66</td><td class="h">72</td><td class="h">7E</td><td class="h">0A</td><td class="h">16</td><td class="h">22</td><td class="h">2E</td><td class="h">3A</td><td class="h">46</td><td class="h">52</td><td class="h">5E</td></tr>
<tr><td class="sub">7</td><td class="h">67</td><td class="h">73</td><td class="h">7F</td><td class="h">0B</td><td class="h">17</td><td class="h">23</td><td class="h">2F</td><td class="h">3B</td><td class="h">47</td><td class="h">53</td><td class="h">5F</td></tr>
</table>

<h2>Absolute notes (wavetable right) — note / octave</h2>
<table class="grid small">
<tr><th></th><th>C</th><th>C#</th><th>D</th><th>D#</th><th>E</th><th>F</th><th>F#</th><th>G</th><th>G#</th><th>A</th><th>A#</th><th>B</th></tr>
<tr><td class="sub">0</td><td>-</td><td class="h">81</td><td class="h">82</td><td class="h">83</td><td class="h">84</td><td class="h">85</td><td class="h">86</td><td class="h">87</td><td class="h">88</td><td class="h">89</td><td class="h">8A</td><td class="h">8B</td></tr>
<tr><td class="sub">1</td><td class="h">8C</td><td class="h">8D</td><td class="h">8E</td><td class="h">8F</td><td class="h">90</td><td class="h">91</td><td class="h">92</td><td class="h">93</td><td class="h">94</td><td class="h">95</td><td class="h">96</td><td class="h">97</td></tr>
<tr><td class="sub">2</td><td class="h">98</td><td class="h">99</td><td class="h">9A</td><td class="h">9B</td><td class="h">9C</td><td class="h">9D</td><td class="h">9E</td><td class="h">9F</td><td class="h">A0</td><td class="h">A1</td><td class="h">A2</td><td class="h">A3</td></tr>
<tr><td class="sub">3</td><td class="h">A4</td><td class="h">A5</td><td class="h">A6</td><td class="h">A7</td><td class="h">A8</td><td class="h">A9</td><td class="h">AA</td><td class="h">AB</td><td class="h">AC</td><td class="h">AD</td><td class="h">AE</td><td class="h">AF</td></tr>
<tr><td class="sub">4</td><td class="h">B0</td><td class="h">B1</td><td class="h">B2</td><td class="h">B3</td><td class="h">B4</td><td class="h">B5</td><td class="h">B6</td><td class="h">B7</td><td class="h">B8</td><td class="h">B9</td><td class="h">BA</td><td class="h">BB</td></tr>
<tr><td class="sub">5</td><td class="h">BC</td><td class="h">BD</td><td class="h">BE</td><td class="h">BF</td><td class="h">C0</td><td class="h">C1</td><td class="h">C2</td><td class="h">C3</td><td class="h">C4</td><td class="h">C5</td><td class="h">C6</td><td class="h">C7</td></tr>
<tr><td class="sub">6</td><td class="h">C8</td><td class="h">C9</td><td class="h">CA</td><td class="h">CB</td><td class="h">CC</td><td class="h">CD</td><td class="h">CE</td><td class="h">CF</td><td class="h">D0</td><td class="h">D1</td><td class="h">D2</td><td class="h">D3</td></tr>
<tr><td class="sub">7</td><td class="h">D4</td><td class="h">D5</td><td class="h">D6</td><td class="h">D7</td><td class="h">D8</td><td class="h">D9</td><td class="h">DA</td><td class="h">DB</td><td class="h">DC</td><td class="h">DD</td><td class="h">DE</td><td class="h">DF</td></tr>
</table>

<h2>Filtertable</h2>
<table>
<tr><th>Value</th><th>Meaning</th></tr>
<tr><td class="k">00</td><td>Set cutoff as right column.</td></tr>
<tr><td class="k">01-7F</td><td>Filter modulation step. Time in left column, signed* extent + direction in right column.</td></tr>
<tr><td class="k">80-F0</td><td>Filter configuration. Filter mode bitmask* in left column (filter can be in multiple modes); resonance is first value of right column and channel bitmask* is second value.</td></tr>
<tr><td class="k">FF</td><td>Jump to index in right column. FF 00 means stop.</td></tr>
</table>

<h2>Pulsetable</h2>
<table>
<tr><th>Value</th><th>Meaning</th></tr>
<tr><td class="k">01-7F</td><td>Pulse modulation step: time in left column; signed* speed in right.</td></tr>
<tr><td class="k">8X-FX</td><td>Set pulse width. X is high value, right column is low value.</td></tr>
<tr><td class="k">FF</td><td>Jump to index in right column. FF 00 stops the table.</td></tr>
</table>

<h2>Bitmasks</h2>
<table>
<tr><th>Filter mode</th><th>Channel</th></tr>
<tr><td>
  <table class="small">
  <tr><td class="k">80</td><td class="n">none</td></tr>
  <tr><td class="k">90</td><td class="n">LP</td></tr>
  <tr><td class="k">A0</td><td class="n">BP</td></tr>
  <tr><td class="k">B0</td><td class="n">LP + BP</td></tr>
  <tr><td class="k">C0</td><td class="n">HP</td></tr>
  <tr><td class="k">D0</td><td class="n">LP + HP</td></tr>
  <tr><td class="k">E0</td><td class="n">BP + HP</td></tr>
  <tr><td class="k">F0</td><td class="n">all</td></tr>
  </table>
</td><td>
  <table class="small">
  <tr><td class="k">0</td><td class="n">none</td></tr>
  <tr><td class="k">1</td><td class="n">1</td></tr>
  <tr><td class="k">2</td><td class="n">2</td></tr>
  <tr><td class="k">3</td><td class="n">1, 2</td></tr>
  <tr><td class="k">4</td><td class="n">3</td></tr>
  <tr><td class="k">5</td><td class="n">1, 3</td></tr>
  <tr><td class="k">6</td><td class="n">2, 3</td></tr>
  <tr><td class="k">7</td><td class="n">all</td></tr>
  </table>
</td></tr>
</table>

<h2>Chord spellings</h2>
<table class="small">
<tr><th></th><th>major</th><th>minor</th><th>dim</th><th>aug</th><th>sus4</th><th>dim7</th><th>7</th><th>mi7</th><th>b5</th><th>#5</th><th>b9</th><th>9</th><th>#9</th><th>11</th><th>#11</th><th>b13</th><th>13</th></tr>
<tr><td class="sub">root</td><td class="h">04 07</td><td class="h">03 07</td><td class="h">03 06</td><td class="h">04 08</td><td class="h">05 07</td><td class="h">03 06 09</td><td class="h">+0B</td><td class="h">+0A</td><td class="h">-07 +06</td><td class="h">-07 +08</td><td class="h">+0D</td><td class="h">+0E</td><td class="h">+0F</td><td class="h">+11</td><td class="h">+12</td><td class="h">+14</td><td class="h">+15</td></tr>
<tr><td class="sub">1st inv</td><td class="h">78 7B</td><td class="h">77 7B</td><td class="h">7A 7D</td><td class="h">78 7C</td><td class="h">79 7B</td><td class="h">77 7A 7D</td><td class="h">+7F</td><td class="h">+7E</td><td class="h">-7B +7A</td><td class="h">-7B +7C</td><td colspan="7"></td></tr>
<tr><td class="sub">2nd inv</td><td class="h">04 7B</td><td class="h">03 7B</td><td class="h">03 7D</td><td class="h">04 7C</td><td class="h">05 7B</td><td class="h">03 7A 7D</td><td colspan="11"></td></tr>
<tr><td class="sub">3rd</td><td colspan="5"></td><td class="h">03 06 7D</td><td colspan="11"></td></tr>
</table>

<h2>Signed values</h2>
<table>
<tr><td class="k">01 → 7F</td><td>Up</td></tr>
<tr><td class="k">FF → 80</td><td>Down</td></tr>
</table>

<h2>Qt frontend — keyboard shortcuts</h2>

<table>
<tr><th colspan="2">Pattern editor</th></tr>
<tr><td class="qt">Space</td><td>Toggle record mode (REC).</td></tr>
<tr><td class="qt">Ctrl+0 / 1 / 2</td><td>EDIT SKIP — rows the cursor advances after each input.</td></tr>
<tr><td class="qt">Ctrl+C / X / V</td><td>Copy / cut / paste selection (system clipboard, cross-app).</td></tr>
<tr><td class="qt">Ctrl+A</td><td>Select entire current channel.</td></tr>
<tr><td class="qt">Del</td><td>Clear selected cells (length unchanged).</td></tr>
<tr><td class="qt">Ctrl+Del</td><td>Remove selected rows (shift up).</td></tr>
<tr><td class="qt">Insert</td><td>Insert empty row at cursor (push-off or grow per Edit menu setting).</td></tr>
<tr><td class="qt">Ctrl+Backspace</td><td>Delete row at cursor.</td></tr>
<tr><td class="qt">Ctrl+Shift+Up / Down</td><td>Transpose selection ±1 octave.</td></tr>
<tr><td class="qt">Esc</td><td>Clear selection.</td></tr>
<tr><td class="qt">Backspace</td><td>Silence the active channel's playing note.</td></tr>
</table>

<table>
<tr><th colspan="2">Order / song editor</th></tr>
<tr><td class="qt">Insert</td><td>Insert empty cell (this channel only).</td></tr>
<tr><td class="qt">Shift+Insert</td><td>Insert empty row across <i>all</i> channels.</td></tr>
<tr><td class="qt">Delete</td><td>Delete cell (this channel only).</td></tr>
<tr><td class="qt">Shift+Delete</td><td>Delete row across <i>all</i> channels.</td></tr>
<tr><td class="qt">Click cell + type</td><td>Type the hex pattern number directly.</td></tr>
</table>

<table>
<tr><th colspan="2">File</th></tr>
<tr><td class="qt">Ctrl+N</td><td>New song (discard current).</td></tr>
<tr><td class="qt">Ctrl+O</td><td>Open .sng / .sid / .mid / .mod.</td></tr>
<tr><td class="qt">Ctrl+S</td><td>Save.</td></tr>
<tr><td class="qt">Ctrl+Shift+S</td><td>Save As.</td></tr>
<tr><td class="qt">F9</td><td>Pack to PRG / SID / BIN.</td></tr>
</table>

<table>
<tr><th colspan="2">Transport + view</th></tr>
<tr><td class="qt">F1 / F2 / F3 / F4</td><td>Play from start / play from cursor (Pause toggle) / play pattern / stop.</td></tr>
<tr><td class="qt">F5 / F6 / F7 / F8</td><td>Switch to Pattern / Order / Instrument / Tables editor.</td></tr>
<tr><td class="qt">Ctrl+F</td><td>Toggle follow-play.</td></tr>
<tr><td class="qt">Numpad + / −</td><td>Octave up / down.</td></tr>
</table>

<p class="note">Settings → Note entry layout chooses Protracker (default) /
DMC / Janko. Settings → 'Use physical (scancode) note layout' keeps
Z…M as the chromatic bottom row even on Dvorak / AZERTY / Colemak.</p>

<p class="note">Original Ravenspiral chart by Covert Bitops fans —
www.ravenspiral.com. This rendering re-typesets that content for the
Qt edition and adds the editor's own bindings.</p>

</body></html>
)HTML");
}
