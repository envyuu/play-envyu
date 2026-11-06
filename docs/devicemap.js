'use strict';
/* Interactive schematic of the device. Positions are millimetres measured from the manual's dimension drawing
   (282 x 207 mm, pages 11 and 12), Perform columns come from its quick reference (page 154).
   Controls share their state with the demos further down the page. */
(function deviceMap() {
  const CATS = [
    ['Tuning', '#e5383b'], ['Cutoff', '#ff9f66'], ['Distortion', '#f7e463'], ['Rearrange', '#2fcf5a'],
    ['Repeat', '#bdeea0'], ['Delay', '#c79bea', 'echo'], ['Reverb', '#f08bea', 'cc'], ['Loop', '#ff3d87', 'loop'],
  ];
  const TRACK_MIDI = '#9b6fd6', TRACK_AUDIO = '#3fb56b';
  const FILES = [
    { dir: '/Firmware/', name: 'PolyendPlay_v1.6.0_mod.ppf', text: 'Your patched firmware. Install it from the firmware update menu.' },
    { dir: '/Samples/Live Recordings/', name: 'Rec_001.wav', text: 'USB recordings land here and appear as a sample in your project.' },
    { dir: '/', name: 'usbaudio.cfg', text: 'The saved USB Audio settings: Audio Out, Multitrack 9/10, Track Level, FX Mode and Rec.' },
  ];
  const INFO = {
    pads: { t: 'Pads in Perform mode', d: 'Delay, Reverb and Loop pads get new effects on the MIDI page. Hold one to see its name, or pick tracks with the Select column on the far right.', href: '#perform', link: 'All pads in detail' },
    perform: { t: 'Perform and Audio/MIDI', d: 'Press Perform, then Shift + Audio/MIDI to switch pages. The four right-hand columns show which page you are on, and the last one selects tracks.', href: '#perform', link: 'All pads in detail' },
    chord: { t: 'Chord knob on MIDI tracks', d: 'Double-tap the lit knob to reach the new Arp parameter, then turn it.', href: '#arp', link: 'Hear the arpeggiator' },
    rand: { t: 'Randomize knob', d: 'Double-tap to reach Randomize Type. MIDI tracks get two new types that stay in your project scale.', href: '#scale', link: 'Hear Note In Scale' },
    screen: { t: 'Display and Screen knob', d: 'The main menu gets a USB Audio page. Turn the Screen knob to move, press it to change a value.', href: '#usb', link: 'See the routing' },
    rec: { t: 'Live Rec', d: 'Hold about one second to record incoming USB audio. A short press is still Live Rec, now on release.', href: '#rec', link: 'All recording details' },
    usb: { t: 'USB port', d: 'Audio in and out with your computer, plus file transfer and firmware updates from the command line.', href: '#usb', link: 'See all routing options' },
    sd: { t: 'SD card', d: 'Select a file to see what the mod keeps on the card.', href: '#install', link: 'Install' },
  };
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const M = { spot: 'pads', page: 'midi', col: 10, row: 1, held: false, file: 0, sel: new Set() };
  const FUNC = ['Mute', 'Solo', 'Variation', 'Select'];
  const selText = () => M.sel.size ? (M.sel.size > 1 ? 'Tracks ' : 'Track ') + [...M.sel].sort((a, b) => a - b).map(t => t + 1).join(', ') : 'all MIDI tracks';

  const padInfo = (c, r) => {
    if (c >= 16) return { cat: FUNC[c - 16], value: `Track ${r + 1}`, track: true, func: FUNC[c - 16] };
    const [cat, , kind] = CATS[c >> 1], i = r + (c & 1) * 8;
    if (M.page !== 'midi' || !kind) return { cat, value: 'Original', changed: false };
    const names = kind === 'echo' ? ECHO.map(e => e[5]) : kind === 'cc' ? LFO.map(e => e[5]) : [...LOOPNAME, ...LOOPNAME.map(n => n + ' LR')];
    return { cat, value: names[i], kind, i, changed: true };
  };
  function chartFor(kind, i) {
    if (kind === 'echo') { const n = echoNotes(ECHO[i]); return roll(n, Math.max(192, n[n.length - 1].t + 36), { label: `Echoes of ${ECHO[i][5]}`, axis: 'White: your note. Amber: echoes.' }); }
    if (kind === 'loop') { const r = loopNotes(i); return roll(r.notes, 384, { mark: r.holdAt, markLabel: 'pad held', label: 'Loop timeline', axis: 'Grey: silent while you hold. Amber: the loop.' }); }
    return lfoChart(LFO[i]);
  }

  /* ---------- Schematic ---------- */
  const device = $('device'), disp = el('div', { className: 'pd', inert: true }), zoom = el('div', { className: 'pd', id: 'mapDisp' });
  const place = (node, x, y, w, h) => { Object.entries({ x, y, w, h }).forEach(([k, v]) => node.style.setProperty('--' + k, v)); return node; };
  const select = spot => { if (M.spot !== spot) { M.spot = spot; render(); } };
  const hw = (label, spot, onPress) => {
    const b = el('button', { type: 'button', className: 'hwb' + (spot ? ' live' : ''), textContent: label });
    if (!spot) { b.disabled = true; return b; }
    b.dataset.spot = spot;
    b.addEventListener('click', () => { select(spot); if (onPress) onPress(); });
    return b;
  };
  const liveKnob = (label, spot, cfg) => {
    const box = el('div', { className: 'knobbox live' }, el('span', { textContent: label }));
    box.dataset.spot = spot;
    box.addEventListener('pointerdown', () => select(spot), true);
    box.addEventListener('focusin', () => select(spot));
    return { box, knob: PD.knob(box, cfg) };
  };
  const dead = label => el('div', { className: 'knobbox' }, el('span', { className: 'deadknob' }), el('span', { textContent: label }));
  const chord = liveKnob('Chord', 'chord', arpKnobCfg);
  const rand = liveKnob('Randomize', 'rand', nisKnobCfg);
  const scr = liveKnob('Screen', 'screen', usbKnobCfg);

  const flash = () => device.querySelectorAll('.flip').forEach(b => { b.classList.add('flash'); setTimeout(() => b.classList.remove('flash'), 350); });
  const togglePage = () => { M.page = M.page === 'midi' ? 'audio' : 'midi'; flash(); render(); };
  const audioMidi = hw('Audio/ MIDI', 'perform', togglePage); audioMidi.classList.add('flip');
  const shift = hw('Shift', 'perform', togglePage); shift.classList.add('flip');
  const liveRec = hw('Live Rec', 'rec');
  const performBtn = hw('Perform', 'perform');
  const holdStart = e => { if (e.button > 0) return; select('rec'); try { liveRec.setPointerCapture(e.pointerId); } catch (_) {} liveRec.classList.add('down'); recCtl.press(); drawLive(); };
  const holdEnd = () => { if (liveRec.classList.contains('down')) { liveRec.classList.remove('down'); recCtl.release(); drawLive(); } };
  liveRec.addEventListener('pointerdown', holdStart);
  ['pointerup', 'pointercancel'].forEach(t => liveRec.addEventListener(t, holdEnd));
  liveRec.addEventListener('keydown', e => { if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) { e.preventDefault(); holdStart({ button: 0 }); } });
  liveRec.addEventListener('keyup', e => { if (e.key === ' ' || e.key === 'Enter') holdEnd(); });

  const face = el('div', { className: 'dev-face' });
  const dispWrap = el('div', { className: 'dev-disp' }, disp);
  dispWrap.addEventListener('pointerdown', () => { if (!['screen', 'usb'].includes(M.spot)) select('screen'); });
  face.append(place(dispWrap, 31.1, 18.1, 59.2, 44.8), place(hw('', null), 10.8, 20.2, 15.2, 15.2), place(hw('', null), 10.8, 45.1, 15.2, 15.2), place(scr.box, 11.9, 67.9, 13.4, 13.4));
  [[liveRec, 32.9], [hw('Play', null), 52.7], [performBtn, 72.6]].forEach(([b, x]) => face.append(place(b, x, 66.8, 15.2, 15.2)));
  const KNOB_X = [102.2, 128.5, 154.9, 181.3, 207.6], KNOB_Y = [20.2, 44.4, 69];
  [['Master Volume', 'Note', 'Sample', 'Reverb Send', 'Chance'], ['Tempo', 'Volume', chord.box, 'Overdrive', rand.box], ['Track Length', 'Filter Cutoff', 'Sample Attack', 'Repeat Type', 'Move']]
    .forEach((row, r) => row.forEach((k, c) => face.append(place(typeof k === 'string' ? dead(k) : k, KNOB_X[c], KNOB_Y[r], 11.4, 11.4))));
  [[hw('Fill', null), audioMidi], [hw('Save', null), hw('Copy', null)], [hw('View', null), shift]]
    .forEach((row, r) => row.forEach((b, c) => face.append(place(b, [230.4, 256.7][c], [18.1, 42.6, 66.8][r], 15.4, 15.4))));

  const pads = el('div', { className: 'dev-pads', role: 'group', ariaLabel: 'Pads: hold one to see its effect' });
  for (let r = 0; r < 8; r++) for (let c = 0; c < 20; c++) {
    const b = el('button', { type: 'button', className: 'mp', tabIndex: -1 });
    b.dataset.c = c; b.dataset.r = r;
    pads.append(b);
  }
  const padAt = (c, r) => pads.children[r * 20 + c];
  const press = (c, r) => {
    M.col = c; M.row = r; M.held = true;
    if (c === 19) { if (M.sel.has(r)) M.sel.delete(r); else M.sel.add(r); }
    if (M.spot !== 'pads') { M.spot = 'pads'; render(); } else { drawPads(); drawDisplay(); drawLive(); }
  };
  const letGo = () => { if (M.held) { M.held = false; drawPads(); drawDisplay(); } };
  pads.addEventListener('pointerdown', e => { const b = e.target.closest('.mp'); if (b && e.button <= 0) press(+b.dataset.c, +b.dataset.r); });
  pads.addEventListener('keydown', e => {
    const b = e.target.closest('.mp'); if (!b) return;
    let c = +b.dataset.c, r = +b.dataset.r;
    if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); if (!e.repeat) press(c, r); return; }
    if (e.key === 'ArrowLeft') c = Math.max(0, c - 1); else if (e.key === 'ArrowRight') c = Math.min(19, c + 1);
    else if (e.key === 'ArrowUp') r = Math.max(0, r - 1); else if (e.key === 'ArrowDown') r = Math.min(7, r + 1); else return;
    e.preventDefault(); M.col = c; M.row = r; drawPads(); padAt(c, r).focus();
  });
  ['pointerup', 'pointercancel', 'blur'].forEach(t => window.addEventListener(t, letGo));
  window.addEventListener('keyup', e => { if (e.key === ' ' || e.key === 'Enter') letGo(); });
  face.append(place(pads, 13, 93, 257.1, 101.8));
  /* Seen from behind, as in the manual: the knob stubs are mirrored and the side is 35 mm tall including them. */
  const back = el('div', { className: 'dev-back' }, place(el('span', { className: 'dev-backcap', textContent: 'From behind' }), 0, 0, 60, 13));
  [70, 96.5, 123, 149, 175, 266].forEach(x => back.append(place(el('i', { className: 'dev-stub' }), x, 0, 11, 16)));
  back.append(place(el('div', { className: 'dev-backbody' }), 0, 15, 282, 20));
  [['Out', 22.4], ['MIDI Out', 37.6], ['MIDI In', 53.3], ['Reset', 225], ['Power', 262]].forEach(([t, x]) => back.append(place(el('span', { className: 'dev-port' + (t === 'Reset' ? ' small' : ''), textContent: t }), x - 8, 18, 16, 15)));
  back.append(place(hw('Micro SD', 'sd'), 96, 19, 30, 12), place(hw('USB', 'usb'), 234, 19, 18, 12));
  device.append(face, back);

  /* ---------- Drawing ---------- */
  const perform = () => M.spot === 'pads' || M.spot === 'perform';
  function drawPads() {
    const on = perform();
    for (const b of pads.children) {
      const c = +b.dataset.c, r = +b.dataset.r, track = c >= 16;
      const cat = track ? null : CATS[c >> 1];
      const changed = !track && M.page === 'midi' && cat[2];
      b.style.setProperty('--pad', !on ? '' : track ? (M.page === 'midi' ? TRACK_MIDI : TRACK_AUDIO) : cat[1]);
      b.classList.toggle('lit', on);
      b.classList.toggle('faded', on && (track ? c !== 19 && M.page === 'midi' : M.page === 'midi' && !changed));
      b.classList.toggle('picked', on && c === 19 && M.sel.has(r));
      b.classList.toggle('sel', on && c === M.col && r === M.row);
      b.classList.toggle('held', on && M.held && c === M.col && r === M.row);
      b.tabIndex = c === M.col && r === M.row ? 0 : -1;
      const p = padInfo(c, r);
      b.setAttribute('aria-label', `${p.cat}: ${p.value}`);
    }
  }
  function paint(host) {
    const s = M.spot;
    if (s === 'pads' || s === 'perform') {
      const p = padInfo(M.col, M.row);
      PD.perform(host, { label: p.cat, value: p.value, held: M.held, hint: M.held ? '' : `${M.page === 'midi' ? 'MIDI' : 'Audio'} page · hold a pad` });
    } else if (s === 'chord') PD.param(host, arpScene());
    else if (s === 'rand') PD.param(host, nisScene());
    else if (s === 'screen' || s === 'usb') PD.menu(host, usbScene());
    else if (s === 'rec') PD.param(host, recCtl.view());
    else PD.param(host, { toast: FILES[M.file].name });
  }
  /* The display is small at true scale, so the panel shows it enlarged; only the enlarged one takes input. */
  const drawDisplay = () => { paint(disp); paint(zoom); };
  const live = el('div', { className: 'maplive' });
  function drawLive() {
    const s = M.spot;
    if (s === 'pads') {
      const p = padInfo(M.col, M.row);
      if (p.changed) live.replaceChildren(el('p', { className: 'maphead', textContent: `${p.cat} pad → ${p.value}` }), el('div', { className: 'chart' }, chartFor(p.kind, p.i)), el('p', { className: 'desc', textContent: `Applies to: ${selText()}.` }));
      else live.replaceChildren(el('p', { className: 'maphead', textContent: `${p.cat}: ${p.value}` }), el('p', { className: 'desc', textContent: p.func === 'Select' ? `The new effects now apply to: ${selText()}.` : p.track ? 'Unchanged by the mod.' : M.page === 'midi' ? 'This column works as in the original firmware.' : 'The Audio page keeps the original effects. Switch to the MIDI page to see the new ones.' }));
    } else if (s === 'perform') {
      const b = el('button', { type: 'button', className: 'btn small', textContent: `Shift + Audio/MIDI: go to ${M.page === 'midi' ? 'Audio' : 'MIDI'} page` });
      b.onclick = () => { togglePage(); };
      const legend = el('ul', { className: 'legend' });
      CATS.forEach(([n, col, kind]) => { const li = el('li', { textContent: n + (kind && M.page === 'midi' ? ' · new' : '') }); li.style.setProperty('--dot', col); if (kind && M.page === 'midi') li.className = 'new'; legend.append(li); });
      live.replaceChildren(el('p', { className: 'maphead', textContent: `Now: ${M.page === 'midi' ? 'MIDI' : 'Audio'} page` }), b, legend);
    } else if (s === 'chord') {
      const r = arpNotes();
      live.replaceChildren(el('p', { className: 'maphead', textContent: arpUi.focus ? `Arp → ${arpValue()}` : `Chord → ${CHORD_SHORT($('arpChord').selectedOptions[0].textContent)}` }), el('div', { className: 'chart' }, roll(r.out, r.total, { label: `Arp pattern ${r.a[0]}`, axis: arpUi.focus ? 'Turn the knob to change the pattern.' : 'Double-tap the knob to reach Arp.' })));
    } else if (s === 'rand') {
      const r = nisNotes();
      live.replaceChildren(el('p', { className: 'maphead', textContent: `${$('nisType').selectedOptions[0].textContent} · ${$('nisPct').value}%` }), el('div', { className: 'chart' }, roll(r.notes, 384, { label: 'Randomized pattern', axis: 'White: unchanged. Amber: new note in scale. Red: outside.' })));
    } else if (s === 'screen') {
      const r = usbUi.rows[usbUi.sel];
      live.replaceChildren(el('p', { className: 'maphead', textContent: `${r.label} → ${usbName(r)}` }), el('p', { className: 'desc', textContent: $('usbHint').textContent }));
    } else if (s === 'usb') {
      const seg = el('div', { className: 'seg', role: 'group', ariaLabel: 'Audio Out' });
      [...$('usbOut').options].forEach(o => { const b = el('button', { type: 'button', textContent: o.textContent }); b.setAttribute('aria-pressed', String(o.selected)); b.onclick = () => { $('usbOut').value = o.value; renderUsb(); }; seg.append(b); });
      live.replaceChildren(seg, $('usbFlow').cloneNode(true), el('p', { className: 'desc', textContent: $('usbText').textContent }));
      live.querySelector('.flow').removeAttribute('id');
    } else if (s === 'rec') {
      const bar = el('div', { className: 'holdbar' + (liveRec.classList.contains('down') ? ' run' : '') }, el('i'));
      live.replaceChildren(el('p', { className: 'maphead', textContent: 'Hold the lit Live Rec button' }), bar, el('p', { className: 'desc', textContent: $('usbOut').value === 'in' ? 'Audio Out is Stereo + In, so a long hold starts a USB recording.' : 'A long hold asks for Audio Out = Stereo + In. Change it under USB port.' }));
    } else {
      const tree = el('div', { className: 'files' });
      FILES.forEach((f, i) => { const b = el('button', { type: 'button' }, el('span', { textContent: f.dir }), f.name); b.setAttribute('aria-pressed', String(i === M.file)); b.onclick = () => { M.file = i; drawDisplay(); drawLive(); }; tree.append(b); });
      live.replaceChildren(tree, el('p', { className: 'desc', textContent: FILES[M.file].text }));
    }
  }
  const panel = $('mapInfo');
  const zoomFig = el('figure', { className: 'stage mapzoom' }, zoom, el('figcaption', { textContent: 'Display, enlarged · simulated' }));
  function render() {
    const s = INFO[M.spot];
    device.querySelectorAll('[data-spot]').forEach(b => {
      const on = b.dataset.spot === M.spot || (M.spot === 'pads' && b === performBtn);
      if (b.tagName === 'BUTTON') b.setAttribute('aria-pressed', String(on));
      b.classList.toggle('active', on);
    });
    device.dataset.spot = M.spot;
    panel.replaceChildren(el('h3', { textContent: s.t }), zoomFig, el('p', { className: 'mapdesc', textContent: s.d }), live, el('a', { href: s.href, textContent: s.link + ' →' }));
    drawPads(); drawDisplay(); drawLive(); knobs();
    cards.forEach(c => c.setAttribute('aria-pressed', String(c.dataset.spot === M.spot || (c.dataset.spot === 'perform' && M.spot === 'pads'))));
  }
  const knobs = () => { chord.knob.update(); rand.knob.update(); scr.knob.update(); };

  /* ---------- Preview cards ---------- */
  const miniPads = el('div', { className: 'minipads', ariaHidden: 'true' });
  for (let r = 0; r < 4; r++) for (let c = 0; c < 16; c++) { const i = el('i'); i.style.background = CATS[c >> 1][1]; i.dataset.c = c >> 1; miniPads.append(i); }
  const perfName = el('b', { className: 'cardval' });
  const recPill = el('span', { className: 'cardpill', ariaHidden: 'true' }, el('i'), el('span', { textContent: 'USB REC 0:00' }));
  const usbVal = el('b', { className: 'cardval' });
  const usbAnim = el('div', { className: 'cardflow', ariaHidden: 'true' }, el('span', { textContent: 'Computer' }), el('i'), el('span', { textContent: 'Play' }));
  const sdList = el('div', { className: 'cardfiles', ariaHidden: 'true' }, ...['.ppf', 'Rec_001.wav', 'usbaudio.cfg'].map(n => el('span', { textContent: n })));
  const card = (spot, title, text, ...preview) => {
    const b = el('button', { type: 'button', className: 'mapcard' }, el('strong', { textContent: title }), el('div', { className: 'cardprev' }, ...preview), el('small', { textContent: text }));
    b.dataset.spot = spot;
    b.onclick = () => {
      select(spot);
      /* On narrow screens the cards sit below the result, so bring it into view. */
      if (panel.getBoundingClientRect().top < 60) panel.scrollIntoView({ block: 'start', behavior: still ? 'auto' : 'smooth' });
    };
    return b;
  };
  const cards = [
    card('rec', 'Live Rec', 'Hold to record USB audio', recPill),
    card('perform', 'Perform', 'New effects on the MIDI page', miniPads, perfName),
    card('usb', 'USB', 'Audio both ways, file transfer', usbAnim, usbVal),
    card('sd', 'SD card', 'Recordings, settings, firmware', sdList),
  ];
  $('mapCards').append(...cards);

  /* Cards animate only while visible, and not at all with reduced motion. */
  const showcase = [[5, 'Echo 1/8'], [6, 'Wobble 1/4'], [7, '4 Steps LR'], [5, 'Oct PingPong'], [6, 'S&H 1/8'], [7, '1/4 Step']];
  let frame = 0, visible = true;
  function animate() {
    const [cat, name] = showcase[frame % showcase.length];
    perfName.textContent = name;
    miniPads.querySelectorAll('i').forEach(i => { const c = +i.dataset.c; i.style.opacity = c === cat ? 1 : c >= 5 ? .55 : .25; });
    recPill.lastChild.textContent = `USB REC 0:${String(frame % 60).padStart(2, '0')}`;
    usbVal.textContent = $('usbOut').selectedOptions[0].textContent;
    usbAnim.dataset.mode = $('usbOut').value;
    [...sdList.children].forEach((s, i) => s.classList.toggle('on', i === frame % 3));
  }
  animate();
  if (!still) {
    if ('IntersectionObserver' in window) new IntersectionObserver(e => { visible = e[0].isIntersecting; }).observe($('mapCards'));
    setInterval(() => { if (visible && !document.hidden) { frame += 1; animate(); } }, 1000);
  }

  mapSync = () => {
    knobs(); usbVal.textContent = $('usbOut').selectedOptions[0].textContent; usbAnim.dataset.mode = $('usbOut').value;
    if (M.spot !== 'pads' && M.spot !== 'perform' && M.spot !== 'sd') { drawDisplay(); drawLive(); }
  };
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', drawLive);
  render();
})();
