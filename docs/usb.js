'use strict';
/* Update over USB with WebSerial: the same commands as tools/play_upload.py (PING, PUT, CRC, FLASH).
   Only works when the mod is already installed (the stock firmware has no USB file commands). */
(function () {
  const go = document.getElementById('usbGo'), bar = document.getElementById('usbBar'), fill = bar.firstElementChild;
  const log = document.getElementById('usbLog'), note = document.getElementById('usbNote');
  const VID = 0x16D0, PID = 0x0EA0, BAUD = 115200;   /* never 134 baud: that switches the Teensy core to programming mode */
  let file = null, stage = 'idle', link = null;

  const say = (t, kind) => { const li = document.createElement('li'); li.textContent = t; if (kind) li.className = kind; log.append(li); };
  const setBar = f => { bar.hidden = f == null; if (f != null) fill.style.width = Math.round(f * 100) + '%'; };
  const button = (text, enabled = true) => { go.textContent = text; go.disabled = !enabled; };

  const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
  const crc32 = d => { let c = 0xFFFFFFFF; for (let i = 0; i < d.length; i++) c = CRC[(c ^ d[i]) & 255] ^ (c >>> 8); return ((c ^ 0xFFFFFFFF) >>> 0).toString(16).padStart(8, '0'); };

  async function connect() {
    const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: VID, usbProductId: PID }] });
    await port.open({ baudRate: BAUD });
    const reader = port.readable.getReader(), writer = port.writable.getWriter(), dec = new TextDecoder();
    let buf = '', wake = null, closed = false;
    (async () => {
      try { for (;;) { const { value, done } = await reader.read(); if (done) break; buf += dec.decode(value, { stream: true }); if (wake) wake(); } }
      catch (_) { /* the Play restarts after FLASH */ }
      closed = true; if (wake) wake();
    })();
    async function line(sec) {
      const end = performance.now() + sec * 1000;
      while (!buf.includes('\n')) {
        if (closed) throw new Error('The Play disconnected.');
        const left = end - performance.now();
        if (left <= 0) throw new Error('timeout');
        await new Promise(r => { wake = r; setTimeout(r, Math.min(left, 250)); });
        wake = null;
      }
      const i = buf.indexOf('\n'), l = buf.slice(0, i).trim(); buf = buf.slice(i + 1); return l;
    }
    const send = t => writer.write(new TextEncoder().encode(t + '\n'));
    const cmd = async (t, sec = 3) => { await send(t); return line(sec); };
    async function close() { try { await reader.cancel(); } catch (_) {} try { reader.releaseLock(); writer.releaseLock(); } catch (_) {} try { await port.close(); } catch (_) {} }
    return { cmd, line, writer, close };
  }

  async function upload() {
    log.replaceChildren(); setBar(null);
    button('Connecting…', false);
    try { link = await connect(); }
    catch (e) { button('Connect the Play and update'); if (e.name !== 'NotFoundError') say('Could not open the Play: ' + e.message, 'err'); return; }
    try {
      let r;
      try { r = await link.cmd('PING', 1.5); } catch (_) { r = ''; }
      if (r !== 'OK USBUP 1') throw new Error('The Play does not answer. Is the mod already installed? For the first install, use the SD card.');
      say('Play connected, mod is running');
      const path = '/Firmware/' + file.name, crc = crc32(file.data);
      r = await link.cmd(`PUT ${path} ${file.data.length} ${crc}`, 5);
      if (r !== 'OK READY') throw new Error('The Play refused the file: ' + r);
      button('Uploading…', false); setBar(0);
      const step = 16384;
      for (let o = 0; o < file.data.length; o += step) { await link.writer.write(file.data.subarray(o, o + step)); setBar(Math.min(1, (o + step) / file.data.length)); }
      button('Checking on the card…', false);
      r = await link.line(60 + file.data.length / 200000);
      if (r !== 'OK ' + crc) throw new Error('Upload check failed (' + r + '). Nothing was installed. Please try again.');
      r = await link.cmd(`CRC ${path}`, 60);
      if (r !== `OK ${file.data.length} ${crc}`) throw new Error('The file on the card does not match (' + r + '). Nothing was installed.');
      say(`Uploaded and verified on the card: ${file.name}`);
      stage = 'ready'; button('Install now on the Play');
      note.textContent = 'The Play restarts and installs the update (about 35 seconds). Do not switch it off.';
    } catch (e) { say(e.message, 'err'); await link.close(); link = null; stage = 'idle'; setBar(null); button('Try again'); }
  }

  async function install() {
    button('Starting…', false);
    try {
      const r = await link.cmd('FLASH ' + file.name, 5);
      if (r !== 'OK FLASHING') throw new Error('The Play did not start the update: ' + r);
      say('Installing. The Play restarts by itself – do not switch it off.', 'done');
      say('When it shows the pattern again, switch it off and on once.', 'done');
      button('Done', false); stage = 'done';
    } catch (e) { say(e.message + ' The file stays on the card: you can install it from the update menu.', 'err'); button('Try again'); stage = 'idle'; }
    if (link) { await link.close(); link = null; }
    setBar(null);
  }

  go.onclick = () => { if (stage === 'ready') install(); else if (file) upload(); };
  window.addEventListener('envyu-patched', e => { file = e.detail; stage = 'idle'; log.replaceChildren(); setBar(null); if ('serial' in navigator) button('Connect the Play and update'); });
  window.addEventListener('envyu-reset', () => { file = null; stage = 'idle'; log.replaceChildren(); setBar(null); });
  if (!('serial' in navigator)) {
    button('Not available in this browser', false);
    note.textContent = 'Updating over USB needs Chrome, Edge or Opera on a computer. The SD card way works everywhere.';
  }
})();
