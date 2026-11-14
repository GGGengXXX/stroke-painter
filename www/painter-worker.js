// Runs the painter (WebAssembly) off the main thread so the page stays responsive.
importScripts('painter.js');

const ready = createPainter({ locateFile: (f) => f });
let mod = null;
ready.then((m) => { mod = m; }, () => {});

self.onmessage = async (e) => {
  const { id, rgb, w, h, rounds, detail } = e.data;
  try {
    if (!mod) mod = await ready;
    const ptr = mod._malloc(rgb.length);
    mod.HEAPU8.set(rgb, ptr);
    const outPtr = mod._paint_json(ptr, w, h, rounds, detail);
    try {
      const json = mod.UTF8ToString(outPtr);
      self.postMessage({ id, json });
    } finally {
      mod._free(outPtr);
      mod._free(ptr);
    }
  } catch (err) {
    self.postMessage({ id, error: String(err && err.message || err) });
  }
};
