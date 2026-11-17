'use strict';
/* play-envyu 0.2 video: 16:9 stage (or 9:16 with ?short), keyboard steps, simulated display and pads.
   The drum loop is synthesized in the browser (no samples), so every sound here is our own. */
const $ = id => document.getElementById(id);
const el = (tag, props = {}, ...kids) => { const e = Object.assign(document.createElement(tag), props); e.append(...kids); return e; };
const NS = 'http://www.w3.org/2000/svg';
const s = (tag, a = {}, text) => { const e = document.createElementNS(NS, tag); for (const k in a) e.setAttribute(k, a[k]); if (text != null) e.textContent = text; return e; };
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const Q = new URLSearchParams(location.search), SHORT = Q.has('short'), RENDER = Q.has('render');
if (SHORT) document.body.classList.add('short');
const C = { sl: '#ffd23a', smp: '#5fd0c5', rnd: '#ff7a6b', more: '#c79bea' };

/* ---------- Clock, timers, visual events ---------- */
let frames = [], timers = [], queue = [];
const onFrame = f => frames.push(f);
let VT = 0;                                                               /* render mode: the renderer moves this clock frame by frame */
const now = () => (RENDER ? VT : performance.now() / 1000);
const later = (sec, fn) => timers.push({ t: now() + sec, fn });          /* cleared with clearLater() and on every slide change */
const clearLater = () => { timers = []; };
const vis = (t, fn) => queue.push({ t, fn });
const drain = (list, t) => { const due = list.filter(q => q.t <= t); due.sort((a, b) => a.t - b.t).forEach(q => q.fn()); return list.filter(q => q.t > t); };
function tick() {
  const t = now();
  if (queue.length) queue = drain(queue, t);
  if (timers.length) { const keep = timers; timers = []; const rest = drain(keep, t); timers = rest.concat(timers); }
  if (window.AUTO) window.AUTO(t);
  frames.forEach(f => f(t));
}
/* Render mode: CSS animations and transitions run on real time, so every one of them is paused and moved forward by
   exactly the frame time here. New ones (started by this tick) begin at 0. Motion stays, frame-exact. */
const seen = new WeakSet();
function stepCss(dt) {
  for (const a of document.getAnimations()) {
    if (!seen.has(a)) { seen.add(a); a.pause(); a.currentTime = 0; continue; }
    const end = a.effect && a.effect.getComputedTiming().endTime, t = (a.currentTime || 0) + dt * 1000;
    if (end != null && t >= end) { try { a.finish(); } catch (_) { a.cancel(); } } else a.currentTime = t;
  }
}
if (RENDER) window.advance = dt => { VT += dt; stepCss(dt); tick(); stepCss(0); return VT; }; else (function loop() { tick(); requestAnimationFrame(loop); })();

/* ---------- The loop: 100 BPM, two bars, rendered offline ---------- */
const BPM = 100, SPB = 60 / BPM, LOOP_SEC = 8 * SPB, SR = 44100, S16 = SPB / 4;
const PAT = {
  kick: [0, 6, 10, 16, 22, 27], snare: [8, 24], ghost: [15, 30], hat: [...Array(16)].map((_, i) => i * 2), open: [14],
  bass: [[0, 45, 5], [6, 48, 3], [10, 45, 4], [16, 41, 5], [22, 43, 4], [27, 45, 3]],
};
const ONSETS = [                                              /* time (sixteenths) and how strong the hit is */
  ...PAT.kick.map(i => [i, 1]), ...PAT.snare.map(i => [i, .9]), ...PAT.ghost.map(i => [i, .4]),
  ...PAT.hat.filter(i => !PAT.kick.includes(i) && !PAT.snare.includes(i)).map(i => [i, i % 4 ? .18 : .26]), ...PAT.open.map(i => [i, .3]),
].sort((a, b) => a[0] - b[0]);
let LOOP = null;
async function makeLoop() {
  const len = Math.round(LOOP_SEC * SR), oc = new OfflineAudioContext(1, len, SR);
  const comp = oc.createDynamicsCompressor(); comp.threshold.value = -14; comp.ratio.value = 4; comp.connect(oc.destination);
  const bus = oc.createGain(); bus.gain.value = .85; bus.connect(comp);
  const nb = oc.createBuffer(1, SR, SR); const nd = nb.getChannelData(0); let seed = 7;
  for (let i = 0; i < SR; i++) { seed = (seed * 16807) % 2147483647; nd[i] = seed / 1073741823.5 - 1; }
  const T = i => i * S16;
  const noise = (t, dur, type, f, q, vol) => {
    const src = oc.createBufferSource(); src.buffer = nb; const fl = oc.createBiquadFilter(); fl.type = type; fl.frequency.value = f; fl.Q.value = q;
    const g = oc.createGain(); g.gain.setValueAtTime(vol, t); g.gain.exponentialRampToValueAtTime(.001, t + dur);
    src.connect(fl); fl.connect(g); g.connect(bus); src.start(t, Math.random() * .5, dur + .02);
  };
  const tone = (t, f0, f1, dur, vol, type = 'sine') => {
    const o = oc.createOscillator(); o.type = type; o.frequency.setValueAtTime(f0, t); o.frequency.exponentialRampToValueAtTime(f1, t + dur * .35);
    const g = oc.createGain(); g.gain.setValueAtTime(vol, t); g.gain.exponentialRampToValueAtTime(.001, t + dur);
    o.connect(g); g.connect(bus); o.start(t); o.stop(t + dur + .02);
  };
  PAT.kick.forEach(i => { tone(T(i), 140, 46, .42, 1); noise(T(i), .02, 'highpass', 2500, .7, .25); });
  PAT.snare.forEach(i => { noise(T(i), .22, 'bandpass', 1900, .8, .9); tone(T(i), 210, 170, .12, .45, 'triangle'); });
  PAT.ghost.forEach(i => noise(T(i), .1, 'bandpass', 2100, .9, .3));
  PAT.hat.forEach(i => noise(T(i), .045, 'highpass', 7500, .6, i % 4 ? .16 : .24));
  PAT.open.forEach(i => noise(T(i), .28, 'highpass', 6500, .5, .2));
  PAT.bass.forEach(([i, n, l]) => {
    const t = T(i), f = 440 * 2 ** ((n - 69) / 12), o = oc.createOscillator(), lp = oc.createBiquadFilter(), g = oc.createGain();
    o.type = 'sawtooth'; o.frequency.value = f; lp.type = 'lowpass'; lp.frequency.setValueAtTime(900, t); lp.frequency.exponentialRampToValueAtTime(160, t + .25);
    g.gain.setValueAtTime(.0001, t); g.gain.exponentialRampToValueAtTime(.42, t + .01); g.gain.setValueAtTime(.42, t + l * S16 * .8); g.gain.exponentialRampToValueAtTime(.001, t + l * S16);
    o.connect(lp); lp.connect(g); g.connect(bus); o.start(t); o.stop(t + l * S16 + .05);
  });
  const buf = await oc.startRendering(), d = buf.getChannelData(0);
  const N = 304, pk = new Float32Array(N), rms = new Float32Array(N), per = d.length / N;
  let mx = 0;
  for (let c = 0; c < N; c++) { let p = 0, q = 0; const a = Math.floor(c * per), b = Math.floor((c + 1) * per); for (let i = a; i < b; i++) { const v = Math.abs(d[i]); if (v > p) p = v; q += v * v; } pk[c] = p; rms[c] = Math.sqrt(q / (b - a)); mx = Math.max(mx, p); }
  for (let c = 0; c < N; c++) { pk[c] /= mx; rms[c] = Math.min(1, rms[c] / mx * 1.6); }
  LOOP = { buf, pk, rms };
}
const loopReady = makeLoop();

/* ---------- Sound ---------- */
let sound = false, ctx = null, master = null, voices = [];
function audio() { if (!ctx) { ctx = new AudioContext(); master = ctx.createGain(); master.gain.value = .9; master.connect(ctx.destination); } return ctx; }
const when = t => ctx.currentTime + Math.max(0, t - now());
const AUDIO_LOG = []; let T0 = null;
function playSeg(t, off, dur, rate = 1, gain = 1) {
  if (T0 != null) AUDIO_LOG.push({ k: 'seg', t: t - T0, off, dur, rate, gain });
  if (!sound || !ctx || !LOOP) return;
  const w = when(t), src = ctx.createBufferSource(), g = ctx.createGain(), real = dur / rate;
  src.buffer = LOOP.buf; src.playbackRate.value = rate;
  g.gain.setValueAtTime(gain, w); g.gain.setValueAtTime(gain, w + Math.max(.004, real - .008)); g.gain.linearRampToValueAtTime(0, w + real);
  src.connect(g); g.connect(master); src.start(w, off, dur + .02); voices.push(src);
  if (voices.length > 40) voices.shift();
}
function click(t, hi) {
  if (T0 != null) AUDIO_LOG.push({ k: 'click', t: t - T0, hi: !!hi });
  if (!sound || !ctx) return;
  const w = when(t), o = ctx.createOscillator(), g = ctx.createGain();
  o.frequency.value = hi ? 2200 : 1500; g.gain.setValueAtTime(.25, w); g.gain.exponentialRampToValueAtTime(.001, w + .05);
  o.connect(g); g.connect(master); o.start(w); o.stop(w + .06);
}
function stopAll() { voices.forEach(v => { try { v.stop(); } catch (_) {} }); voices = []; }

/* ---------- Slices ---------- */
const grid = (n, w = 1) => [...Array(n + 1)].map((_, i) => Math.min(1, i / n * w));
const beat = thr => [...ONSETS.filter(o => o[1] >= thr).map(o => o[0] / 32), 1].filter((v, i, a) => i === 0 || v > a[i - 1]);
const ensure0 = b => (b[0] > 0 ? [0, ...b] : b);

/* ---------- Simulated display (logical 320 x 240, drawn at 2x) ---------- */
function Dev(size = 'big') {
  const cv = el('canvas'); cv.width = 1280; cv.height = 960;                 /* 4x the 320 x 240 display: sharp up to 1440p */
  const box = el('div', { className: 'dev ' + size }, cv), g = cv.getContext('2d'); g.scale(4, 4);
  const st = { hdrL: 'T1 Slicer 16', hdrR: '', bounds: null, sel: -1, tint: null, endMark: null, mark: null, anim: null, focusL: '', box: false, topR: '', botL: '', botR: '', view: true,
    thr: null, frac: 1, pill: null, level: 0, big: null, bigSub: '', fine: 0, cutDx: null, fade: 1 };
  const X = 8, Y = 62, W = 304, H = 116;
  const text = (t, x, y, col, size = 13, align = 'left', weight = 400) => { g.font = `${weight} ${size}px system-ui, -apple-system, Segoe UI, sans-serif`; g.fillStyle = col; g.textAlign = align; g.fillText(t, x, y); return g.measureText(t).width; };
  function draw(t) {
    g.fillStyle = '#000'; g.fillRect(0, 0, 320, 240);
    text(st.hdrL, 8, 17, '#e6e6e6'); if (st.hdrR) text(st.hdrR, 312, 17, '#9a9a9a', 13, 'right');
    g.fillStyle = '#1e1e1e'; g.fillRect(0, 24, 320, 1);
    if (st.big) {
      text(st.big, 160, 128, '#fff', 44, 'center', 650);
      if (st.bigSub) text(st.bigSub, 160, 158, '#9a9a9a', 14, 'center');
    }
    if (st.focusL) {
      if (st.box) { g.font = '600 13px system-ui, sans-serif'; const w = g.measureText(st.focusL).width; g.fillStyle = '#ff5533'; g.beginPath(); g.roundRect(5, 33, w + 8, 19, 2); g.fill(); text(st.focusL, 9, 47, '#000', 13, 'left', 600); }
      else text(st.focusL, 8, 47, '#e6e6e6');
    }
    if (st.topR) text(st.topR, 312, 47, '#8f8f8f', 13, 'right');
    if (LOOP && st.frac > 0 && !st.big) {
      const ncol = Math.round(W * st.frac), cy = Y + H / 2, b = st.bounds;
      const inSel = c => st.tint && c >= st.tint[0] * W && c < st.tint[1] * W;
      if (st.tint) { g.fillStyle = '#1d1d1d'; g.fillRect(X + st.tint[0] * W, Y, (st.tint[1] - st.tint[0]) * W, H); }
      for (let c = 0; c < ncol; c++) {
        const p = LOOP.pk[c] * H / 2 * .96, r = LOOP.rms[c] * H / 2 * .96, on = inSel(c);
        g.fillStyle = on ? '#9a9a9a' : '#4a4a4a'; g.fillRect(X + c, cy - p, 1, p * 2 || 1);
        g.fillStyle = on ? '#ffffff' : '#8a8a8a'; g.fillRect(X + c, cy - r, 1, r * 2 || 1);
      }
      if (b) {
        g.globalAlpha = st.fade;
        b.forEach((v, i) => { if (i === 0 || v >= 1) return; g.fillStyle = '#3d5a85'; g.fillRect(X + Math.round(v * W), Y, 1, H); });
        g.globalAlpha = 1;
      }
      if (st.cutDx != null) { g.fillStyle = '#ff5540'; g.fillRect(X + st.cutDx * W - 1, Y, 2, H); }
      if (st.thr != null) { g.strokeStyle = '#ff5533'; g.setLineDash([4, 3]); g.lineWidth = 1; [cy - st.thr * H / 2, cy + st.thr * H / 2].forEach(y => { g.beginPath(); g.moveTo(X, y); g.lineTo(X + W, y); g.stroke(); }); g.setLineDash([]); }
      let m = st.mark;
      if (st.anim) { const a = st.anim, k = (t - a.t) / a.dur; m = k >= 0 && k <= 1 ? a.from + (a.to - a.from) * k : (k > 1 ? null : m); if (k > 1) st.anim = null; }
      if (m != null) { g.fillStyle = '#ffd400'; g.fillRect(X + m * W - 1, Y, 2, H); }
    }
    if (st.pill) {
      g.fillStyle = '#d42a1e'; g.beginPath(); g.roundRect(186, 4, 126, 18, 9); g.fill();
      text(st.pill, 196, 17, '#fff', 12, 'left', 650);
      for (let i = 0; i < 10; i++) { g.fillStyle = i < st.level * 10 ? '#fff' : '#ffffff40'; g.fillRect(260 + i * 4.6, 8, 3, 10); }
    }
    if (st.botL) text(st.botL, 8, 210, '#e6e6e6');
    if (st.view) text('VIEW', 160, 210, '#00cc66', 13, 'center');
    if (st.botR) text(st.botR, 312, 210, '#8f8f8f', 13, 'right');
  }
  onFrame(draw);
  return { el: box, st };
}

/* ---------- Pads (View: green mode row + 7 rows of 4 slice pads) ---------- */
const YEL = ['#3a370e', '#ffff61'], GRN = ['#14321d', '#2fcf5a'];
function Pads({ rows = 8, cols = 4, label = '' } = {}) {
  const wrap = el('div', { className: 'pads' }), list = [];
  wrap.style.gridTemplateColumns = `repeat(${cols}, 1fr)`;
  for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) { const i = el('i'); const pal = r === 0 ? GRN : YEL; i.style.setProperty('--off', pal[0]); i.style.setProperty('--on', pal[1]); wrap.append(i); list.push(i); }
  const box = el('div', { className: 'padblock' }, wrap, el('p', { className: 'padcap', textContent: label }));
  const pad = k => list[4 + k];                                   /* slice k → yellow pad */
  return {
    el: box, list, pad,
    mode(m) { list[0].classList.toggle('on', m === 0); list[1].classList.toggle('on', m === 1); },
    flash(k, t, len = .14) { vis(t, () => { const p = pad(k); if (!p) return; p.classList.add('on'); later(len, () => p.classList.remove('on')); }); },
    keys() { const black = [1, 3, 6, 8, 10]; list.slice(4).forEach((p, i) => { const n = i % 12; p.style.setProperty('--off', black.includes(n) ? '#3a3a3a' : '#9a9a9a'); p.style.setProperty('--on', '#ffffff'); }); },
  };
}
function Steps(vals) {
  const row = el('div', { className: 'steps16' }), cells = vals.map(v => el('i', { textContent: v + 1 }));
  row.append(...cells);
  return { el: row, cells, vals, set(i, v) { vals[i] = v; cells[i].textContent = v + 1; cells[i].classList.remove('chg'); void cells[i].offsetWidth; cells[i].classList.add('chg'); } };
}
function Knob(name) {
  const k = el('div', { className: 'knob2' }, el('i')), lab = el('span', { textContent: name });
  let a = -120;
  return { el: el('div', { className: 'knobline' }, k, lab), turn(d) { a = clamp(a + d * 14, -135, 135); k.style.setProperty('--turn', a + 'deg'); }, hot(on) { k.classList.toggle('hot', on); }, label(t) { lab.textContent = t; } };
}

/* ---------- Players ---------- */
function playSlice(dev, b, k, t, rate = 1, gain = 1) {
  const a = b[k], z = b[k + 1]; if (z == null) return;
  const dur = (z - a) * LOOP_SEC;
  playSeg(t, a * LOOP_SEC, dur, rate, gain);
  if (dev) vis(t, () => { dev.st.anim = { t, from: a, to: z, dur: dur / rate }; });
}
/* run fn(i, t) on a fixed grid, scheduled a little ahead */
function every(dt, fn, start = now() + .08) {
  let i = 0;
  onFrame(t => { while (start + i * dt < t + .15) { fn(i, start + i * dt); i++; } });
}

/* ---------- Slide helpers ---------- */
const head = (eyebrow, title, text, color) => {
  const e = el('p', { className: 'eyebrow2', textContent: eyebrow }); e.style.setProperty('--c', color);
  const say = el('p', { className: 'say' }); say.innerHTML = text;
  return el('div', {}, e, el('h1', { textContent: title }), say);
};
const two = (left, right) => el('section', { className: 'slide two' }, left, right);
const FLIP = [0, 1, 4, 3, 0, 1, 8, 9, 12, 13, 2, 3, 0, 14, 10, 7];   /* a new groove from the same slices */

/* ---------- Slides ---------- */
const SLIDES = [];
const add = (chapter, steps, build, inShort = false) => SLIDES.push({ chapter, steps, build, inShort });

/* 1 cold open */
add(null, 3, () => {
  const d = Dev('big'), cap = el('p', { className: 'cap', textContent: 'A two-bar drum loop' }), p = Pads();
  d.st.hdrL = 'T1 Sample'; d.st.view = false; d.st.focusL = 'Loop 01'; d.st.topR = '100 BPM';
  const row = el('div', { className: 'devrow' }, d.el, p.el);
  p.el.style.display = 'none';
  let mode = 0;
  every(SPB / 2, (i, t) => {
    const b = d.st.bounds || grid(16);
    if (mode === 0) { if (i % 16 === 0) { playSeg(t, 0, LOOP_SEC); vis(t, () => { d.st.anim = { t, from: 0, to: 1, dur: LOOP_SEC }; }); } }
    else if (mode === 1) { playSlice(d, b, i % 16, t); vis(t, () => { d.st.tint = [b[i % 16], b[i % 16 + 1]]; }); }
    else { const k = FLIP[i % 16]; playSlice(d, b, k, t); p.flash(k, t); vis(t, () => { d.st.tint = [b[k], b[k + 1]]; d.st.topR = `Slice ${k + 1}/16`; }); }
  });
  return {
    root: el('section', { className: 'slide cold' }, row, cap),
    step: n => {
      mode = n;
      if (n >= 1) {
        d.st.hdrL = 'T1 Slicer 16'; d.st.view = true; d.st.focusL = '16 Slices'; d.st.botL = 'Grid'; d.st.botR = 'Gate 100%';
        const b = grid(16); d.st.bounds = b.slice(0, 1); b.forEach((v, i) => later(i * .05, () => { d.st.bounds = b.slice(0, i + 1).concat(1); }));
        cap.textContent = 'Sixteen slices on the Polyend Play';
      }
      if (n === 2) { p.el.style.display = ''; p.mode(0); cap.textContent = 'Sixteen pads to play them'; }
    },
  };
}, true);

/* 2 title */
add(null, 1, () => {
  const ROWS = [0x00000, 0x00000, 0x55537, 0x55555, 0x56557, 0x54551, 0x74257, 0x03000], MAC = 0x70000;
  const wm = el('div', { className: 'wordmark' }), cells = [];
  for (let r = 0; r < 8; r++) for (let c = 0; c < 20; c++) { const i = el('i'); wm.append(i); cells.push([i, (ROWS[r] >> c) & 1, r === 0 && (MAC >> c) & 1]); }
  cells.forEach(([i, on, mac]) => { if (on) later(.1 + Math.random() * .7, () => { i.style.background = '#d7d7d7'; }); else if (mac) later(1, () => { i.style.background = '#ffffff'; }); else if (Math.random() < .3) { later(Math.random() * .6, () => { i.style.background = '#3a3a3a'; later(.15, () => { i.style.background = ''; }); }); } });
  return { root: el('section', { className: 'slide title-slide title2' }, wm, el('h1', { textContent: 'envyū 0.2' }), el('p', { className: 'say', textContent: 'Beat slicer · sampling · tap tempo — a free community mod for the Polyend Play' })) };
}, true);

/* 3 chapters */
add(null, 4, () => {
  const items = [['01', 'Beat Slicer', 'Cut, play and rearrange any sample', C.sl], ['02', 'Sampling', 'Record straight from your computer', C.smp], ['03', 'Tempo & Randomize', 'Tap tempo and new random types', C.rnd], ['04', 'And more', 'Small things that add up', C.more]];
  const list = el('ol', { className: 'chapters' });
  items.forEach(([n, t, sub, c]) => { const li = el('li', {}, el('span', { textContent: n }), el('div', {}, t, el('small', { textContent: sub }))); li.style.setProperty('--c', c); list.append(li); });
  return { root: el('section', { className: 'slide', style: 'align-content:center' }, el('h1', { textContent: "What's new" }), list), step: n => [...list.children].forEach((li, i) => li.classList.toggle('on', i <= n)) };
});

/* 4 engines menu */
add('sl', 1, () => {
  const disp = el('div', { className: 'bigdisp' }, el('div', { className: 'pd' }));
  const rows = [...Array(6)].map((_, i) => ({ label: 'Track ' + (i + 1), value: 'Normal' }));
  const draw = (edit, toast) => PD.menu(disp.firstChild, { title: 'Engines', rows, sel: 0, editing: edit, toast });
  draw(false);
  later(1.2, () => draw(true)); later(2.2, () => { rows[0].value = 'Slicer'; draw(true); }); later(3, () => draw(false, 'Track 1: Slicer'));
  return { root: two(head('01 Beat Slicer', 'Any track becomes a slicer', 'Menu → <b>Engines</b> → set the track to <b>Slicer</b>.', C.sl), el('div', { className: 'demo' }, disp)) };
});

/* 5 grid or beat */
add('sl', 3, () => {
  const d = Dev('mid'), k = Knob('Track Length'), say = el('p', { className: 'modetext' });
  d.st.botR = 'Gate 100%';
  let b = grid(16);
  every(SPB / 2, (i, t) => { const n = b.length - 1, j = i % n; if (j < n) { playSlice(d, b, j, t, 1, .8); vis(t, () => { d.st.tint = [b[j], b[j + 1]]; }); } });
  const setGrid = n => { b = grid(n); d.st.bounds = b; d.st.focusL = n + ' Slices'; d.st.hdrL = 'T1 Slicer ' + n; d.st.botL = 'Grid'; d.st.topR = ''; };
  let sweep = null;
  onFrame(t => { if (!sweep) return; const k2 = clamp((t - sweep.t0) / 1.6, 0, 1), thr = sweep.from + (sweep.to - sweep.from) * k2; d.st.thr = thr; b = ensure0(beat(thr)); d.st.bounds = b; const lv = Math.round(1 + (1.05 - thr) / .9 * 15); d.st.focusL = `Sensitivity ${clamp(lv, 1, 16)}/16`; d.st.hdrL = 'T1 Slicer Beat'; d.st.topR = `Beat ${b.length - 1}`; });
  return {
    root: two(head('01 Beat Slicer', 'Grid or Beat', 'Cut evenly into <b>4 to 32</b> slices, or at the <b>hits</b> of the sample.', C.sl), el('div', { className: 'demo' }, d.el, k.el, say)),
    step: n => {
      clearLater();
      if (n === 0) { sweep = null; d.st.thr = null; d.st.box = true; say.textContent = 'Grid: equal slices.'; setGrid(4); [8, 16].forEach((m, i) => later(.8 + i * .8, () => { k.turn(2); setGrid(m); })); }
      else {
        d.st.botL = 'Beat'; d.st.box = true; say.textContent = n === 1 ? 'Beat: a threshold sinks through the waveform. Every hit above it starts a slice.' : 'More sensitivity finds the quieter hits too.';
        sweep = { from: n === 1 ? 1.05 : .55, to: n === 1 ? .55 : .15, t0: now() };
        k.turn(3);
      }
    },
  };
});

/* 6 pads */
add('sl', 2, () => {
  const d = Dev('mid'), p = Pads({ label: 'yellow pads = slices' }), b = grid(16);
  d.st.bounds = b; d.st.botL = 'Grid'; d.st.botR = 'Gate 100%';
  p.mode(0);
  const seq = [0, null, 4, 0, 8, 9, 12, null, 0, 13, 4, 2, 8, 14, 15, 7];
  every(SPB / 2, (i, t) => { const k = seq[i % 16]; if (k == null) return; playSlice(d, b, k, t); p.flash(k, t); vis(t, () => { d.st.tint = [b[k], b[k + 1]]; d.st.focusL = `Slice ${k + 1}/16`; }); });
  return { root: two(head('01 Beat Slicer', 'Pads play slices', 'In <b>View</b>, each yellow pad is one slice. One-shot: the slice plays to its end.', C.sl), el('div', { className: 'demo' }, el('div', { className: 'devrow' }, d.el, p.el))), step: () => {} };
}, true);

/* 7 steps choose slices */
add('sl', 2, () => {
  const d = Dev('small'), p = Pads(), b = grid(16), st = Steps([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]), enc = el('p', { className: 'hand' });
  d.st.bounds = b; d.st.botL = 'Grid'; p.mode(0);
  every(SPB / 2, (i, t) => { const j = i % 16, k = st.vals[j]; playSlice(d, b, k, t); vis(t, () => { st.cells.forEach((c, x) => c.classList.toggle('now', x === j)); d.st.tint = [b[k], b[k + 1]]; }); });
  return {
    root: el('section', { className: 'slide' }, el('div', { className: 'stack' }, head('01 Beat Slicer', 'Steps choose slices', 'Hold a step and tap a pad – or turn the <b>encoder</b> while you hold it.', C.sl), el('div', { className: 'devrow' }, d.el, p.el, enc), st.el)),
    step: n => {
      clearLater();
      if (n === 0) {
        const plan = [[5, 12], [9, 0], [14, 4]];
        plan.forEach(([s0, v], i) => { const t = i * 1.6; later(t + .3, () => { st.cells[s0].classList.add('held'); enc.textContent = `hold step ${s0 + 1}`; }); later(t + .8, () => { p.pad(v).classList.add('held'); st.set(s0, v); enc.textContent = `+ pad ${v + 1}`; }); later(t + 1.3, () => { st.cells[s0].classList.remove('held'); p.pad(v).classList.remove('held'); enc.textContent = ''; }); });
      } else {
        later(.2, () => { st.cells[2].classList.add('held'); enc.textContent = 'hold step 3 + encoder ▸'; });
        [3, 4, 5, 6, 8].forEach((v, i) => later(.7 + i * .45, () => st.set(2, v)));
        later(3.4, () => { st.cells[2].classList.remove('held'); enc.textContent = ''; });
      }
    },
  };
});

/* 8 keys */
add('sl', 1, () => {
  const d = Dev('small'), p = Pads({ label: 'green pad 2 = Keys' }), b = grid(16);
  d.st.bounds = b; d.st.botL = 'Grid'; d.st.tint = [b[0], b[1]]; d.st.focusL = 'Slice 1/16'; d.st.topR = 'Keys';
  p.mode(1); p.keys();
  const mel = [0, null, 3, 7, 10, null, 7, 12, 0, null, 3, 5, 7, null, 3, -2];
  every(SPB / 2, (i, t) => { const n = mel[i % 16]; if (n == null) return; playSlice(d, b, 0, t, 2 ** (n / 12)); const pi = clamp(n + 6, 0, 27); vis(t, () => { const q = p.list[4 + pi]; q.classList.add('on'); later(.15, () => q.classList.remove('on')); }); });
  return { root: two(head('01 Beat Slicer', 'Keys: a slice in tune', 'The <b>green pads</b> switch modes. Pad 2 is <b>Keys</b>: the pads play the selected slice across the scale.', C.sl), el('div', { className: 'demo' }, el('div', { className: 'devrow' }, d.el, p.el))) };
});

/* 9 grid fit */
add('sl', 2, () => {
  const d = Dev('mid'), k = Knob('Micromove · grid width');
  d.st.botL = 'Grid'; d.st.box = true;
  let w = 1;
  const apply = () => { const b = grid(16, w); d.st.bounds = b; d.st.focusL = Math.abs(w - 1) < .001 ? 'Grid Fit' : `${(BPM / w).toFixed(1)} BPM`; };
  apply();
  every(SPB / 2, (i, t) => { const b = d.st.bounds, j = i % 16; if (b[j + 1] > b[j]) { playSlice(d, b, j, t, 1, .8); vis(t, () => { d.st.tint = [b[j], b[j + 1]]; }); } });
  return {
    root: two(head('01 Beat Slicer', 'Fit the grid to the tempo', 'Stretch the grid with <b>Micromove</b>. The display shows the tempo it now matches. <b>Grid Fit</b> snaps back.', C.sl), el('div', { className: 'demo' }, d.el, k.el)),
    step: n => {
      clearLater();
      if (n === 0) [1.02, 1.04, 1.06, 1.04, 1.03].forEach((v, i) => later(.4 + i * .6, () => { k.turn(v > w ? 1 : -1); w = v; apply(); }));
      else later(.3, () => { w = 1; k.turn(-2); apply(); });
    },
  };
});

/* 10 cut by hand */
add('sl', 2, () => {
  const d = Dev('mid'), p = Pads(), k = Knob('Sample Start'), hint = el('p', { className: 'hand' });
  let b = grid(16); const base = b[2], ms = 1 / (LOOP_SEC * 1000);
  d.st.bounds = b; d.st.botL = 'Grid'; d.st.box = true; p.mode(0);
  const show = cut => { b = grid(16); b[2] = base + cut * ms; d.st.bounds = b; d.st.tint = [b[2], b[3]]; d.st.cutDx = b[2]; };
  return {
    root: two(head('01 Beat Slicer', 'Cut by hand', '<b>Hold a slice pad</b> and turn <b>Sample Start</b>. Only this slice moves, in milliseconds.', C.sl), el('div', { className: 'demo' }, el('div', { className: 'devrow' }, d.el, p.el), el('div', { className: 'knobline' }, k.el, hint))),
    step: n => {
      clearLater();
      if (n === 0) {
        later(.3, () => { p.pad(2).classList.add('held'); hint.textContent = 'pad 3 held'; show(0); d.st.focusL = 'Cut 3 +0 ms'; k.hot(true); playSlice(d, b, 2, now() + .02); });
        [2, 5, 8, 12, 16, 20].forEach((c, i) => later(1 + i * .5, () => { k.turn(1); show(c); d.st.focusL = `Cut 3 +${c} ms`; if (i % 2) playSlice(d, b, 2, now() + .02); }));
      } else {
        p.pad(2).classList.remove('held'); k.hot(false); hint.textContent = 'pad released';
        d.st.focusL = 'Start +0%'; d.st.cutDx = null;
        [1, 2, 3].forEach((c, i) => later(.5 + i * .5, () => { k.turn(1); d.st.focusL = `Start +${c * 2}%`; }));
        later(2.3, () => { hint.textContent = 'slices stay where they are'; });
      }
    },
  };
});

/* 11 fine offset per step */
add('sl', 1, () => {
  const d = Dev('small'), b = grid(16), st = Steps(FLIP.slice()), k = Knob('Sample Start'), hint = el('p', { className: 'hand' });
  d.st.bounds = b; d.st.botL = 'Grid'; d.st.box = true;
  let off = 0; const HELD = 6;
  every(SPB / 2, (i, t) => { const j = i % 16, s0 = st.vals[j], late = j === HELD ? off * (b[s0 + 1] - b[s0]) : 0; playSlice(null, [b[s0] + late, b[s0 + 1]], 0, t); vis(t, () => { st.cells.forEach((c, x) => c.classList.toggle('now', x === j)); if (j === HELD) d.st.anim = { t, from: b[s0] + late, to: b[s0 + 1], dur: (b[s0 + 1] - b[s0] - late) * LOOP_SEC }; }); });
  const s0 = st.vals[HELD];
  d.st.tint = [b[s0], b[s0 + 1]]; d.st.focusL = 'Start +0%'; d.st.topR = `Slice ${s0 + 1}/16`;
  later(.4, () => { st.cells[HELD].classList.add('held'); hint.textContent = `step ${HELD + 1} held`; k.hot(true); });
  [4, 8, 12, 16, 20].forEach((v, i) => later(1 + i * .45, () => { off = v / 64; k.turn(1); d.st.tint = [b[s0] + off * (b[s0 + 1] - b[s0]), b[s0 + 1]]; d.st.focusL = `Start +${Math.round(v * 100 / 64)}%`; }));
  later(3.6, () => { st.cells[HELD].classList.remove('held'); k.hot(false); hint.textContent = 'only this step starts later'; });
  return { root: el('section', { className: 'slide' }, el('div', { className: 'stack' }, head('01 Beat Slicer', 'Groove per step', '<b>Hold a step</b> and turn <b>Sample Start</b>: just this step starts a little later.', C.sl), el('div', { className: 'devrow' }, d.el, el('div', {}, k.el, hint)), st.el)) };
});

/* 12 sampling from the computer */
add('smp', 3, () => {
  const disp = el('div', { className: 'bigdisp' }, el('div', { className: 'pd' })), d = Dev('mid'), opts = el('div', { className: 'panelbox' });
  const svg = s('svg', { viewBox: '0 0 900 220', class: 'flowsvg' });
  svg.append(s('rect', { x: 10, y: 50, width: 250, height: 120 }), s('text', { x: 135, y: 100 }, 'Computer'), s('text', { x: 135, y: 136, class: 'sub' }, 'any app'), s('rect', { x: 640, y: 50, width: 250, height: 120, class: 'hot' }), s('text', { x: 765, y: 100 }, 'Play'), s('text', { x: 765, y: 136, class: 'sub' }, 'Stereo + In'), s('path', { d: 'M270 110 L630 110' }));
  const box = el('div', { className: 'demo' }, svg, disp);
  PD.menu(disp.firstChild, { title: 'USB Audio', sel: 0, editing: true, rows: [{ label: 'Audio Out', value: 'Stereo + In' }, { label: 'Wait For Signal', value: 'On' }, { label: 'Pause On Silence', value: 'On' }] });
  return {
    root: two(head('02 Sampling', 'Sample from your computer', 'With <b>Stereo + In</b>, your computer plays through the Play. Hold <b>Live Rec</b> for a second to record it.', C.smp), box),
    step: n => {
      clearLater();
      if (n >= 1 && !box.contains(d.el)) {
        svg.remove(); disp.replaceWith(d.el); box.append(opts);
        d.st.hdrL = 'T1 Slicer 16'; d.st.view = false; d.st.frac = 0; d.st.pill = 'REC 0:00';
        const t0 = now() + .3;
        every(SPB / 2, (i, t) => { if (i < 16) playSeg(t, i * LOOP_SEC / 16, LOOP_SEC / 16, 1, .7); }, t0);
        onFrame(t => { const k = clamp((t - t0) / LOOP_SEC, 0, 1); d.st.frac = k; const c = Math.min(303, Math.floor(k * 303)); d.st.level = LOOP ? LOOP.rms[c] * 1.2 : 0; d.st.pill = `REC 0:0${Math.floor(k * LOOP_SEC)}`; if (k >= 1) { d.st.pill = null; d.st.level = 0; } });
        opts.replaceChildren(el('p', { className: 'modetext', textContent: 'Live Rec held: recording what comes in over USB.' }));
      }
      if (n === 2) opts.replaceChildren(el('p', { className: 'modename', textContent: 'Waits. Pauses.' }), el('p', { className: 'modetext', textContent: 'Wait For Signal starts on the first sound, Pause On Silence skips the gaps. Time and level stay on screen.' }));
    },
  };
});

/* 13 straight into the slicer */
add('smp', 1, () => {
  const d = Dev('big'), p = Pads(), b = ensure0(beat(.3));
  d.st.view = true; d.st.hdrL = 'T1 Slicer Beat'; d.st.botL = 'Beat'; d.st.focusL = 'Live Recording 004';
  d.st.bounds = [0, 1];
  b.forEach((v, i) => later(.6 + i * .08, () => { d.st.bounds = b.slice(0, i + 1).concat(1); }));
  later(1.8, () => { d.st.topR = `Beat ${b.length - 1}`; p.mode(0); });
  const seq = [0, 3, 1, 5, 0, 7, 2, 9];
  every(SPB / 2, (i, t) => { if (t < now() + 1.5 && i < 4) return; const k = seq[i % seq.length] % (b.length - 1); playSlice(d, b, k, t); p.flash(k, t); vis(t, () => { d.st.tint = [b[k], b[k + 1]]; }); }, now() + 2);
  return { root: el('section', { className: 'slide cold' }, el('div', { className: 'devrow' }, d.el, p.el), el('p', { className: 'cap', textContent: 'The recording lands on the slicer track, already cut.' })) };
});

/* 14 tap tempo */
add('rnd', 1, () => {
  const d = Dev('mid'), k = Knob('Tempo knob: tap it');
  d.st.hdrL = 'Menu'; d.st.view = false; d.st.big = '96 BPM'; d.st.bigSub = 'touch the tempo knob in rhythm';
  const taps = [.6, 1.2, 1.8, 2.4, 3.0, 3.6, 4.2];
  taps.forEach((tt, i) => later(tt, () => { click(now(), true); k.hot(true); later(.1, () => k.hot(false)); d.st.pill = null; d.st.big = i < 5 ? `TAP ${i + 1}` : '100 BPM'; d.st.bigSub = i < 5 ? 'set from the sixth tap' : 'steady'; }));
  later(4.6, () => every(SPB, (i, t) => click(t, i % 4 === 0)));
  return { root: two(head('03 Tempo & Randomize', 'Tap tempo', 'Open the menu and <b>tap the tempo knob</b>. From the sixth tap, the tempo is set and stays steady.', C.rnd), el('div', { className: 'demo' }, d.el, k.el)) };
});

/* 15 randomize */
add('rnd', 2, () => {
  const types = ['Swap Steps', 'Scatter Steps', 'Thin Out', 'Melodic Walk', 'Accent', 'Call & Response', 'Humanize', 'Octave Jump', 'Note Length', 'Shuffle'];
  const tl = el('div', { className: 'typelist' }); types.forEach(t => tl.append(el('span', { textContent: t })));
  const st = Steps(FLIP.slice()), b = grid(16);
  every(SPB / 2, (i, t) => { const j = i % 16; playSlice(null, b, st.vals[j], t); vis(t, () => st.cells.forEach((c, x) => c.classList.toggle('now', x === j))); });
  return {
    root: el('section', { className: 'slide' }, el('div', { className: 'stack' }, head('03 Tempo & Randomize', 'New randomize types', 'For MIDI and sample tracks. Before you commit, the <b>pads show what will change</b>.', C.rnd), tl, st.el)),
    step: n => {
      clearLater();
      if (n === 0) [...tl.children].forEach((c, i) => later(.2 + i * .25, () => c.classList.add('on')));
      else {
        [...tl.children].forEach((c, i) => c.classList.toggle('on', i === 0));
        const pick = [3, 7, 10, 14];
        later(.4, () => pick.forEach(i => st.cells[i].classList.add('held')));
        later(1.8, () => { pick.forEach((i, x) => { st.set(i, FLIP[pick[(x + 1) % pick.length]]); st.cells[i].classList.remove('held'); }); });
      }
    },
  };
});

/* 16 and more */
add('more', 5, () => {
  const items = [['A waveform for every sample track', 'Start, end, attack and decay drawn while you turn'], ['Perform keeps your selection', 'and the mute pads flash on every hit'], ['Color Mode', 'for red-green and blue-yellow color blindness'], ['USB audio on Windows', 'detected with the built-in driver'], ['Safety net', 'if the mod is ever damaged, the Play starts as stock']];
  const list = el('ul', { className: 'bullets' });
  items.forEach(([t, sub]) => list.append(el('li', {}, t, el('small', { textContent: sub }))));
  return { root: el('section', { className: 'slide', style: 'align-content:center' }, el('p', { className: 'eyebrow2', textContent: '04 And more', style: `--c:${C.more}` }), list), step: n => [...list.children].forEach((li, i) => li.classList.toggle('on', i <= n)) };
});

/* 17 try it */
add(null, 2, () => {
  const items = ['Download the official firmware 1.6.0 from Polyend.', 'Patch it in your browser. Nothing leaves your computer.', 'Install it from the firmware update menu.'];
  const list = el('ol', { className: 'steps2' });
  items.forEach((t, i) => list.append(el('li', { className: 'on' }, el('span', { textContent: String(i + 1) }), t)));
  const teaser = el('p', { className: 'teaser', textContent: 'Next: the Play as a grid controller for norns.' });
  return { root: el('section', { className: 'slide', style: 'align-content:center' }, el('h1', { textContent: 'Try it yourself' }), list, el('p', { className: 'warn2', textContent: 'Free · unofficial · link in the description. Back up your SD card first.' }), teaser), step: n => teaser.classList.toggle('on', n === 1) };
}, true);

/* ---------- Deck ---------- */
const DECK = SHORT ? SLIDES.filter(x => x.inShort) : SLIDES;
const stage = $('stage'), pips = $('pips');
let idx = -1, stepIdx = 0, cur = null;
DECK.forEach((sl, i) => {
  const b = el('button', { type: 'button', ariaLabel: `Slide ${i + 1}`, onclick: () => go(i, 0) });
  if (i && sl.chapter !== DECK[i - 1].chapter) b.classList.add('gap');
  pips.append(b);
});
function go(i, st) {
  i = clamp(i, 0, DECK.length - 1);
  if (i !== idx || !cur) {
    frames = []; queue = []; clearLater(); stopAll();
    idx = i; cur = DECK[i].build();
    stage.replaceChildren(cur.root);
  }
  stepIdx = clamp(st, 0, DECK[i].steps - 1);
  if (cur.step) cur.step(stepIdx);
  const col = C[DECK[i].chapter] || '#ffb238';
  [...pips.children].forEach((b, k) => { b.className = (b.classList.contains('gap') ? 'gap ' : '') + (k === i ? 'now' : k < i ? 'done' : ''); });
  pips.style.setProperty('--c', col);
  history.replaceState(null, '', `${location.search}#${i + 1}.${stepIdx + 1}`);
}
function next() { if (stepIdx < DECK[idx].steps - 1) go(idx, stepIdx + 1); else if (idx < DECK.length - 1) go(idx + 1, 0); }
function prev() { if (stepIdx > 0) go(idx, stepIdx - 1); else if (idx > 0) go(idx - 1, DECK[idx - 1].steps - 1); }
const firstOf = ch => DECK.findIndex(sl => sl.chapter === ch);
addEventListener('keydown', e => {
  const k = e.key.toLowerCase();
  if (k === 'arrowright' || k === 'pagedown' || (k === ' ' && e.target.tagName !== 'BUTTON')) { e.preventDefault(); next(); }
  else if (k === 'arrowleft' || k === 'pageup') { e.preventDefault(); prev(); }
  else if (k === '1') go(firstOf('sl'), 0); else if (k === '2') go(firstOf('smp'), 0); else if (k === '3') go(firstOf('rnd'), 0); else if (k === '4') go(firstOf('more'), 0);
  else if (k === 'home') go(0, 0);
  else if (k === 's') { sound = !sound; if (sound) { audio().resume(); } else stopAll(); }
  else if (k === 'f') { if (document.fullscreenElement) document.exitFullscreen(); else document.documentElement.requestFullscreen(); }
  else if (k === 'h') document.body.classList.toggle('nokeys');
});
function fit() {
  const W = SHORT ? 1080 : 1920, H = SHORT ? 1920 : 1080, sc = Math.min(innerWidth / W, innerHeight / H);
  $('fit').style.transform = `scale(${sc}) translate(-50%, -50%)`;
}
addEventListener('resize', fit); fit();
let idleTimer = 0;
const awake = () => { document.body.classList.remove('idle'); clearTimeout(idleTimer); idleTimer = setTimeout(() => document.body.classList.add('idle'), 2500); };
addEventListener('pointermove', awake); addEventListener('pointerdown', awake); awake();
const m = location.hash.match(/^#(\d+)(?:\.(\d+))?/);
go(m ? +m[1] - 1 : 0, m && m[2] ? +m[2] - 1 : 0);
loopReady.then(() => document.body.dataset.loop = 'ready', e => { document.body.dataset.loop = 'error: ' + e; });

/* ---------- Render mode: fixed timeline for the video (?render). [slide number, step, seconds] ---------- */
const TIMELINE = [
  [1, 0, 5], [1, 1, 5], [1, 2, 8],
  [2, 0, 7],
  [4, 0, 7],
  [5, 0, 6], [5, 1, 7], [5, 2, 5],
  [6, 0, 10],
  [7, 0, 6], [7, 1, 5],
  [8, 0, 9],
  [9, 0, 5], [9, 1, 3],
  [10, 0, 6], [10, 1, 5],
  [11, 0, 8],
  [12, 0, 5], [12, 1, 7], [12, 2, 5],
  [13, 0, 8],
  [14, 0, 8],
  [15, 0, 5], [15, 1, 5],
  [16, 0, 3.5], [16, 1, 3.5], [16, 2, 3.5], [16, 3, 3.5], [16, 4, 3.5],
  [17, 0, 9], [17, 1, 6],
];
const TOTAL = TIMELINE.reduce((a, x) => a + x[2], 0);
function startRender() {
  T0 = now(); AUDIO_LOG.length = 0; let last = -1;
  const starts = []; let acc = 0; TIMELINE.forEach(x => { starts.push(acc); acc += x[2]; });
  window.AUTO = t => {
    const e = t - T0; if (e >= TOTAL) { window.RENDER_DONE = true; return; }
    let k = 0; while (k + 1 < TIMELINE.length && starts[k + 1] <= e) k++;
    if (k !== last) { last = k; const [sl, st] = TIMELINE[k]; go(sl - 1, st); }
  };
  return TOTAL;
}
window.startRender = startRender; window.AUDIO_LOG = AUDIO_LOG; window.TIMELINE = TIMELINE; window.TOTAL = TOTAL;
window.loopData = () => LOOP ? Array.from(LOOP.buf.getChannelData(0), v => Math.round(v * 32767)) : null;
if (RENDER) document.body.classList.add('nokeys', 'idle', 'render');
