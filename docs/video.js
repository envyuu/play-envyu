'use strict';
/* Presentation for the video: 16:9 stage, keyboard steps, live demos. Note rules come from fxdata.js. */
const $ = id => document.getElementById(id);
const el = (tag, props = {}, ...kids) => { const e = Object.assign(document.createElement(tag), props); e.append(...kids); return e; };
const NS = 'http://www.w3.org/2000/svg';
const s = (tag, a = {}, text) => { const e = document.createElementNS(NS, tag); for (const k in a) e.setAttribute(k, a[k]); if (text != null) e.textContent = text; return e; };
const TICKS_PER_SEC = 192;                                       /* 120 BPM at 96 ticks per quarter */
const C = { usb: '#5fd0c5', perf: '#c79bea', arp: '#ffb238', nis: '#ff7a6b' };
const CATS = [['Tuning', '#e5383b'], ['Cutoff', '#ff9f66'], ['Distortion', '#f7e463'], ['Rearrange', '#2fcf5a'], ['Repeat', '#bdeea0'], ['Delay', '#c79bea'], ['Reverb', '#f08bea'], ['Loop', '#ff3d87']];

/* ---------- Clock and sound ---------- */
let frames = [], sound = false, ctx = null, master = null;
const onFrame = f => frames.push(f);
const t0 = performance.now();
(function loop(now) { const sec = (now - t0) / 1000; frames.forEach(f => f(sec)); requestAnimationFrame(loop); })(t0);
function pluck(note, vel = 100, dur = .18) {
  if (!sound) return;
  if (!ctx) { ctx = new AudioContext(); master = ctx.createGain(); master.gain.value = .3; master.connect(ctx.destination); }
  const t = ctx.currentTime, o = ctx.createOscillator(), g = ctx.createGain();
  o.type = 'triangle'; o.frequency.value = 440 * 2 ** ((note - 69) / 12);
  g.gain.setValueAtTime(vel / 127, t); g.gain.exponentialRampToValueAtTime(.001, t + dur);
  o.connect(g); g.connect(master); o.start(t); o.stop(t + dur + .02);
}

/* ---------- Piano roll with a running playhead ---------- */
const COLOR = { src: '#f3f3f5', dim: '#5a5d66', out: '#ff5c4d', echo: '#ffb238', loop: '#ff3d87', arp: '#ffb238', neu: '#ffb238' };
function Roll(opts = {}) {
  const W = opts.w || 1000, H = opts.h || 520, L = 66, B = 16, T = 8;
  const svg = s('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': opts.label || 'Note timeline' });
  const box = el('div', { className: 'rollbox' }, svg);
  let rects = [], total = 384, last = 0, head = null;
  function set(notes, tot, o = {}) {
    total = tot; svg.replaceChildren(); rects = [];
    const ns = notes.map(n => n.note), lo = o.lo ?? Math.min(...ns, 60) - 1, hi = o.hi ?? Math.max(...ns, 60) + 1, span = Math.max(hi - lo + 1, 8);
    const x = t => L + t / total * (W - L - 8), y = n => T + (hi - n) / span * (H - T - B), rowH = Math.min((H - T - B) / span, 44);
    if (o.rowLit) for (let n = lo; n <= hi; n++) if (o.rowLit(n)) svg.append(s('rect', { x: L, y: y(n), width: W - L - 8, height: rowH, fill: '#ffffff', opacity: .07 }));
    for (let t = 0; t <= total + .01; t += o.grid || 24) svg.append(s('line', { x1: x(t), x2: x(t), y1: T, y2: H - B, stroke: '#2b2e37', 'stroke-width': t % ((o.grid || 24) * 4) === 0 ? 2 : 1 }));
    [hi - 1, lo + 1].filter((v, i, a) => a.indexOf(v) === i).forEach(n => svg.append(s('text', { x: 2, y: y(n) + rowH * .75 }, noteName(n))));
    if (o.mark != null) { svg.append(s('line', { x1: x(o.mark), x2: x(o.mark), y1: T, y2: H - B, stroke: '#5fd0c5', 'stroke-width': 3, 'stroke-dasharray': '8 7' })); svg.append(s('text', { x: x(o.mark) + 8, y: T + 22, style: 'fill:#5fd0c5' }, o.markLabel || '')); }
    notes.forEach(n => {
      const col = COLOR[n.kind] || COLOR.neu;
      const r = s('rect', { class: 'n', x: x(n.t), y: y(n.note), width: Math.max(5, Math.min(x(n.t + n.dur) - x(n.t), W - 8 - x(n.t))), height: rowH, rx: 5, fill: col, style: `color:${col}`, opacity: n.kind === 'dim' ? .5 : n.kind === 'src' ? 1 : clamp(.35 + (n.vel || 100) / 127 * .7, .35, 1) });
      svg.append(r); rects.push({ r, n });
    });
    head = s('line', { x1: L, x2: L, y1: T, y2: H - B, stroke: '#fff', 'stroke-width': 3, opacity: .75 });
    svg.append(head);
    box.__x = x;
  }
  function tick(sec) {
    if (!head) return;
    const pos = (sec * TICKS_PER_SEC) % total, px = box.__x(pos);
    head.setAttribute('x1', px); head.setAttribute('x2', px);
    rects.forEach(({ r, n }) => {
      const on = pos >= n.t && pos < n.t + Math.max(n.dur, 6);
      r.classList.toggle('lit', on);
      const crossed = last <= pos ? n.t >= last && n.t < pos : n.t >= last || n.t < pos;
      if (crossed && n.kind !== 'dim') pluck(n.note, n.vel || 100);
    });
    last = pos;
  }
  onFrame(tick);
  return { el: box, set };
}

/* ---------- Slide helpers ---------- */
const head = (eyebrow, title, text, color) => {
  const e = el('p', { className: 'eyebrow2', textContent: eyebrow }); e.style.setProperty('--c', color);
  const say = el('p', { className: 'say' }); say.innerHTML = text;
  return el('div', {}, e, el('h1', { textContent: title }), say);
};
const display = cls => el('div', { className: 'bigdisp ' + (cls || '') }, el('div', { className: 'pd' }));
const pdOf = d => d.firstChild;
const seg = (labels, onPick) => {
  const w = el('div', { className: 'seg2' });
  labels.forEach((t, i) => w.append(el('button', { type: 'button', textContent: t, onclick: () => onPick(i) })));
  w.pick = i => [...w.children].forEach((b, k) => b.setAttribute('aria-pressed', String(k === i)));
  return w;
};
const USB_MODES = [
  ['Off', 'USB carries only MIDI and the file connection, as with the original firmware.'],
  ['Stereo', 'The finished mix goes to your computer at 44.1 kHz.'],
  ['Stereo + Multi', '10 or 11 channels to your DAW, plus a separate stereo device.'],
  ['Stereo + In', 'Your computer plays through the Play, and also receives the mix. This mode lets you record USB audio.'],
];

/* ---------- Slides ---------- */
const SLIDES = [];
const add = (chapter, steps, build) => SLIDES.push({ chapter, steps, build });

add(null, 1, () => {
  const wave = el('div', { className: 'wave', ariaHidden: 'true' });
  for (let r = 0; r < 8; r++) for (let c = 0; c < 20; c++) { const i = el('i'); i.style.setProperty('--c', c < 16 ? CATS[c >> 1][1] : '#9b6fd6'); i.dataset.c = c; i.dataset.r = r; wave.append(i); }
  onFrame(sec => { for (const i of wave.children) { const v = Math.sin(sec * 1.6 - i.dataset.c * .42 + i.dataset.r * .3); i.style.opacity = (.1 + Math.max(0, v) ** 3 * .9).toFixed(2); } });
  return { root: el('section', { className: 'slide title-slide' }, wave, el('h1', { textContent: 'More play in your Play.' }), el('p', { className: 'say', textContent: 'A community firmware mod for the Polyend Play · Firmware 1.6.0 · Beta' })) };
});

add(null, 4, () => {
  const items = [['01', 'USB Audio', 'Interface, multitrack, audio in, recording', C.usb], ['02', 'Perform pads', 'Echo, Loop and CC Motion, on the tracks you choose', C.perf], ['03', 'Arpeggiator', 'A new parameter on every step', C.arp], ['04', 'Note In Scale', 'Randomize that stays in key', C.nis]];
  const list = el('ol', { className: 'chapters' });
  items.forEach(([n, t, d, c]) => { const li = el('li', {}, el('span', { textContent: n }), el('div', {}, document.createTextNode(t), el('small', { textContent: d }))); li.style.setProperty('--c', c); list.append(li); });
  return { root: el('section', { className: 'slide', style: 'align-content:center' }, el('h1', { textContent: 'What is new' }), list), step: n => [...list.children].forEach((li, i) => li.classList.toggle('on', i <= n)) };
});

/* USB: modes */
add('usb', 4, () => {
  const d = display(), title = el('p', { className: 'modename' }), text = el('p', { className: 'modetext' });
  let cur = 3;
  const draw = () => {
    PD.menu(pdOf(d), { title: 'USB Audio', sel: 0, editing: true, rows: [{ label: 'Audio Out', value: USB_MODES[cur][0] }, { label: 'Multitrack 9/10', value: 'Master' }, { label: 'Track Level', value: 'Pre Fader' }, { label: 'FX Mode', value: 'Creative' }, { label: 'Rec', value: 'Mono' }] });
    title.textContent = USB_MODES[cur][0]; text.textContent = USB_MODES[cur][1]; sw.pick(cur);
  };
  const sw = seg(USB_MODES.map(m => m[0]), i => { cur = i; draw(); });
  return { root: el('section', { className: 'slide two' }, head('01 USB Audio', 'Your Play becomes an audio interface', 'A new page in the main menu. <b>One setting</b> decides what travels over USB.', C.usb), el('div', { className: 'demo' }, d, sw, el('div', { className: 'panelbox' }, title, text))), step: n => { cur = n; draw(); } };
});

/* USB: multitrack */
add('usb', 3, () => {
  const meters = [], wrap = el('div', { className: 'meters' });
  for (let i = 0; i < 11; i++) { const bar = el('i'), lab = el('small'), m = el('div', { className: 'meter' + (i >= 8 ? ' main' : '') }, el('div', { className: 'well' }, bar), el('b', { textContent: `Ch ${i + 1}`, style: 'font-weight:600' }), lab); meters.push({ m, bar, lab }); wrap.append(m); }
  const note = el('p', { className: 'modetext' }); let step = 0;
  const preset = seg(['Master', 'FX Return'], i => { step = i ? 2 : 1; draw(); });
  function draw() {
    const fxr = step === 2;
    meters.forEach(({ m, lab }, i) => {
      lab.textContent = i < 8 ? `Track ${i + 1}` : i < 10 ? `${fxr ? 'FX Return' : 'Master'} ${i === 8 ? 'L' : 'R'}` : (fxr ? 'Master mono' : 'FX Return mono');
      m.classList.toggle('off', step === 0 && i >= 8);
    });
    preset.pick(fxr ? 1 : 0);
    note.textContent = ['Every track on its own channel: mono, after track filter and overdrive, before pan.', 'Channels 9 and 10 carry the master, with limiter and Loop.', 'Or: tracks stay dry, with wet delay and reverb on 9 and 10. Useful for mixing in the box.'][step];
  }
  const phase = [0, 1.7, 3.1, .6, 2.4, 4.2, 5.1, 1.1, 0, .4, 2];
  onFrame(sec => meters.forEach(({ bar }, i) => {
    const beat = (sec * 2 + phase[i] / 6.28 * (i % 3 + 1)) % 1, master = i >= 8, fxr = step === 2 && (i === 8 || i === 9), v = master ? (fxr ? .35 : .72) + .14 * Math.sin(sec * 5 + i) : (.15 + .75 * (1 - beat) ** 2.4);
    bar.style.height = (clamp(v, .02, 1) * 100).toFixed(1) + '%';
  }));
  return { root: el('section', { className: 'slide two' }, head('01 USB Audio', 'All tracks, separately', '<b>10 or 11 channels</b> into your DAW, plus a separate stereo device.', C.usb), el('div', { className: 'demo' }, wrap, preset, note, el('p', { className: 'small', style: 'margin:0', textContent: 'Levels shown as an illustration.' }))), step: n => { step = n; draw(); } };
});

/* USB: audio in */
add('usb', 3, () => {
  const svg = s('svg', { class: 'flowsvg', viewBox: '0 0 1040 600', role: 'img', 'aria-label': 'Audio from the computer into the Play and back' });
  const box = (x, y, w, h, t, sub) => { const r = s('rect', { x, y, width: w, height: h, rx: 18 }); svg.append(r, s('text', { x: x + w / 2, y: y + h / 2 - (sub ? 16 : 0) }, t)); if (sub) svg.append(s('text', { class: 'sub', x: x + w / 2, y: y + h / 2 + 22 }, sub)); return r; };
  const pIn = s('path', { d: 'M 280 150 L 620 150' }), pFx = s('path', { d: 'M 450 150 L 450 450 L 620 450' }), pOut = s('path', { d: 'M 760 220 C 760 560 140 560 140 220' });
  svg.append(pIn, pFx, pOut);
  box(20, 80, 260, 140, 'Computer', 'USB out L R');
  const m = box(620, 80, 400, 140, 'Master', 'before limiter and Loop');
  const f = box(620, 380, 400, 140, 'Delay + Reverb', 'fixed send, 25%');
  const say = el('p', { className: 'modetext' });
  const TEXT = ['Audio from your computer joins the master.', 'A fixed 25% also goes to delay and reverb.', 'Your computer receives the finished mix back.'];
  return { root: el('section', { className: 'slide two' }, head('01 USB Audio', 'Play your computer through the Play', 'Set Audio Out to <b>Stereo + In</b>. Any app becomes a sound source.', C.usb), el('div', { className: 'demo' }, svg, say)), step: n => {
    pFx.classList.toggle('dim', n < 1); pOut.classList.toggle('dim', n < 2);
    m.classList.toggle('hot', n === 0); f.classList.toggle('hot', n === 1); say.textContent = TEXT[n];
  } };
});

/* USB: record */
add('usb', 3, () => {
  const d = display(), btn = el('button', { type: 'button', className: 'hold', textContent: 'Live Rec' });
  const file = el('div', { className: 'new', textContent: '      Rec_001.wav' }); file.style.whiteSpace = 'pre';
  const tree = el('div', { className: 'tree' }, el('div', { textContent: 'Samples/' }), el('div', { textContent: '  Live Recordings/', style: 'white-space:pre' }), file);
  let st = 0, started = 0, pressAt = 0, timer = 0;
  const fmt = sec => `USB REC ${Math.floor(sec / 60)}:${String(Math.floor(sec) % 60).padStart(2, '0')}`;
  const draw = sec => {
    if (st === 0) PD.param(pdOf(d), { toast: 'Hold Live Rec' });
    else if (st === 1) PD.param(pdOf(d), { pill: fmt((sec ?? started) - started), toast: 'USB REC  Rec_001' });
    else PD.param(pdOf(d), { toast: 'Rec -> Sample 10' });
    file.classList.toggle('show', st === 2);
  };
  let lastShown = -1;
  onFrame(sec => { if (st === 1) { if (!started) started = sec; const w = Math.floor(sec - started); if (w !== lastShown) { lastShown = w; draw(sec); } } });
  const set = v => { st = v; started = 0; lastShown = -1; draw(); };
  btn.addEventListener('pointerdown', () => { btn.classList.add('down'); pressAt = performance.now(); timer = setTimeout(() => set(st === 1 ? 2 : 1), 1000); });
  const up = () => { btn.classList.remove('down'); clearTimeout(timer); };
  btn.addEventListener('pointerup', up); btn.addEventListener('pointercancel', up);
  return { root: el('section', { className: 'slide two' }, head('01 USB Audio', 'Record it straight to the card', '<b>Hold Live Rec</b> for a second. The take lands in your project as a sample. Up to 6 minutes, mono or stereo.', C.usb), el('div', { className: 'demo' }, el('div', { className: 'row' }, d, btn), el('div', { className: 'panelbox' }, tree))), step: n => set(n) };
});

/* Perform */
function fxSlide(kind, cat, color, title, text, picks) {
  const names = kind === 'echo' ? ECHO.map(e => e[5]) : kind === 'cc' ? LFO.map(e => e[5]) : [...LOOPNAME, ...LOOPNAME.map(n => n + ' LR')];
  add('perf', picks.length, () => {
    const d = display(), pads = el('div', { className: 'padset' }), roll = Roll({ w: 800, h: 520 }), name = el('p', { className: 'rollname' }), note = el('p', { className: 'rollnote' });
    pads.style.setProperty('--c', color);
    let cur = picks[0];
    const cc = kind === 'cc' ? s('svg', { viewBox: '0 0 800 420', style: 'width:100%;height:auto;display:block' }) : null, read = kind === 'cc' ? el('div', { className: 'ccread' }) : null;
    let dot = null, ccCfg = null, ccTotal = 384;
    function pick(i) {
      cur = i;
      [...pads.children].forEach(b => b.setAttribute('aria-pressed', String(+b.dataset.i === i)));
      PD.perform(pdOf(d), { label: cat, value: names[i], held: true });
      name.textContent = names[i];
      if (kind === 'echo') {
        const c = ECHO[i], n = echoNotes(c).map((x, k) => ({ ...x, kind: k ? 'echo' : 'src' }));
        roll.set(n, Math.max(192, n[n.length - 1].t + 36));
        note.textContent = `${c[1]} real MIDI repeats, fading in velocity.` + (c[3] ? ` Pitch ${c[4] ? 'alternates by' : 'moves by'} ${Math.abs(c[3])} semitones.` : '');
      } else if (kind === 'loop') {
        const r = loopNotes(i), out = new Set(r.out);
        roll.set(r.notes.map(n => ({ ...n, kind: n.kind || (out.has(n) ? 'loop' : 'src') })), 384, { mark: r.holdAt, markLabel: 'pad held' });
        note.textContent = r.cap.length ? `Repeats the last ${r.len} ticks${r.rev ? ', backwards' : ''}. Let go and the pattern continues in the right place.` : 'This short window holds no note right now, so you hear silence until you let go.';
      } else {
        ccCfg = LFO[i]; ccTotal = ccCfg[2] >= 384 ? ccCfg[2] * (ccCfg[1] === 'once' ? 1.25 : 2) : 384;
        cc.replaceChildren();
        const x = t => 8 + t / ccTotal * 784, y = v => 12 + (127 - v) / 127 * 396;
        [0, 64, 127].forEach(v => cc.append(s('line', { x1: 8, x2: 792, y1: y(v), y2: y(v), stroke: '#2b2e37', 'stroke-width': 2 })));
        let p = ''; for (let t = 0; t <= ccTotal; t += 1) p += (t ? 'L' : 'M') + x(t).toFixed(1) + ' ' + y(lfoValue(ccCfg, t, RND)).toFixed(1);
        cc.append(s('path', { d: p, fill: 'none', stroke: color, 'stroke-width': 6, 'stroke-linejoin': 'round' }));
        dot = s('circle', { r: 14, fill: '#fff' }); cc.append(dot);
        cc.__x = x; cc.__y = y;
        note.textContent = `Moves ${CCN[ccCfg[0]]}. When you let go, the CC returns to its last value.`;
      }
    }
    names.forEach((n, i) => null);
    for (let row = 0; row < 8; row++) for (const off of [0, 8]) { const i = row + off; const b = el('button', { type: 'button', textContent: names[i], onclick: () => pick(i) }); b.dataset.i = i; pads.append(b); }
    if (cc) onFrame(sec => { if (!dot) return; const t = (sec * TICKS_PER_SEC) % ccTotal, v = lfoValue(ccCfg, t, RND); dot.setAttribute('cx', cc.__x(t)); dot.setAttribute('cy', cc.__y(v)); read.textContent = `CC${ccCfg[0]} ${String(Math.round(v)).padStart(3, ' ')}`; });
    const left = el('div', {}, head('02 Perform pads', title, text, color), el('div', { style: 'margin-top:34px' }, d));
    const right = el('div', {}, name, cc ? el('div', {}, cc, read) : roll.el, note);
    return { root: el('section', { className: 'slide fx' }, left, pads, right), step: n => pick(picks[n]) };
  });
}
fxSlide('echo', 'Delay', '#c79bea', 'Echo', 'Delay pads now repeat your <b>actual MIDI notes</b>.', [1, 8, 6, 15]);
fxSlide('loop', 'Loop', '#ff3d87', 'MIDI Loop', 'Hold a Loop pad: it <b>captures the last 4 steps</b>, then you can go shorter.', [0, 2, 3, 10]);
fxSlide('cc', 'Reverb', '#f08bea', 'CC Motion', 'Reverb pads <b>move a CC</b> on your synth instead of sending one fixed value.', [2, 4, 6, 10]);

/* Perform: track selection */
add('perf', 3, () => {
  const sel = new Set(), rollA = Roll({ w: 1000, h: 250 }), rollB = Roll({ w: 1000, h: 250 });
  const cfg = ECHO[1];
  const lane = (track, base, pick) => { const out = []; base.forEach(n => { out.push({ ...n, kind: 'src' }); if (pick) echoNotes(cfg).slice(1).forEach(e => { const t = n.t + e.t; if (t < 384) out.push({ t, dur: e.dur, note: n.note, vel: e.vel, kind: 'echo' }); }); }); return out; };
  const drums = [0, 96, 192, 288].map(t => ({ t, dur: 18, note: 36, vel: 110 }));
  const synth = [0, 48, 96, 144, 192, 240, 288, 336].map((t, i) => ({ t, dur: 18, note: [60, 63, 67, 70, 72, 70, 67, 63][i], vel: 100 }));
  const bD = el('button', { type: 'button', className: 'tbtn', onclick: () => toggle(0) }, 'Track 1', el('small', { textContent: 'Drums' }));
  const bS = el('button', { type: 'button', className: 'tbtn', onclick: () => toggle(3) }, 'Track 4', el('small', { textContent: 'Synth' }));
  const say = el('p', { className: 'modetext' });
  function draw() {
    const all = sel.size === 0;
    rollA.set(lane(0, drums, all || sel.has(0)), 384, { lo: 35, hi: 37 });
    rollB.set(lane(3, synth, all || sel.has(3)), 384, { lo: 58, hi: 74 });
    bD.setAttribute('aria-pressed', String(sel.has(0))); bS.setAttribute('aria-pressed', String(sel.has(3)));
    say.textContent = all ? 'Nothing selected: the effect hits every MIDI track.' : `Only ${[...sel].sort().map(t => 'Track ' + (t + 1)).join(' and ')} gets the effect. The rest keeps playing untouched.`;
  }
  function toggle(t) { if (sel.has(t)) sel.delete(t); else sel.add(t); draw(); }
  return { root: el('section', { className: 'slide two' }, head('02 Perform pads', 'Choose your tracks', 'Press the <b>Select pads</b> in column 20. Echo, Loop and CC Motion only touch what you selected.', C.perf), el('div', { className: 'demo' }, el('div', { className: 'lanes' }, el('div', { className: 'lane' }, bD, rollA.el), el('div', { className: 'lane' }, bS, rollB.el)), say)), step: n => { sel.clear(); if (n === 1) sel.add(3); if (n === 2) sel.add(0); draw(); } };
});

/* Arp */
add('arp', 6, () => {
  const d = display(), roll = Roll({ w: 900, h: 560 }), name = el('p', { className: 'rollname' }), note = el('p', { className: 'rollnote' });
  const st = { focus: 0, chord: 3, mode: 1, oct: false };
  const chordName = () => CHORDS[st.chord][0].replace(/ \(.*/, '');
  const value = () => ARP[st.mode][0] + (st.oct && st.mode ? ' +Oct' : '');
  const DESC = { off: 'Arp off: the step plays its chord as usual.', up: 'Low to high, then again.', dn: 'High to low, then again.', ud: 'Up and back down without repeating the ends.', rnd: 'Random chord tones, different each time on the device.', conv: 'Alternates outside in: low, high, next low, next high.', div: 'Starts in the middle and moves outwards.', pink: 'Walks up, returning to the top note after every tone.', strum: 'Plays each chord tone once, 3 ticks apart, then lets them ring.' };
  const knobHint = el('p', { className: 'knobhint2' });
  const host = el('div', { className: 'knobwrap' }, knobHint);
  const k = PD.knob(host, { name: 'Chord knob', max: () => st.focus ? ARP.length - 1 : CHORDS.length - 1, get: () => st.focus ? st.mode : st.chord, set: v => { if (st.focus) st.mode = v; else st.chord = v; draw(); }, text: () => st.focus ? value() : chordName(), onDouble: () => { st.focus ^= 1; draw(); } });
  function draw() {
    const r = arpPattern(st.mode, st.chord, st.oct);
    PD.param(pdOf(d), { focus: st.focus, blocks: [{ label: 'Chord', value: chordName(), frac: st.chord / (CHORDS.length - 1) }, { label: 'Arp', value: value(), frac: st.mode / (ARP.length - 1) }] });
    roll.set(r.out.map(n => ({ ...n, kind: 'arp' })), r.total);
    name.textContent = st.focus ? value() : `Chord: ${chordName()}`;
    note.textContent = st.mode ? DESC[r.a[1]] : DESC.off;
    knobHint.textContent = st.focus ? 'Turn: 28 patterns. Shift + turn: +Oct.' : 'Double-tap the knob to reach Arp.';
    k.update();
  }
  const SEQ = [{ focus: 0, mode: 1 }, { focus: 1, mode: 1 }, { focus: 1, mode: 3 }, { focus: 1, mode: 17 }, { focus: 1, mode: 20 }, { focus: 1, mode: 1, oct: true }];
  return { root: el('section', { className: 'slide two' }, el('div', {}, head('03 Arpeggiator', 'A new parameter on every step', 'On a MIDI track, the Chord knob gets a second parameter: <b>Arp</b>. It plays in time until the next note, Stop, or at most 4 bars.', C.arp), el('div', { className: 'row tight', style: 'margin-top:30px;gap:28px' }, d, host)), el('div', { className: 'demo' }, el('div', {}, name, roll.el, note))), step: n => { const q = SEQ[n]; st.focus = q.focus; st.mode = q.mode; st.oct = !!q.oct; draw(); } };
});

/* Note In Scale */
add('nis', 4, () => {
  const d = display(), roll = Roll({ w: 900, h: 560 }), note = el('p', { className: 'rollnote' }), name = el('p', { className: 'rollname' });
  const st = { focus: 0, pct: 0, plus: false, seed: 7 }, scale = 2, sc = SCALES[scale][1];
  const knobHint = el('p', { className: 'knobhint2' });
  const host = el('div', { className: 'knobwrap' }, knobHint);
  const typeName = () => `In C ${SCALES[scale][0]}${st.plus ? '+' : ''}`;
  const k = PD.knob(host, { name: 'Randomize knob', px: 6, max: () => st.focus ? 1 : 20, get: () => st.focus ? +st.plus : st.pct / 5, set: v => { if (st.focus) st.plus = !!v; else st.pct = v * 5; draw(); }, text: () => st.focus ? typeName() : st.pct + '%', onDouble: () => { st.focus ^= 1; draw(); } });
  function draw() {
    const r = nisPattern(scale, st.pct, st.plus, st.seed);
    PD.param(pdOf(d), { focus: st.focus, blocks: [{ label: 'Randomize', value: st.pct + '%', frac: st.pct / 100 }, { label: 'Randomize Type', value: typeName(), frac: +st.plus }] });
    roll.set(r.notes, 384, { lo: 46, hi: 80, rowLit: n => sc.includes(((n % 12) + 12) % 12) });
    name.textContent = st.pct ? `${typeName()} · ${st.pct}%` : 'Your original pattern';
    note.textContent = st.pct === 0 ? 'Light bands: the notes of the project scale.' : st.plus && r.outside ? `${r.changed} of 16 steps changed, ${r.outside} leaving the scale on purpose (red).` : `${r.changed} of 16 steps changed, all inside the scale. A changed step never keeps its original note.`;
    knobHint.textContent = st.focus ? 'Turn: In ‹Scale› or In ‹Scale›+.' : 'Turn the Randomize knob. Double-tap for the type.';
    k.update();
  }
  const SEQ = [{ pct: 0, plus: false }, { pct: 50, plus: false }, { pct: 100, plus: false }, { pct: 100, plus: true }];
  return { root: el('section', { className: 'slide two' }, el('div', {}, head('04 Note In Scale', 'Randomize that stays in key', 'Two new Randomize types for MIDI tracks. The name <b>follows your project scale</b>, and new notes stay within one octave.', C.nis), el('div', { className: 'row tight', style: 'margin-top:30px;gap:28px' }, d, host)), el('div', { className: 'demo' }, el('div', {}, name, roll.el, note))), step: n => { const q = SEQ[n]; st.focus = n === 3 ? 1 : 0; st.pct = q.pct; st.plus = q.plus; draw(); } };
});

/* End */
add(null, 3, () => {
  const items = ['Download the official firmware 1.6.0 from Polyend.', 'Patch it in your browser. Nothing leaves your computer.', 'Install it from the firmware update menu.'];
  const list = el('ol', { className: 'steps2' });
  items.forEach((t, i) => list.append(el('li', {}, el('span', { textContent: String(i + 1) }), t)));
  return { root: el('section', { className: 'slide', style: 'align-content:center' }, el('h1', { textContent: 'Try it yourself' }), list, el('p', { className: 'warn2', textContent: 'Link in the description. Unofficial, at your own risk. Back up your SD card first. Tested on one Rev A device so far.' })), step: n => [...list.children].forEach((li, i) => li.classList.toggle('on', i <= n)) };
});

/* ---------- Deck ---------- */
const stage = $('stage'), pips = $('pips');
let idx = 0, stepIdx = 0, cur = null;
const STORE_FRAMES = [];
SLIDES.forEach((sl, i) => {
  const b = el('button', { type: 'button', ariaLabel: `Slide ${i + 1}`, onclick: () => go(i, 0) });
  if (i && sl.chapter !== SLIDES[i - 1].chapter) b.classList.add('gap');
  pips.append(b);
});
function go(i, st) {
  i = clamp(i, 0, SLIDES.length - 1);
  if (i !== idx || !cur) {
    frames = [];
    idx = i; cur = SLIDES[i].build();
    stage.replaceChildren(cur.root);
  }
  stepIdx = clamp(st, 0, SLIDES[i].steps - 1);
  if (cur.step) cur.step(stepIdx);
  const col = C[SLIDES[i].chapter] || '#ffb238';
  [...pips.children].forEach((b, k) => { b.className = (b.classList.contains('gap') ? 'gap ' : '') + (k === i ? 'now' : k < i ? 'done' : ''); });
  pips.style.setProperty('--c', col);
  history.replaceState(null, '', `#${i + 1}.${stepIdx + 1}`);
}
function next() { if (stepIdx < SLIDES[idx].steps - 1) go(idx, stepIdx + 1); else if (idx < SLIDES.length - 1) go(idx + 1, 0); }
function prev() { if (stepIdx > 0) go(idx, stepIdx - 1); else if (idx > 0) go(idx - 1, SLIDES[idx - 1].steps - 1); }
const firstOf = ch => SLIDES.findIndex(sl => sl.chapter === ch);
addEventListener('keydown', e => {
  if (e.target.closest && e.target.closest('.pk')) { if (['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Home', 'End', 'Enter'].includes(e.key)) return; }
  const k = e.key.toLowerCase();
  if (k === 'arrowright' || k === 'pagedown' || (k === ' ' && !(e.target.tagName === 'BUTTON'))) { e.preventDefault(); next(); }
  else if (k === 'arrowleft' || k === 'pageup') { e.preventDefault(); prev(); }
  else if (k === '1') go(firstOf('usb'), 0); else if (k === '2') go(firstOf('perf'), 0); else if (k === '3') go(firstOf('arp'), 0); else if (k === '4') go(firstOf('nis'), 0);
  else if (k === 'home') go(0, 0);
  else if (k === 's') { sound = !sound; if (sound) pluck(72); }
  else if (k === 'f') { if (document.fullscreenElement) document.exitFullscreen(); else document.documentElement.requestFullscreen(); }
  else if (k === 'h') document.body.classList.toggle('nokeys');
});
/* Fit the fixed 1920 x 1080 stage into the window. */
function fit() {
  const sc = Math.min(innerWidth / 1920, innerHeight / 1080);
  $('fit').style.transform = `scale(${sc}) translate(-50%, -50%)`;
}
addEventListener('resize', fit); fit();
let idleTimer = 0;
const awake = () => { document.body.classList.remove('idle'); clearTimeout(idleTimer); idleTimer = setTimeout(() => document.body.classList.add('idle'), 2500); };
addEventListener('pointermove', awake); addEventListener('pointerdown', awake); awake();
const m = location.hash.match(/^#(\d+)(?:\.(\d+))?/);
idx = -1;
go(m ? +m[1] - 1 : 0, m && m[2] ? +m[2] - 1 : 0);
