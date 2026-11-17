'use strict';
/* Beat Slicer demo. The drum loop is synthesized in the browser (no samples). Uses audio(), master, stopAll and
   playButton from app.js, PD.knob from display.js. The display is drawn here; it approximates the device. */
(() => {
  const BPM = 100, SPB = 60 / BPM, LOOP_SEC = 8 * SPB, SR = 44100, S16 = SPB / 4;
  const PAT = { kick: [0, 6, 10, 16, 22, 27], snare: [8, 24], ghost: [15, 30], hat: [...Array(16)].map((_, i) => i * 2), open: [14],
    bass: [[0, 45, 5], [6, 48, 3], [10, 45, 4], [16, 41, 5], [22, 43, 4], [27, 45, 3]] };
  const ONSETS = [...PAT.kick.map(i => [i, 1]), ...PAT.snare.map(i => [i, .9]), ...PAT.ghost.map(i => [i, .4]),
    ...PAT.hat.filter(i => !PAT.kick.includes(i) && !PAT.snare.includes(i)).map(i => [i, i % 4 ? .18 : .26]), ...PAT.open.map(i => [i, .3])].sort((a, b) => a[0] - b[0]);
  const GROOVE = [0, 1, 4, 3, 0, 1, 8, 9, 12, 13, 2, 3, 0, 14, 10, 7];
  const MEL = [0, null, 3, 7, 10, null, 7, 12, 0, null, 3, 5, 7, null, 3, -2];
  const COUNTS = [4, 8, 12, 16, 24, 32];
  let LOOP = null;

  async function makeLoop() {
    const len = Math.round(LOOP_SEC * SR), oc = new OfflineAudioContext(1, len, SR);
    const comp = oc.createDynamicsCompressor(); comp.threshold.value = -14; comp.ratio.value = 4; comp.connect(oc.destination);
    const bus = oc.createGain(); bus.gain.value = .85; bus.connect(comp);
    const nb = oc.createBuffer(1, SR, SR), nd = nb.getChannelData(0); let seed = 7;
    for (let i = 0; i < SR; i++) { seed = (seed * 16807) % 2147483647; nd[i] = seed / 1073741823.5 - 1; }
    const T = i => i * S16;
    const noise = (t, dur, type, f, q, vol) => { const s = oc.createBufferSource(); s.buffer = nb; const fl = oc.createBiquadFilter(); fl.type = type; fl.frequency.value = f; fl.Q.value = q; const g = oc.createGain(); g.gain.setValueAtTime(vol, t); g.gain.exponentialRampToValueAtTime(.001, t + dur); s.connect(fl); fl.connect(g); g.connect(bus); s.start(t, (t * 7.3) % .5, dur + .02); };
    const tone = (t, f0, f1, dur, vol, type = 'sine') => { const o = oc.createOscillator(); o.type = type; o.frequency.setValueAtTime(f0, t); o.frequency.exponentialRampToValueAtTime(f1, t + dur * .35); const g = oc.createGain(); g.gain.setValueAtTime(vol, t); g.gain.exponentialRampToValueAtTime(.001, t + dur); o.connect(g); g.connect(bus); o.start(t); o.stop(t + dur + .02); };
    PAT.kick.forEach(i => { tone(T(i), 140, 46, .42, 1); noise(T(i), .02, 'highpass', 2500, .7, .25); });
    PAT.snare.forEach(i => { noise(T(i), .22, 'bandpass', 1900, .8, .9); tone(T(i), 210, 170, .12, .45, 'triangle'); });
    PAT.ghost.forEach(i => noise(T(i), .1, 'bandpass', 2100, .9, .3));
    PAT.hat.forEach(i => noise(T(i), .045, 'highpass', 7500, .6, i % 4 ? .16 : .24));
    PAT.open.forEach(i => noise(T(i), .28, 'highpass', 6500, .5, .2));
    PAT.bass.forEach(([i, n, l]) => { const t = T(i), o = oc.createOscillator(), lp = oc.createBiquadFilter(), g = oc.createGain(); o.type = 'sawtooth'; o.frequency.value = 440 * 2 ** ((n - 69) / 12); lp.type = 'lowpass'; lp.frequency.setValueAtTime(900, t); lp.frequency.exponentialRampToValueAtTime(160, t + .25); g.gain.setValueAtTime(.0001, t); g.gain.exponentialRampToValueAtTime(.42, t + .01); g.gain.setValueAtTime(.42, t + l * S16 * .8); g.gain.exponentialRampToValueAtTime(.001, t + l * S16); o.connect(lp); lp.connect(g); g.connect(bus); o.start(t); o.stop(t + l * S16 + .05); });
    const buf = await oc.startRendering(), d = buf.getChannelData(0);
    const N = 304, pk = new Float32Array(N), rms = new Float32Array(N), per = d.length / N; let mx = 0;
    for (let c = 0; c < N; c++) { let p = 0, q = 0; const a = Math.floor(c * per), b = Math.floor((c + 1) * per); for (let i = a; i < b; i++) { const v = Math.abs(d[i]); if (v > p) p = v; q += v * v; } pk[c] = p; rms[c] = Math.sqrt(q / (b - a)); mx = Math.max(mx, p); }
    for (let c = 0; c < N; c++) { pk[c] /= mx; rms[c] = Math.min(1, rms[c] / mx * 1.6); }
    LOOP = { buf, pk, rms }; draw();
  }

  /* ---------- state ---------- */
  const st = { mode: 'grid', count: 16, sens: 8, sel: 0, cut: {}, anim: null, lit: -1 };
  const grid = n => [...Array(n + 1)].map((_, i) => i / n);
  const beatBounds = () => { const thr = 1.05 - (st.sens - 1) / 15 * .9; const b = ONSETS.filter(o => o[1] >= thr).map(o => o[0] / 32); if (b[0] !== 0) b.unshift(0); return [...new Set(b), 1]; };
  function bounds() {
    const b = st.mode === 'beat' ? beatBounds() : grid(st.count);
    for (const k in st.cut) if (b[k] != null && k > 0 && k < b.length - 1) b[k] = Math.min(b[+k + 1] - .004, Math.max(b[k - 1] + .004, b[k] + st.cut[k] / 1000 / LOOP_SEC));
    return b;
  }
  const nSlices = () => bounds().length - 1;

  /* ---------- display ---------- */
  const cv = document.getElementById('slCanvas'), g = cv.getContext('2d');
  g.scale(cv.width / 320, cv.height / 240);
  function text(t, x, y, col, size = 13, align = 'left', w = 400) { g.font = `${w} ${size}px system-ui, -apple-system, sans-serif`; g.fillStyle = col; g.textAlign = align; g.fillText(t, x, y); return g.measureText(t).width; }
  function draw() {
    const now = performance.now() / 1000, b = bounds(), n = b.length - 1, X = 8, Y = 62, W = 304, H = 116, cy = Y + H / 2;
    g.fillStyle = '#000'; g.fillRect(0, 0, 320, 240);
    text(st.mode === 'beat' ? 'T1 Slicer Beat' : `T1 Slicer ${n}`, 8, 17, '#e6e6e6');
    g.fillStyle = '#1e1e1e'; g.fillRect(0, 24, 320, 1);
    const focus = { grid: `${st.count} Slices`, beat: `Sensitivity ${st.sens}/16`, keys: `Slice ${st.sel + 1}/${n}`, cut: `Cut ${st.sel + 1} ${(st.cut[st.sel] || 0) >= 0 ? '+' : ''}${st.cut[st.sel] || 0} ms` }[st.mode];
    g.font = '600 13px system-ui, sans-serif'; const fw = g.measureText(focus).width; g.fillStyle = '#ff5533'; g.fillRect(5, 33, fw + 8, 19); text(focus, 9, 47, '#000', 13, 'left', 600);
    text(st.mode === 'beat' ? `Beat ${n}` : st.mode === 'keys' ? 'Keys' : `Slice ${Math.min(st.sel, n - 1) + 1}/${n}`, 312, 47, '#8f8f8f', 13, 'right');
    if (LOOP) {
      const s0 = Math.min(st.sel, n - 1), t0 = b[s0], t1 = b[s0 + 1];
      g.fillStyle = '#1d1d1d'; g.fillRect(X + t0 * W, Y, (t1 - t0) * W, H);
      for (let c = 0; c < W; c++) {
        const p = LOOP.pk[c] * H / 2 * .96, r = LOOP.rms[c] * H / 2 * .96, on = c >= t0 * W && c < t1 * W;
        g.fillStyle = on ? '#9a9a9a' : '#4a4a4a'; g.fillRect(X + c, cy - p, 1, p * 2 || 1);
        g.fillStyle = on ? '#fff' : '#8a8a8a'; g.fillRect(X + c, cy - r, 1, r * 2 || 1);
      }
      for (let i = 1; i < n; i++) { g.fillStyle = '#3d5a85'; g.fillRect(X + Math.round(b[i] * W), Y, 1, H); }
      if (st.mode === 'cut' && s0 > 0) { g.fillStyle = '#ff5540'; g.fillRect(X + t0 * W - 1, Y, 2, H); }
      if (st.mode === 'beat') { const thr = 1.05 - (st.sens - 1) / 15 * .9; g.strokeStyle = '#ff5533'; g.setLineDash([4, 3]); [cy - thr * H / 2, cy + thr * H / 2].forEach(y => { g.beginPath(); g.moveTo(X, y); g.lineTo(X + W, y); g.stroke(); }); g.setLineDash([]); }
      if (st.anim) { const k = (now - st.anim.t) / st.anim.dur; if (k >= 0 && k <= 1) { g.fillStyle = '#ffd400'; g.fillRect(X + (st.anim.from + (st.anim.to - st.anim.from) * k) * W - 1, Y, 2, H); } else if (k > 1) st.anim = null; }
    } else text('…', 160, 128, '#8f8f8f', 16, 'center');
    text(st.mode === 'beat' ? 'Beat' : 'Grid', 8, 210, '#e6e6e6');
    text('VIEW', 160, 210, '#00cc66', 13, 'center');
    text('Gate 100%', 312, 210, '#8f8f8f', 13, 'right');
  }
  (function loop() { if (st.anim || !LOOP) draw(); requestAnimationFrame(loop); })();

  /* ---------- sound ---------- */
  function playSlice(k, when, rate = 1, nodes) {
    if (!LOOP) return;
    const b = bounds(); if (k >= b.length - 1) return;
    const a = b[k], z = b[k + 1], dur = (z - a) * LOOP_SEC, c = audio(), t = when ?? c.currentTime + .01;
    const src = c.createBufferSource(), gn = c.createGain(); src.buffer = LOOP.buf; src.playbackRate.value = rate;
    gn.gain.setValueAtTime(1.6, t); gn.gain.setValueAtTime(1.6, t + Math.max(.004, dur / rate - .008)); gn.gain.linearRampToValueAtTime(0, t + dur / rate);
    src.connect(gn); gn.connect(master); src.start(t, a * LOOP_SEC, dur + .02);
    if (nodes) nodes.push(src);
    const delay = Math.max(0, (t - c.currentTime) * 1000);
    return setTimeout(() => { st.anim = { t: performance.now() / 1000, from: a, to: z, dur: dur / rate }; st.lit = k; pads(); setTimeout(() => { if (st.lit === k) { st.lit = -1; pads(); } }, 140); }, delay);
  }

  /* ---------- pads: green row (Slices, Keys) + 7 rows of 4 ---------- */
  const padBox = document.getElementById('slPads'), PADBTN = [];
  for (let r = 0; r < 8; r++) for (let c = 0; c < 4; c++) {
    const b = document.createElement('button'); b.type = 'button'; b.className = 'slpad' + (r === 0 ? ' green' : '');
    const k = (r - 1) * 4 + c;
    if (r === 0) { b.textContent = ['Slices', 'Keys', '', ''][c]; b.setAttribute('aria-label', c < 2 ? `Mode ${b.textContent}` : 'unused'); if (c > 1) b.disabled = true; }
    const press = () => {
      if (r === 0) { if (c < 2) setMode(c ? 'keys' : (st.mode === 'keys' ? 'grid' : st.mode)); return; }
      if (st.mode === 'keys') { stopAll(); playSlice(st.sel, null, 2 ** ((k - 12) / 12)); flash(b); return; }
      if (k >= nSlices()) return;
      st.sel = k; stopAll(); playSlice(k); syncKnob();
    };
    b.addEventListener('pointerdown', e => { if (e.button <= 0) press(); });
    b.addEventListener('keydown', e => { if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) { e.preventDefault(); press(); } });
    padBox.append(b); PADBTN.push(b);
  }
  function flash(b) { b.classList.add('on'); setTimeout(() => b.classList.remove('on'), 140); }
  function pads() {
    const n = nSlices();
    PADBTN.forEach((b, i) => {
      if (i < 4) { b.setAttribute('aria-pressed', String((i === 1) === (st.mode === 'keys') && i < 2)); return; }
      const k = i - 4;
      if (st.mode === 'keys') { const black = [1, 3, 6, 8, 10].includes(k % 12); b.className = 'slpad key' + (black ? ' black' : ''); b.textContent = ''; b.disabled = false; b.setAttribute('aria-label', `Key ${k - 12 >= 0 ? '+' : ''}${k - 12}`); return; }
      b.className = 'slpad' + (k === st.sel ? ' sel' : '') + (k === st.lit ? ' on' : '');
      b.disabled = k >= n; b.textContent = k < n ? String(k + 1) : ''; b.setAttribute('aria-label', k < n ? `Slice ${k + 1}` : 'no slice');
    });
  }

  /* ---------- knob (Track Length / Sensitivity / Encoder / Sample Start) ---------- */
  const hint = document.getElementById('slHint');
  const knob = PD.knob(document.getElementById('slKnob'), {
    name: 'Slicer knob',
    max: () => ({ grid: COUNTS.length - 1, beat: 15, keys: nSlices() - 1, cut: 60 }[st.mode]),
    get: () => ({ grid: COUNTS.indexOf(st.count), beat: st.sens - 1, keys: st.sel, cut: (st.cut[st.sel] || 0) + 30 }[st.mode]),
    set: v => {
      if (st.mode === 'grid') { st.count = COUNTS[v]; st.cut = {}; st.sel = Math.min(st.sel, st.count - 1); }
      else if (st.mode === 'beat') { st.sens = v + 1; st.cut = {}; st.sel = Math.min(st.sel, nSlices() - 1); }
      else if (st.mode === 'keys') st.sel = v;
      else { if (st.sel === 0) return; st.cut[st.sel] = v - 30; playSlice(st.sel); }
      render();
    },
    text: () => ({ grid: `Track Length ${st.count} slices`, beat: `Sensitivity ${st.sens}`, keys: `Slice ${st.sel + 1}`, cut: `Sample Start, slice ${st.sel + 1}` }[st.mode]),
  });
  const HINT = {
    grid: 'Turn Track Length: 4 to 32 slices. In View on the device, Micromove stretches the grid (with tempo) and Grid Fit snaps back.',
    beat: 'Turn for more or fewer hits. Lower settings only remove slices, they never move them.',
    keys: 'The encoder chooses the slice. The pads play it across the scale.',
    cut: 'On the device: hold a slice pad and turn Sample Start. Here: pick a pad, then turn. Slice 1 has no start to move.',
  };
  const TEXT = {
    grid: 'Grid splits the sample into equal slices. Each yellow pad plays one slice to its end, like a classic sampler. Steps choose slices: hold a step and tap a pad, or turn the encoder while you hold it.',
    beat: 'Beat listens to the sample and starts a slice at every hit above the red line. Turn up the sensitivity and the line sinks, so quieter hits count too.',
    keys: 'The second green pad switches to Keys: the pads become a keyboard and play the selected slice in tune. With Live Rec on, every hit is recorded as a step with that slice and pitch.',
    cut: 'A slice starts a bit late? Hold its pad and turn Sample Start: only this slice moves, in milliseconds. Let go and nothing moves by accident. To shift a single step instead, hold the step and turn Sample Start.',
  };
  const TITLE = { grid: 'Grid', beat: 'Beat', keys: 'Keys', cut: 'Cut by hand' };
  function syncKnob() { knob.update(); draw(); pads(); }
  function render() {
    document.getElementById('slTitle').textContent = TITLE[st.mode];
    document.getElementById('slText').textContent = TEXT[st.mode];
    hint.textContent = HINT[st.mode];
    syncKnob();
  }
  function setMode(m) {
    st.mode = m;
    document.querySelectorAll('[data-sl]').forEach(t => t.setAttribute('aria-selected', String(t.dataset.sl === m)));
    if (m === 'cut' && st.sel === 0) st.sel = 2;
    stopAll(); render();
  }
  document.querySelectorAll('[data-sl]').forEach(t => t.onclick = () => setMode(t.dataset.sl));

  playButton(document.getElementById('slPlay'), (t0, nodes) => {
    const timers = [];
    for (let i = 0; i < 32; i++) {
      const t = t0 + i * SPB / 2;
      if (st.mode === 'keys') { const m = MEL[i % 16]; if (m != null) timers.push(playSlice(st.sel, t, 2 ** (m / 12), nodes)); }
      else { const k = GROOVE[i % 16] % nSlices(); timers.push(playSlice(k, t, 1, nodes)); }
    }
    return () => timers.forEach(clearTimeout);
  }, () => 16 * SPB);

  render();
  makeLoop();
})();
