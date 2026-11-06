'use strict';
/* Simulated Play display and knob. Own layout and system fonts, no assets from the firmware.
   One display pixel = 1em inside .pd-screen, so the 320x240 layout scales with the column. */
const PD = (() => {
  const h = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

  /* Shrink a line until it fits, as long names would otherwise be clipped. */
  function fit(node, base) {
    node.style.fontSize = base + 'em';
    if (node.scrollWidth > node.clientWidth && node.scrollWidth > 0) node.style.fontSize = (base * node.clientWidth / node.scrollWidth * .98).toFixed(2) + 'em';
  }
  function screen(host, label) {
    let s = host.querySelector('.pd-screen');
    if (!s) { s = h('div', 'pd-screen'); s.setAttribute('role', 'img'); host.append(s); }
    s.setAttribute('aria-label', 'Simulated display: ' + label);
    s.replaceChildren();
    return s;
  }
  function overlay(s, o = {}) {
    if (o.pill) { const p = h('div', 'pd-pill'); p.append(h('i'), document.createTextNode(o.pill)); s.append(p); }
    if (o.toast) { const t = h('div', 'pd-toast', o.toast); s.append(t); fit(t, 15); }
  }

  /* Parameter view: label, large value and bar. The second knob parameter sits dimmed below. */
  function param(host, { blocks = [], focus = 0, ...o }) {
    const s = screen(host, blocks[focus] ? `${blocks[focus].label}, ${blocks[focus].value}` : (o.pill || o.toast || 'idle'));
    blocks.forEach((b, i) => {
      const box = h('div', 'pd-param' + (i === focus ? ' on' : ''));
      box.style.top = (18 + i * 93) + 'em';
      const v = h('div', 'pd-value', b.value), bar = h('div', 'pd-bar'), fill = h('i');
      fill.style.width = Math.round((b.frac || 0) * 100) + '%';
      bar.append(fill); box.append(h('div', 'pd-label', b.label), v, bar); s.append(box);
      fit(v, 32);
    });
    overlay(s, o);
  }

  /* Menu view: title, rows with values, soft buttons. */
  function menu(host, { title, rows, sel = 0, editing = false, onRow, ...o }) {
    const hadFocus = host.contains(document.activeElement) && document.activeElement.dataset.row;
    const s = screen(host, `${title}, ${rows[sel].label}, ${rows[sel].value}`);
    s.setAttribute('role', 'group');
    const head = h('div', 'pd-title'); head.append(h('span', '', title)); s.append(head);
    const list = h('div', 'pd-rows');
    rows.forEach((r, i) => {
      const b = h('button', 'pd-row' + (i === sel ? ' sel' : '') + (i === sel && editing ? ' edit' : ''));
      b.type = 'button'; b.dataset.row = i;
      const value = h('span', 'pd-rowval', r.value);
      b.append(h('span', '', r.label), value);
      b.onclick = () => onRow && onRow(i);
      list.append(b);
    });
    s.append(list);
    const foot = h('div', 'pd-soft');
    ['Back', 'Play', 'Perform'].forEach(t => foot.append(h('span', '', t)));
    s.append(foot);
    overlay(s, o);
    if (hadFocus) { const again = s.querySelector(`[data-row="${hadFocus}"]`); if (again) again.focus(); }
  }

  /* Perform view: the name of the pad, lit while it is held. */
  function perform(host, { label, value, held, hint }) {
    const s = screen(host, `${label}, ${value}${held ? ', held' : ''}`);
    const box = h('div', 'pd-perform' + (held ? ' on' : ''));
    const v = h('div', 'pd-value', value);
    box.append(h('div', 'pd-label', label), v);
    s.append(box);
    fit(v, 32);
    s.append(h('div', 'pd-hint', hint || ''));
  }

  /* Knob: drag, wheel or arrow keys to turn; tap and double-tap as on the device. */
  function knob(host, { name, max, get, set, text, onTap, onDouble, px = 9 }) {
    const dial = h('div', 'pk');
    dial.tabIndex = 0;
    dial.setAttribute('role', 'slider');
    dial.setAttribute('aria-label', name);
    dial.setAttribute('aria-valuemin', '0');
    dial.append(h('i'));
    host.prepend(dial);
    const lim = () => (typeof max === 'function' ? max() : max);
    const turn = d => { const v = Math.max(0, Math.min(lim(), get() + d)); if (v !== get()) set(v); };
    const pressed = (() => {
      let last = -Infinity;
      return () => {
        const now = performance.now();
        if (onDouble && now - last < 350) { last = -Infinity; onDouble(); return; }
        last = now;
        if (onTap) onTap();
      };
    })();
    let startY = 0, startX = 0, acc = 0, moved = false, active = false;
    dial.addEventListener('pointerdown', e => {
      if (e.button > 0) return;
      active = true; moved = false; acc = 0; startX = e.clientX; startY = e.clientY;
      try { dial.setPointerCapture(e.pointerId); } catch (_) {}
      dial.classList.add('down'); dial.focus({ preventScroll: true });
    });
    dial.addEventListener('pointermove', e => {
      if (!active) return;
      const dist = (startY - e.clientY) + (e.clientX - startX);
      if (Math.abs(dist) > 3) moved = true;
      const steps = Math.trunc((dist - acc) / px);
      if (steps) { acc += steps * px; turn(steps); }
    });
    const end = () => { if (!active) return; active = false; dial.classList.remove('down'); if (!moved) pressed(); };
    dial.addEventListener('pointerup', end);
    dial.addEventListener('pointercancel', () => { active = false; dial.classList.remove('down'); });
    dial.addEventListener('wheel', e => { e.preventDefault(); turn(e.deltaY < 0 ? 1 : -1); }, { passive: false });
    dial.addEventListener('keydown', e => {
      const k = e.key;
      if (k === 'ArrowUp' || k === 'ArrowRight') turn(1);
      else if (k === 'ArrowDown' || k === 'ArrowLeft') turn(-1);
      else if (k === 'Home') turn(-lim());
      else if (k === 'End') turn(lim());
      else if (k === 'Enter' || k === ' ') { if (e.repeat) return; if (onDouble) onDouble(); else if (onTap) onTap(); }
      else return;
      e.preventDefault();
    });
    return {
      update() {
        const m = lim(), v = get();
        dial.setAttribute('aria-valuemax', m);
        dial.setAttribute('aria-valuenow', v);
        dial.setAttribute('aria-valuetext', text());
        dial.style.setProperty('--turn', (m ? -135 + 270 * v / m : -135) + 'deg');
      },
    };
  }
  return { param, menu, perform, knob };
})();
