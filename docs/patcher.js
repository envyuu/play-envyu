'use strict';
/* Browser version of patcher/play_patch.py. The file stays on the visitor's computer. */
(function () {
  const FLASH_CONFIG_START = 0x400, FLASH_CONFIG_END = 0x410;
  const log = document.getElementById('patchLog'), dl = document.getElementById('patchDl');
  const riskL = document.getElementById('riskL'), risk = document.getElementById('risk');
  const input = document.getElementById('ppfFile'), drop = document.getElementById('drop');
  let url = null;

  const say = (t, err) => { const li = document.createElement('li'); li.textContent = t; if (err) li.className = 'err'; log.append(li); };
  const reset = () => { log.replaceChildren(); dl.hidden = true; riskL.hidden = true; risk.checked = false; if (url) URL.revokeObjectURL(url); url = null; };
  const hex2 = b => b.toString(16).toUpperCase().padStart(2, '0');
  async function sha256(bytes) {
    if (!crypto.subtle) throw new Error('Your browser only allows this check on a secure (https) page.');
    const h = await crypto.subtle.digest('SHA-256', bytes);
    return [...new Uint8Array(h)].map(b => b.toString(16).padStart(2, '0')).join('');
  }
  const enc = s => { const a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i); return a; };
  const rec2line = rec => ':' + Array.from(rec, hex2).join('');
  const parse = line => { const hex = line.slice(1), r = new Uint8Array(hex.length / 2); for (let i = 0; i < r.length; i++) r[i] = parseInt(hex.substr(i * 2, 2), 16); return r; };
  const sum8 = (r, n = r.length) => { let s = 0; for (let i = 0; i < n; i++) s += r[i]; return s & 255; };

  function apply(raw, patches, payloadAddr, payload) {
    const text = Array.from({ length: Math.ceil(raw.length / 8192) }, (_, i) => String.fromCharCode.apply(null, raw.subarray(i * 8192, (i + 1) * 8192))).join('');
    const lines = text.split('\n');
    let image = 1, base = 0, lastBase = null;
    for (const line of lines) {
      const body = line.replace(/\r+$/, '');
      if (body[0] !== ':') continue;
      const r = parse(body);
      if (r[3] === 2) base = ((r[4] << 8) | r[5]) << 4;
      else if (r[3] === 4) base = ((r[4] << 8) | r[5]) * 65536;
      else if (r[3] === 0) lastBase = base;
      else if (r[3] === 1) break;
    }
    if (!(lastBase <= payloadAddr && payloadAddr + payload.length <= lastBase + 0x10000)) throw new Error('The mod does not fit the last firmware segment.');
    const appended = [];
    for (let o = 0; o < payload.length; o += 16) {
      const chunk = payload.subarray(o, o + 16), off = payloadAddr + o - lastBase;
      const r = new Uint8Array(chunk.length + 5);
      r[0] = chunk.length; r[1] = off >> 8; r[2] = off & 255; r[3] = 0; r.set(chunk, 4);
      r[r.length - 1] = (256 - sum8(r, r.length - 1)) & 255;
      appended.push(rec2line(r) + '\r');
    }
    const out = [], applied = new Set();
    image = 1; base = 0;
    for (let line of lines) {
      const body = line.replace(/\r+$/, '');
      if (body[0] !== ':') { out.push(line); continue; }
      if (body === ':00000001FF' && image === 1) out.push(...appended);
      const r = parse(body);
      if (sum8(r) !== 0) throw new Error('This file is damaged: bad checksum.');
      const n = r[0], addr = (r[1] << 8) | r[2], typ = r[3];
      if (typ === 2) base = ((r[4] << 8) | r[5]) << 4;
      else if (typ === 4) base = ((r[4] << 8) | r[5]) * 65536;
      let changed = false;
      if (typ === 0 && image === 1) for (let k = 0; k < n; k++) {
        const a = base + addr + k;
        if (patches.has(a)) { r[4 + k] = patches.get(a); applied.add(a); changed = true; }
      }
      if (changed) { r[r.length - 1] = (256 - sum8(r, r.length - 1)) & 255; line = rec2line(r) + line.slice(body.length); }
      out.push(line);
      if (typ === 1) { image += 1; base = 0; }
    }
    if (applied.size !== patches.size) throw new Error('Some patch locations were not found in this file.');
    return enc(out.join('\n'));
  }

  async function run(file) {
    reset();
    try {
      const res = await fetch('manifest.json', { cache: 'no-cache' });
      if (!res.ok) throw new Error('Could not load the mod data from this site.');
      const m = await res.json();
      if (m.format !== 1) throw new Error('Unsupported mod data format.');
      const raw = new Uint8Array(await file.arrayBuffer());
      if (await sha256(raw) !== m.stock_sha256) throw new Error(`This is not the official firmware ${m.firmware_version} file. Nothing was changed. Please download it again from Polyend.`);
      say(`Official firmware ${m.firmware_version} recognised`);
      const patches = new Map();
      for (const p of m.patches) { const a = parseInt(p.addr, 16); for (let k = 0; k * 2 < p.bytes.length; k++) patches.set(a + k, parseInt(p.bytes.substr(k * 2, 2), 16)); }
      const addr = parseInt(m.payload_addr, 16);
      for (const a of patches.keys()) if (a >= FLASH_CONFIG_START && a < FLASH_CONFIG_END) throw new Error('Mod data touches a protected area. Stopping.');
      const payload = enc(atob(m.payload_base64));
      const result = apply(raw, patches, addr, payload);
      say(`Mod ${m.mod_version} applied`);
      if (await sha256(result) !== m.output_sha256) throw new Error('The result does not match the tested build. Nothing to download.');
      say('Result matches the tested build byte for byte');
      url = URL.createObjectURL(new Blob([result], { type: 'application/octet-stream' }));
      dl.href = url; dl.download = `PolyendPlay_v${m.firmware_version}_mod.ppf`;
      riskL.hidden = false;
    } catch (e) { say(e.message, true); }
  }
  risk.onchange = () => { dl.hidden = !risk.checked; };
  input.onchange = () => { if (input.files[0]) run(input.files[0]); };
  ['dragenter', 'dragover'].forEach(t => drop.addEventListener(t, e => { e.preventDefault(); drop.classList.add('over'); }));
  ['dragleave', 'drop'].forEach(t => drop.addEventListener(t, e => { e.preventDefault(); drop.classList.remove('over'); }));
  drop.addEventListener('drop', e => { if (e.dataTransfer.files[0]) run(e.dataTransfer.files[0]); });
})();
