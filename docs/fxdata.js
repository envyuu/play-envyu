'use strict';
/* Tables and note rules shared by the guide and the video page. They mirror fw/fx.c, fw/arp.c and fw/nis.c.
   Timing is in ticks: 1/16 = 24. */
const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = n => NOTE[n % 12] + (Math.floor(n / 12) - 1);
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
/* ---------- Echo (fx.c CFG, plan_echo) ---------- */
const ECHO = [[24,3,70,0,0,'Echo 1/16'],[48,3,65,0,0,'Echo 1/8'],[72,3,60,0,0,'Echo 3/16'],[96,3,60,0,0,'Echo 1/4'],[32,4,65,0,0,'Echo 1/8T'],[16,4,70,0,0,'Echo 1/16T'],[12,6,85,0,0,'Roll 1/32'],[144,2,55,0,0,'Echo 3/8'],[48,3,70,12,0,'Oct Up'],[48,3,70,-12,0,'Oct Down'],[72,4,70,7,0,'Fifths Up'],[24,4,80,12,1,'Oct PingPong'],[24,6,85,1,0,'Chroma Up'],[24,6,85,-1,0,'Chroma Down'],[48,4,125,0,0,'Swell 1/8'],[24,8,80,5,0,'Fourths Arp']];
function echoNotes(c) {
  const [delay, reps, pct, pitch, alt] = c; const gate = Math.max(6, Math.floor(delay / 2));
  const out = [{ t: 0, dur: 24, note: 60, vel: 100, kind: 'src' }];
  let v = 100, note = 60, step = pitch;
  for (let r = 1; r <= reps; r++) {
    v = clamp(Math.trunc(v * pct / 100), 1, 127); note += step; if (alt) step = -step;
    if (note < 0 || note > 127) break;
    out.push({ t: r * delay, dur: gate, note, vel: v });
  }
  return out;
}
/* ---------- Loop (fx.c LOOP_LEN, loop_snapshot, loop_capture) ---------- */
const LOOPLEN = [96, 72, 48, 24, 12, 8, 6, 3];
const LOOPNAME = ['4 Steps', '3 Steps', '2 Steps', '1 Step', '1/2 Step', '1/3 Step', '1/4 Step', '1/8 Step'];
const SRC = [60, 67, 72, 63, 65, 70, 74, 62, 60, 67, 72, 63, 65, 70, 74, 62];
function loopNotes(pad) {
  const holdAt = 8 * 24 + 4, len = LOOPLEN[pad & 7], rev = pad >= 8;
  const srcAll = SRC.map((note, i) => ({ t: i * 24, dur: 12, note, vel: 100 }));
  const before = srcAll.filter(n => n.t < holdAt).map(n => ({ ...n, kind: 'src' }));
  const muted = srcAll.filter(n => n.t >= holdAt).map(n => ({ ...n, kind: 'dim' }));
  const win = srcAll.filter(n => n.t < holdAt && holdAt - n.t <= 96 && n.t - (holdAt - 96) >= 96 - len);
  const cap = win.map(n => { const rel = n.t - (holdAt - len); let dur = n.dur; if (dur >= len) dur = len > 2 ? Math.floor(len / 2) : 1; return { rel: rev ? len - 1 - rel : rel, dur, note: n.note, vel: n.vel }; });
  const out = [];
  for (let k = 0, s = holdAt; s < 16 * 24; k++, s += len) cap.forEach(c => { if (s + c.rel < 16 * 24) out.push({ t: s + c.rel, dur: c.dur, note: c.note, vel: c.vel }); });
  return { holdAt, notes: [...before, ...muted, ...out], out, cap, len, rev };
}
/* ---------- CC Motion (fx.c LFO) ---------- */
const CCN = { 74: 'Filter cutoff (CC74)', 1: 'Mod wheel (CC1)', 10: 'Pan (CC10)', 71: 'Resonance (CC71)', 7: 'Volume (CC7)' };
const LFO = [[74,'tri',24,20,120,'Wobble 1/16'],[74,'tri',48,20,120,'Wobble 1/8'],[74,'tri',96,10,127,'Wobble 1/4'],[74,'tri',192,10,127,'Wobble 1/2'],[74,'up',384,0,127,'Sweep Up'],[74,'dn',384,0,127,'Sweep Down'],[74,'sh',24,10,127,'S&H 1/16'],[74,'sh',48,10,127,'S&H 1/8'],[1,'tri',96,0,127,'ModWheel LFO'],[1,'once',768,0,127,'ModWheel Swell'],[10,'sq',48,0,127,'Pan PingPong'],[10,'tri',384,0,127,'Auto Pan'],[71,'tri',96,10,110,'Reso Wobble'],[74,'sq',12,15,127,'Filter Gate'],[7,'tri',48,40,127,'Tremolo 1/8'],[71,'sh',48,10,110,'Reso S&H']];
const PER = { 12: '1/32 cycle', 24: '1/16', 48: '1/8', 96: '1/4', 192: '1/2', 384: '1 bar', 768: '2 bars' };
function lfoValue(c, t, rnd) {
  const [, sh, per, lo, hi] = c, span = hi - lo, ph = t % per;
  switch (sh) {
    case 'tri': { const h = per / 2, x = ph < h ? ph : per - ph; return lo + span * x / h; }
    case 'up': return lo + span * ph / per;
    case 'dn': return hi - span * ph / per;
    case 'sq': return ph < per / 2 ? hi : lo;
    case 'once': return lo + span * Math.min(t, per) / per;
    default: return lo + rnd[Math.floor(t / per) % rnd.length] * span;
  }
}
const RND = Array.from({ length: 64 }, (_, i) => { const x = Math.sin(i * 12.9898 + 4.1) * 43758.5453; return x - Math.floor(x); });
const ARP = [['Off','off',24,1],['Up 1/16','up',24,1],['Down 1/16','dn',24,1],['UpDown 1/16','ud',24,1],['Random 1/16','rnd',24,1],['Up 1/32','up',12,1],['Down 1/32','dn',12,1],['UpDown 1/32','ud',12,1],['Random 1/32','rnd',12,1],['Up 1/8','up',48,1],['Down 1/8','dn',48,1],['UpDown 1/8','ud',48,1],['Random 1/8','rnd',48,1],['Up 1/16T','up',16,1],['Down 1/16T','dn',16,1],['Up 2Oct','up',24,2],['UpDown 2Oct','ud',24,2],['Converge','conv',24,1],['Diverge','div',24,1],['Pinky','pink',24,1],['Strum','strum',3,1],['Up 1/4','up',96,1],['Down 1/4','dn',96,1],['UpDown 1/4','ud',96,1],['Random 1/4','rnd',96,1],['Up 1/2','up',192,1],['Down 1/2','dn',192,1],['UpDown 1/2','ud',192,1],['Random 1/2','rnd',192,1]];
/* Names as on the device (chord list in the firmware); None plays root + octave in the arp. */
const CHORDS = [['None (root + octave)',[0,12]],['Major',[0,4,7]],['Minor',[0,3,7]],['Maj 7th',[0,4,7,11]],['Min 7th',[0,3,7,10]],['Sus4',[0,5,7]]];
const SCALES = [['Major',[0,2,4,5,7,9,11]],['Minor',[0,2,3,5,7,8,10]],['Dorian',[0,2,3,5,7,9,10]],['Lydian',[0,2,4,6,7,9,11]],['Mixolydian',[0,2,4,5,7,9,10]],['Phrygian',[0,1,3,5,7,8,10]],['Harm Minor',[0,2,3,5,7,8,11]],['Penta Min',[0,3,5,7,10]],['Penta Maj',[0,2,4,7,9]],['Blues Min',[0,3,5,6,7,10]]];
const BASE = [60, 60, 63, 60, 67, 60, 65, 63, 60, 60, 63, 60, 70, 67, 65, 63];
function mulberry(a) { return () => { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; }
function arpPattern(mode, chord, addOct) {
  const a = ARP[mode], ch = CHORDS[chord][1], octs = a[3] + (addOct ? 1 : 0);
  const set = []; for (let o = 0; o < octs && o < 3; o++) for (const b of ch) { const n = 60 + b + 12 * o; if (!set.includes(n)) set.push(n); }
  const N = set.length; const out = []; const rate = a[2]; const gate = Math.max(3, Math.floor(rate * 3 / 4));
  const total = 192;
  if (a[1] === 'off') { const plain = chord === 0 ? [60] : ch.map(b => 60 + b); return { out: plain.map(n => ({ t: 0, dur: 96, note: n, vel: 100 })), total, a, set: plain }; }
  if (a[1] === 'strum') { set.forEach((n, i) => out.push({ t: i * rate, dur: 96 - i * rate, note: n, vel: 100 })); return { out, total, a, set }; }
  for (let p = 0; p * rate < Math.max(total, rate * 4); p++) {
    let r = 0;
    switch (a[1]) {
      case 'up': r = p % N; break;
      case 'dn': r = N - 1 - (p % N); break;
      case 'ud': { const per = N > 1 ? 2 * N - 2 : 1, q = p % per; r = q < N ? q : per - q; break; }
      case 'rnd': r = Math.floor(RND[p % 64] * N); break;
      case 'conv': { const q = p % N; r = q & 1 ? N - 1 - (q >> 1) : q >> 1; break; }
      case 'div': { const q = p % N, mid = (N - 1) >> 1; r = q & 1 ? mid + ((q + 1) >> 1) : mid - (q >> 1); if (r >= N) r = N - 1; if (r < 0) r = 0; break; }
      case 'pink': { const q = p % (2 * Math.max(N - 1, 1)); r = q & 1 ? N - 1 : q >> 1; if (r >= N) r = N - 1; break; }
    }
    out.push({ t: p * rate, dur: gate, note: set[r], vel: 100 });
  }
  return { out, total: Math.max(total, rate * 4), a, set };
}
function nisPattern(scale, p, plus, seed) {
  const r = mulberry(seed), sc = SCALES[scale][1];
  const inScale = n => sc.includes(((n % 12) + 12) % 12);
  let changed = 0, outside = 0;
  const notes = BASE.map((orig, i) => {
    const pickChange = r(), pickOut = r(), pickNote = r();
    if (pickChange * 100 >= p) return { t: i * 24, dur: 18, note: orig, vel: 100, kind: 'src' };
    changed++;
    const wantOut = plus && p > 50 && pickOut * 100 < (p - 50) / 2;
    const cand = []; for (let n = Math.max(12, orig - 12); n <= Math.min(119, orig + 12); n++) if (n !== orig && (wantOut ? !inScale(n) : inScale(n))) cand.push(n);
    const note = cand[Math.floor(pickNote * cand.length)];
    if (wantOut) outside++;
    return { t: i * 24, dur: 18, note, vel: 100, kind: wantOut ? 'out' : undefined };
  });
  return { notes, changed, outside };
}
