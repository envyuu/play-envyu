'use strict';
/* Tables and note rules live in fxdata.js. */
const $ = id => document.getElementById(id);
const el = (tag, props = {}, ...kids) => { const e = Object.assign(document.createElement(tag), props); e.append(...kids); return e; };
const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

/* Set by devicemap.js, so that the schematic follows the demos. */
let mapSync = () => {};

/* ---------- Audio preview ---------- */
let ctx, master, stopAll = () => {};
function audio() {
  if (!ctx) { ctx = new (window.AudioContext || window.webkitAudioContext)(); master = ctx.createGain(); master.gain.value = .35; master.connect(ctx.destination); }
  if (ctx.state === 'suspended') ctx.resume();
  return ctx;
}
const hz = n => 440 * 2 ** ((n - 69) / 12);
const secPerTick = 60 / 120 / 96;
function pluck(t, note, vel, dur, nodes) {
  const o = ctx.createOscillator(), g = ctx.createGain();
  o.type = 'triangle'; o.frequency.value = hz(note);
  const a = vel / 127;
  g.gain.setValueAtTime(a, t); g.gain.exponentialRampToValueAtTime(.001, t + Math.max(.08, dur));
  o.connect(g); g.connect(master); o.start(t); o.stop(t + Math.max(.1, dur) + .02); nodes.push(o);
}
function playButton(btn, makeNotes, seconds) {
  btn.onclick = () => {
    const wasMine = btn.dataset.on === '1';
    stopAll();
    if (wasMine) return;
    audio();
    const nodes = []; const t0 = ctx.currentTime + .05;
    const extra = makeNotes(t0, nodes);
    btn.dataset.on = '1'; btn.textContent = '■ Stop';
    const timer = setTimeout(() => stopAll(), (seconds() + .4) * 1000);
    stopAll = () => {
      clearTimeout(timer); nodes.forEach(n => { try { n.stop(); } catch (_) {} }); if (extra) extra();
      btn.dataset.on = ''; btn.textContent = '▶ Hear it'; stopAll = () => {};
    };
  };
}

/* ---------- Note charts ---------- */
function roll(notes, total, opts = {}) {
  const W = 640, H = 220, L = 38, B = 22, T = 8;
  const NS = 'http://www.w3.org/2000/svg';
  const s = (tag, a) => { const e = document.createElementNS(NS, tag); for (const k in a) e.setAttribute(k, a[k]); return e; };
  const svg = s('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': opts.label || 'Note timeline' });
  const ns = notes.map(n => n.note); const lo = Math.min(...ns, 60) - 1, hi = Math.max(...ns, 60) + 1, span = Math.max(hi - lo + 1, 8);
  const x = t => L + t / total * (W - L - 6), y = n => T + (hi - n) / span * (H - T - B), rowH = Math.min((H - T - B) / span, 18);
  const step = opts.grid || 24;
  const bold = opts.bold || step * 4;
  for (let t = 0; t <= total + .01; t += step) svg.append(s('line', { x1: x(t), x2: x(t), y1: T, y2: H - B, stroke: css('--line'), 'stroke-width': t % bold === 0 ? 1 : .4 }));
  [lo + 1, hi - 1].filter((v, i, a) => a.indexOf(v) === i).forEach(n => { const tx = s('text', { x: 2, y: y(n) + rowH * .8 }); tx.textContent = noteName(n); svg.append(tx); });
  if (opts.mark != null) { svg.append(s('line', { x1: x(opts.mark), x2: x(opts.mark), y1: T, y2: H - B, stroke: css('--accent2'), 'stroke-width': 2, 'stroke-dasharray': '5 4' })); const m = s('text', { x: x(opts.mark) + 5, y: T + 11 }); m.textContent = opts.markLabel || ''; svg.append(m); }
  notes.forEach(n => {
    const w = Math.max(3, x(n.t + n.dur) - x(n.t));
    svg.append(s('rect', { x: x(n.t), y: y(n.note), width: Math.min(w, W - 6 - x(n.t)), height: rowH, rx: 2, fill: n.kind === 'src' ? css('--text') : n.kind === 'dim' ? css('--muted') : n.kind === 'out' ? css('--danger') : css('--accent'), opacity: n.kind === 'src' || n.kind === 'dim' ? (n.kind === 'dim' ? .45 : 1) : clamp(.25 + n.vel / 127 * .85, .25, 1) }));
  });
  const lb = s('text', { x: L, y: H - 6 }); lb.textContent = opts.axis || ''; svg.append(lb);
  return svg;
}

function lfoChart(c) {
  const W = 640, H = 220, L = 34, B = 22, T = 8, total = c[2] >= 384 ? c[2] * (c[1] === 'once' ? 1.25 : 2) : 384;
  const NS = 'http://www.w3.org/2000/svg', s = (t, a) => { const e = document.createElementNS(NS, t); for (const k in a) e.setAttribute(k, a[k]); return e; };
  const svg = s('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': `CC value over time for ${c[5]}` });
  const x = t => L + t / total * (W - L - 6), y = v => T + (127 - v) / 127 * (H - T - B);
  [0, 64, 127].forEach(v => { svg.append(s('line', { x1: L, x2: W - 6, y1: y(v), y2: y(v), stroke: css('--line'), 'stroke-width': .6 })); const tx = s('text', { x: 4, y: y(v) + 4 }); tx.textContent = v; svg.append(tx); });
  let d = '';
  for (let t = 0; t <= total; t += 1) d += (t ? 'L' : 'M') + x(t).toFixed(1) + ' ' + y(lfoValue(c, t, RND)).toFixed(1);
  svg.append(s('path', { d, fill: 'none', stroke: css('--accent'), 'stroke-width': 2.4, 'stroke-linejoin': 'round' }));
  const lb = s('text', { x: L, y: H - 6 }); lb.textContent = total >= 768 ? `${total / 384} bars` : '1 bar'; svg.append(lb);
  return svg;
}

/* ---------- Perform panel ---------- */
const fx = { kind: 'echo', pad: { echo: 1, loop: 2, cc: 2 }, held: false };
/* The firmware renames the pads only; the category stays Delay, Loop or Reverb. */
const FX_CATEGORY = { echo: 'Delay', loop: 'Loop', cc: 'Reverb' };
const padNames = () => fx.kind === 'echo' ? ECHO.map(e => e[5]) : fx.kind === 'cc' ? LFO.map(e => e[5]) : [...LOOPNAME, ...LOOPNAME.map(n => n + ' LR')];
function drawFx() {
  PD.perform($('fxDisp'), { label: FX_CATEGORY[fx.kind], value: padNames()[fx.pad[fx.kind]], held: fx.held, hint: fx.held ? '' : 'Hold a pad' });
  $('fxPads').querySelectorAll('.pad').forEach(b => b.classList.toggle('down', fx.held && b.getAttribute('aria-pressed') === 'true'));
}
function holdPad(i) {
  fx.pad[fx.kind] = i; fx.held = true; stopAll();
  $('fxPads').querySelectorAll('.pad').forEach(b => b.setAttribute('aria-pressed', String(+b.dataset.i === i)));
  renderFx(); drawFx();
}
function releasePad() { if (fx.held) { fx.held = false; drawFx(); } }
['pointerup', 'pointercancel', 'blur'].forEach(ev => window.addEventListener(ev, releasePad));
window.addEventListener('keyup', e => { if (e.key === ' ' || e.key === 'Enter') releasePad(); });
function renderPads() {
  const names = padNames();
  const g = $('fxPads'); g.replaceChildren();
  for (let row = 0; row < 8; row++) for (const off of [0, 8]) {
    const i = row + off;
    const b = el('button', { type: 'button', className: 'pad', textContent: names[i] });
    b.dataset.i = i;
    b.addEventListener('pointerdown', e => { if (e.button <= 0) holdPad(i); });
    b.addEventListener('keydown', e => { if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) { e.preventDefault(); holdPad(i); } });
    b.setAttribute('aria-pressed', String(fx.pad[fx.kind] === i)); g.append(b);
  }
}
function renderFx() {
  const p = fx.pad[fx.kind], chart = $('fxChart');
  if (fx.kind === 'echo') {
    const c = ECHO[p], notes = echoNotes(c), total = Math.max(192, notes[notes.length - 1].t + 36);
    $('fxTitle').textContent = c[5];
    chart.replaceChildren(roll(notes, total, { label: `Echoes of ${c[5]}`, axis: 'White: your note. Amber: echoes, fading with velocity.' }));
    const vel = notes.slice(1).map(n => n.vel).join(' · ');
    $('fxText').textContent = `${c[1]} repeats, ${c[0]} ticks apart. Velocity from 100: ${vel}.` + (c[3] ? ` Pitch ${c[4] ? 'alternates by' : 'moves by'} ${Math.abs(c[3])} semitones.` : '');
  } else if (fx.kind === 'loop') {
    const r = loopNotes(p);
    $('fxTitle').textContent = (LOOPNAME[p & 7]) + (r.rev ? ' LR' : '');
    chart.replaceChildren(roll(r.notes, 16 * 24, { mark: r.holdAt, markLabel: 'pad held', label: `Loop timeline for ${LOOPNAME[p & 7]}`, axis: 'Grey: pattern notes, silent while you hold. Amber: the loop.' }));
    $('fxText').textContent = r.cap.length ? `Repeats the last ${r.len} ticks before the pad went down${r.rev ? ', backwards' : ''}. Let go and the pattern continues in the right place.` : `This short window holds no note at this moment, so you hear silence until you let go. That is expected.`;
  } else {
    const c = LFO[p];
    $('fxTitle').textContent = c[5];
    chart.replaceChildren(lfoChart(c));
    const shape = { tri: 'Triangle', up: 'Rising ramp', dn: 'Falling ramp', sq: 'Square', sh: 'Random steps', once: 'One-time rise' }[c[1]];
    $('fxText').textContent = `${shape} on ${CCN[c[0]]}, ${c[3]} to ${c[4]}, every ${PER[c[2]] || c[2] + ' ticks'}.`;
  }
}
document.querySelectorAll('[data-fx]').forEach(t => t.onclick = () => {
  fx.kind = t.dataset.fx; stopAll();
  document.querySelectorAll('[data-fx]').forEach(o => o.setAttribute('aria-selected', String(o === t)));
  renderPads(); renderFx(); drawFx();
});
playButton($('fxPlay'), (t0, nodes) => {
  const p = fx.pad[fx.kind];
  if (fx.kind === 'echo') echoNotes(ECHO[p]).forEach(n => pluck(t0 + n.t * secPerTick, n.note, n.vel, Math.max(.12, n.dur * secPerTick * 2), nodes));
  else if (fx.kind === 'loop') { const r = loopNotes(p); r.notes.filter(n => n.kind === 'src' || !n.kind).forEach(n => pluck(t0 + n.t * secPerTick, n.note, n.vel, .16, nodes)); }
  else {
    const c = LFO[p], total = 768, o = ctx.createOscillator(), f = ctx.createBiquadFilter(), g = ctx.createGain(), pan = ctx.createStereoPanner();
    o.type = 'sawtooth'; o.frequency.value = 110; f.type = 'lowpass'; f.frequency.value = 2500; f.Q.value = 2; g.gain.value = .28;
    for (let t = 0; t <= total; t += 2) {
      const v = lfoValue(c, t, RND), at = t0 + t * secPerTick;
      if (c[0] === 74) f.frequency.setValueAtTime(100 + (v / 127) ** 2 * 7000, at);
      if (c[0] === 71) f.Q.setValueAtTime(.5 + v / 127 * 18, at);
      if (c[0] === 10) pan.pan.setValueAtTime(v / 63.5 - 1, at);
      if (c[0] === 7) g.gain.setValueAtTime(.28 * v / 127, at);
      if (c[0] === 1) o.detune.setValueAtTime(Math.sin(t / 6) * v / 127 * 60, at);
    }
    o.connect(f); f.connect(pan); pan.connect(g); g.connect(master); o.start(t0); o.stop(t0 + total * secPerTick); nodes.push(o);
  }
}, () => fx.kind === 'cc' ? 768 * secPerTick : fx.kind === 'loop' ? 384 * secPerTick : 3);

/* ---------- Arp (arp.c ARPDEF, pick) ---------- */
ARP.forEach((a, i) => $('arpMode').append(el('option', { value: i, textContent: a[0] })));
CHORDS.forEach((c, i) => $('arpChord').append(el('option', { value: i, textContent: c[0] })));
$('arpChord').value = 3;
$('arpMode').value = 1;
const arpUi = { focus: 0 };
const arpNotes = () => arpPattern(+$('arpMode').value, +$('arpChord').value, $('arpOct').checked);
function renderArp() {
  const r = arpNotes(); $('arpTitle').textContent = r.a[0];
  $('arpChart').replaceChildren(roll(r.out, r.total, { label: `Arp pattern ${r.a[0]}`, axis: `${r.total / 96} beats, grid lines at 1/16.` + (r.a[1] === 'strum' || r.a[1] === 'off' ? '' : ' Note length is 3/4 of the rate.') }));
  const desc = { off: 'Arp off: the step plays its chord as usual.', up: 'Low to high, then again.', dn: 'High to low, then again.', ud: 'Up and back down without repeating the ends.', rnd: 'Random chord tones. Different each time on the device.', conv: 'Alternates outside in: low, high, next low, next high.', div: 'Starts in the middle and moves outwards.', pink: 'Walks up, returning to the top note after every tone.', strum: 'Plays each chord tone once, 3 ticks apart, then lets them ring.' }[r.a[1]];
  $('arpText').textContent = `${desc} ${r.set.length} ${r.set.length === 1 ? 'note' : 'notes'}: ${r.set.map(noteName).join(' ')}.`;
  drawArp();
}
const selIndex = s => s.selectedIndex, selFrac = s => s.options.length > 1 ? s.selectedIndex / (s.options.length - 1) : 0;
const CHORD_SHORT = c => c.startsWith('None') ? 'None' : c;
const arpSelect = () => $(arpUi.focus ? 'arpMode' : 'arpChord');
function arpValue() { const m = $('arpMode'); return m.selectedOptions[0].textContent + ($('arpOct').checked && +m.value ? ' +Oct' : ''); }
const arpKnobCfg = {
  name: 'Chord knob', max: () => arpSelect().options.length - 1, get: () => selIndex(arpSelect()),
  set: v => { arpSelect().selectedIndex = v; stopAll(); renderArp(); },
  text: () => arpUi.focus ? 'Arp ' + arpValue() : 'Chord ' + CHORD_SHORT($('arpChord').selectedOptions[0].textContent),
  onDouble: () => { arpUi.focus ^= 1; drawArp(); },
};
const arpKnob = PD.knob($('arpKnob'), arpKnobCfg);
const arpScene = () => ({ focus: arpUi.focus, blocks: [
  { label: 'Chord', value: CHORD_SHORT($('arpChord').selectedOptions[0].textContent), frac: selFrac($('arpChord')) },
  { label: 'Arp', value: arpValue(), frac: selFrac($('arpMode')) },
] });
function drawArp() {
  PD.param($('arpDisp'), arpScene());
  arpKnob.update();
  $('arpHint').textContent = arpUi.focus ? 'Turn to choose a pattern. Double-tap to go back to Chord.' : 'Turn to change the chord. Double-tap the knob to reach Arp.';
  mapSync();
}
['arpMode', 'arpChord', 'arpOct'].forEach(i => $(i).onchange = () => { stopAll(); renderArp(); });
playButton($('arpPlay'), (t0, nodes) => { const r = arpNotes(); for (let k = 0; k < 2; k++) r.out.forEach(n => pluck(t0 + (n.t + k * r.total) * secPerTick, n.note, n.vel, Math.max(.1, n.dur * secPerTick * 1.3), nodes)); }, () => arpNotes().total * 2 * secPerTick);

/* ---------- Note In Scale (nis.c) ---------- */
SCALES.forEach((s, i) => $('nisScale').append(el('option', { value: i, textContent: 'C ' + s[0] })));
$('nisScale').value = 2;
let nisSeed = 7;
const nisNotes = () => nisPattern(+$('nisScale').value, +$('nisPct').value, $('nisType').value === '1', nisSeed);
function renderNis() {
  const name = SCALES[+$('nisScale').value][0]; $('nisPctOut').textContent = $('nisPct').value + '%';
  $('nisT0').textContent = `In C ${name}`; $('nisT1').textContent = `In C ${name}+`;
  const r = nisNotes(); $('nisTitle').textContent = $('nisType').selectedOptions[0].textContent;
  $('nisChart').replaceChildren(roll(r.notes, 16 * 24, { label: 'Randomized pattern', axis: 'White: unchanged. Amber: new note in scale. Red: outside the scale.' }));
  $('nisText').textContent = `${r.changed} of 16 steps changed` + (r.outside ? `, ${r.outside} of them outside the scale.` : '.') + ($('nisType').value === '1' ? ' Above 50%, a share of changed steps leaves the scale: (percent − 50) ÷ 2, at most 25%.' : '');
  drawNis();
}
const nisUi = { focus: 0 };
const nisKnobCfg = {
  name: 'Randomize knob', px: 6, max: () => nisUi.focus ? 1 : 20,
  get: () => nisUi.focus ? +$('nisType').value : Math.round(+$('nisPct').value / 5),
  set: v => { if (nisUi.focus) $('nisType').value = v; else $('nisPct').value = v * 5; stopAll(); renderNis(); },
  text: () => nisUi.focus ? 'Randomize Type ' + $('nisType').selectedOptions[0].textContent : 'Randomize ' + $('nisPct').value + '%',
  onDouble: () => { nisUi.focus ^= 1; drawNis(); },
};
const nisKnob = PD.knob($('nisKnob'), nisKnobCfg);
const nisScene = () => ({ focus: nisUi.focus, blocks: [
  { label: 'Randomize', value: $('nisPct').value + '%', frac: +$('nisPct').value / 100 },
  { label: 'Randomize Type', value: $('nisType').selectedOptions[0].textContent, frac: +$('nisType').value },
] });
function drawNis() {
  PD.param($('nisDisp'), nisScene());
  nisKnob.update();
  $('nisHint').textContent = nisUi.focus ? 'Turn to choose the type. Only the two new types are shown here. Double-tap to go back.' : 'Turn to set the amount. Double-tap the knob to reach Randomize Type.';
  mapSync();
}
['nisScale', 'nisType'].forEach(i => $(i).onchange = () => { stopAll(); renderNis(); });
$('nisPct').oninput = renderNis;
$('nisRoll').onclick = () => { nisSeed += 1; stopAll(); renderNis(); };
playButton($('nisPlay'), (t0, nodes) => nisNotes().notes.forEach(n => pluck(t0 + n.t * secPerTick, n.note, n.vel, .2, nodes)), () => 384 * secPerTick);

/* ---------- USB routing ---------- */
/* Rows and names as in fw/usbmenu.c. Rows with a select stay in sync with the controls beside the display. */
const usbUi = { sel: 0, edit: false, fx: 1, rec: 0, rows: [
  { label: 'Audio Out', select: 'usbOut' },
  { label: 'Multitrack 9/10', select: 'usbPreset' },
  { label: 'Track Level', select: 'usbFader' },
  { label: 'FX Mode', key: 'fx', names: ['Classic', 'Creative'], note: () => usbUi.fx ? "Creative: this mod's Echo, Loop, CC Motion and Arp." : 'Classic: the original pad names and behaviour, no Arp.' },
  { label: 'Rec', key: 'rec', names: ['Mono', 'Stereo'], note: () => `USB recordings are saved in ${usbUi.rec ? 'stereo' : 'mono'}.` },
] };
const usbCount = r => r.select ? $(r.select).options.length : r.names.length;
const usbIndex = r => r.select ? $(r.select).selectedIndex : usbUi[r.key];
const usbName = r => r.select ? $(r.select).selectedOptions[0].textContent : r.names[usbUi[r.key]];
function usbSetValue(i) {
  const r = usbUi.rows[usbUi.sel];
  if (r.select) $(r.select).selectedIndex = i; else usbUi[r.key] = i;
  renderUsb();
}
function usbRow(i) {
  usbUi.edit = i === usbUi.sel ? !usbUi.edit : true;
  usbUi.sel = i; renderUsb();
}
function renderUsb() {
  const out = $('usbOut').value, multi = out === 'multi';
  $('usbPresetL').hidden = !multi; $('usbFaderL').hidden = !multi; $('usb11L').hidden = !multi;
  const ch = (a, b, hot) => el('div', { className: 'ch' + (hot ? ' hot' : '') }, el('span', { textContent: a }), el('b', { textContent: b }));
  const col = (title, ...items) => el('div', {}, el('h4', { textContent: title }), el('div', { className: 'chs' }, ...items));
  const arrow = t => el('div', { className: 'arrow', textContent: t, ariaHidden: 'true' });
  const none = () => el('div', { className: 'ch off' }, el('span', { textContent: 'No audio over USB' }));
  const flow = $('usbFlow'); let text = '', title = $('usbOut').selectedOptions[0].textContent;
  if (out === 'off') { flow.replaceChildren(col('Play', none()), arrow('✕'), col('Computer', none())); text = 'USB carries only MIDI and the file connection, as with the original firmware.'; }
  if (out === 'stereo') { flow.replaceChildren(col('Play', ch('Master L', '', 1), ch('Master R', '', 1)), arrow('→'), col('Computer input', ch('Ch 1', 'L'), ch('Ch 2', 'R'))); text = 'The finished mix goes to the computer. 44.1 kHz.'; }
  if (out === 'in') { flow.replaceChildren(col('Computer output', ch('L', ''), ch('R', '')), arrow('⇄'), col('Play', ch('Added to master', 'L R', 1), ch('Sent to delay + reverb', 'mono', 1), ch('Master back to computer', 'L R', 1))); text = 'Audio from the computer joins the master before limiter and Loop, with a fixed send of 25% to delay and reverb. The computer also receives the mix. This mode lets you record USB audio. Multitrack is off here to leave room on the USB connection.'; }
  if (multi) {
    const fxr = $('usbPreset').value === 'fx', eleven = $('usb11').checked, fader = $('usbFader').value === 'post';
    const rows = []; for (let i = 1; i <= 8; i++) rows.push(ch(`Ch ${i}`, `Track ${i}`));
    const a = fxr ? 'FX Return' : 'Master';
    rows.push(ch('Ch 9', a + ' L', 1), ch('Ch 10', a + ' R', 1));
    if (eleven) rows.push(ch('Ch 11', (fxr ? 'Master' : 'FX Return') + ' mono', 1));
    flow.replaceChildren(col('Play', el('div', { className: 'ch' }, el('span', { textContent: 'Tracks 1–8' }), el('b', { textContent: fader ? 'with track volume' : 'without track volume' })), ch(a, 'stereo', 1)), arrow('→'), col('Computer input', ...rows));
    text = `${eleven ? 11 : 10} channels to your DAW, plus a separate stereo device.` + (fxr ? ' Tracks stay dry, with wet effects on channels 9 and 10: useful for mixing in the box.' : ' Loop and limiter exist only in the master.');
    title += ` · ${eleven ? 11 : 10} channels`;
  }
  const ui = usbUi.rows[usbUi.sel];
  if (ui.note) text += ' ' + ui.note();
  $('usbTitle').textContent = title; $('usbText').textContent = text;
  drawUsb();
}
['usbOut', 'usbPreset', 'usbFader', 'usb11'].forEach(i => $(i).onchange = renderUsb);
const usbKnobCfg = {
  name: 'Screen knob', max: () => usbUi.edit ? usbCount(usbUi.rows[usbUi.sel]) - 1 : usbUi.rows.length - 1,
  get: () => usbUi.edit ? usbIndex(usbUi.rows[usbUi.sel]) : usbUi.sel,
  set: v => { if (usbUi.edit) usbSetValue(v); else { usbUi.sel = v; renderUsb(); } },
  text: () => { const r = usbUi.rows[usbUi.sel]; return `${r.label}: ${usbName(r)}${usbUi.edit ? ', editing' : ''}`; },
  onTap: () => { usbUi.edit = !usbUi.edit; renderUsb(); },
};
const usbKnob = PD.knob($('usbKnob'), usbKnobCfg);
const usbScene = () => ({ title: 'USB Audio', sel: usbUi.sel, editing: usbUi.edit, onRow: usbRow, rows: usbUi.rows.map(r => ({ label: r.label, value: usbName(r) })) });
function drawUsb() {
  PD.menu($('usbDisp'), usbScene());
  usbKnob.update();
  $('usbHint').textContent = usbUi.edit ? 'Turn the Screen knob to change the value. Press it when done.' : 'Turn the Screen knob to move. Press it to change a value.';
  mapSync();
}

/* ---------- Live Rec button (lrec.c: long press 1000 ms) ---------- */
const recCtl = (function rec() {
  const btn = $('recBtn'), hint = $('recHint');
  let downAt = 0, holdTimer = 0, fired = false, recording = false, tick = 0, ds = 0, take = 1, pill = '', toast = '';
  const draw = () => { PD.param($('recDisp'), { pill, toast }); mapSync(); };
  const say = t => { toast = t; draw(); };
  const fmt = d => { const s = Math.floor(d / 10); return `USB REC ${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; };
  function setRec(on) {
    recording = on;
    if (on) { ds = 0; pill = fmt(0); say(`USB REC  Rec_${String(take).padStart(3, '0')}`); tick = setInterval(() => { ds += 1; pill = fmt(ds); draw(); }, 100); hint.textContent = 'Hold again to stop.'; }
    else { clearInterval(tick); pill = ''; say(ds < 3 ? 'Rec empty: no USB audio' : 'Rec -> Sample ' + (9 + take)); take += 1; hint.textContent = 'Press briefly, or press and hold.'; }
  }
  const down = e => { if (e.button > 0) return; e.preventDefault(); if (btn.setPointerCapture && e.pointerId != null) try { btn.setPointerCapture(e.pointerId); } catch (_) {} btn.classList.add('down'); downAt = performance.now(); fired = false; clearTimeout(holdTimer); holdTimer = setTimeout(() => { fired = true; if (!recording && $('usbOut').value !== 'in') say('USB REC: Audio Out = Stereo + In'); else setRec(!recording); }, 1000); };
  const up = () => { if (!downAt) return; btn.classList.remove('down'); clearTimeout(holdTimer); downAt = 0; if (!fired) say('Live Rec (normal)'); };
  btn.addEventListener('pointerdown', down);
  btn.addEventListener('pointerup', up);
  btn.addEventListener('pointercancel', up);
  btn.addEventListener('keydown', e => { if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) { e.preventDefault(); down({ button: 0, preventDefault() {} }); } });
  btn.addEventListener('keyup', e => { if (e.key === ' ' || e.key === 'Enter') up(); });
  draw();
  return { press: () => down({ button: 0, preventDefault() {} }), release: up, view: () => ({ pill, toast }) };
})();

renderPads(); renderFx(); drawFx(); renderArp(); renderNis(); renderUsb();
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => { renderFx(); renderArp(); renderNis(); });
