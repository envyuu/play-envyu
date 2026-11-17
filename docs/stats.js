'use strict';
/* Visitor counts with GoatCounter (https://www.goatcounter.com): no cookies, no IP addresses stored,
   no personal data. Besides page views it counts patcher steps as events (patched, downloaded,
   USB update ok or failed), so the maintainer sees how the patcher is used and where it breaks.
   Nothing about the visitor's device or files is sent. Localhost visits are not counted. */
(function () {
  const ENDPOINT = 'https://envyu.goatcounter.com/count';
  const queue = [];
  window.goatcounter = window.goatcounter || {};
  window.goatcounter.endpoint = ENDPOINT;
  window.envyuCount = (name, title) => {
    try {
      const gc = window.goatcounter;
      if (gc && gc.count) gc.count({ path: name, title: title || name, event: true });
      else if (queue.length < 20) queue.push([name, title]);
    } catch (_) { /* counting must never break the page */ }
  };
  const s = document.createElement('script');
  s.async = true;
  s.src = 'https://gc.zgo.at/count.js';
  s.dataset.goatcounter = ENDPOINT;
  s.onload = () => { while (queue.length) window.envyuCount(...queue.shift()); };
  document.head.append(s);
})();
