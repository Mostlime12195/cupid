// Cupid player.
//
// Standalone:  player.html?game=path/to/game.love   (or drop a .love on the page)
//              Shows the splash while it loads (splash.png in the .love),
//              then starts the game by itself.
// Embedded:    <iframe src="player.html?embed=1">, then postMessage
//              {type: 'run', files: {'main.lua': '...', 'img/a.png': Uint8Array}}
//              or {type: 'run', love: Uint8Array}.
//
// Messages sent to the embedding page all carry source: 'love-player':
//   ready, started, log {level, text}, error {text}, quit {code}, title {title}
// build.py replaces this with a hash of the engine files, so browsers never
// combine a cached love.wasm with a newer love.mjs (or vice versa).
const BUILD = '__LOVE_BUILD__';
const { default: createLove } = await import(`./love.mjs?v=${BUILD}`);

const SAVE_ROOT = '/home/web_user/.local/share/love';
const GAME_DIR = '/game';
const GAME_FILE = '/game.love';

const params = new URLSearchParams(location.search);
const embedded = params.get('embed') === '1';
const scaleMode = params.get('scale') || document.querySelector('meta[name="love-scale"]')?.content || 'fit';
// ?jit=0 runs all Lua in the interpreter (for comparison or debugging).
const jitEnabled = params.get('jit') !== '0';
const gameUrl = params.get('game') || document.querySelector('meta[name="love-game"]')?.content || null;

const $ = (sel) => document.querySelector(sel);
const canvas = $('#canvas');
const overlay = $('#overlay');
const statusEl = $('#overlay-status');
const progressEl = $('#overlay-progress');
const playButton = $('#play-button');
const filePicker = $('#file-picker');

const splashImage = $('#splash-image');
const splashMark = $('#splash-mark');
const splashCaption = $('#splash-caption');

// Games start without a click, so browsers hold their sound back until the
// first one: keep track of the engine's audio and let it play then.
const audioContexts = new Set();
for (const name of ['AudioContext', 'webkitAudioContext']) {
  const Base = window[name];
  if (!Base) continue;
  window[name] = class extends Base {
    constructor(...args) {
      super(...args);
      audioContexts.add(this);
    }
  };
}
for (const type of ['pointerdown', 'keydown', 'touchend']) {
  window.addEventListener(type, () => {
    for (const ctx of audioContexts) if (ctx.state === 'suspended') ctx.resume().catch(() => {});
  }, { capture: true });
}

document.body.classList.toggle('embed', embedded);
document.body.classList.toggle('pixelated', params.get('pixelated') === '1' || scaleMode === 'integer');

// ---------------------------------------------------------------------------
// Messaging

function post(msg) {
  if (embedded && window.parent !== window)
    window.parent.postMessage({ source: 'love-player', ...msg }, '*');
}

let errorLines = null;
let errorTimer = 0;

function flushError() {
  if (errorLines) {
    post({ type: 'error', text: errorLines.join('\n') });
    errorLines = null;
  }
}

// LÖVE's error handler prints "Error: ..." followed by a traceback. Group
// those lines so the embedding page gets the whole error at once.
function output(level, text) {
  post({ type: 'log', level, text });
  (level === 'err' ? console.error : console.log)(text);

  if (text.startsWith('Error: ')) {
    flushError();
    errorLines = [text.slice(7)];
  } else if (errorLines && (text.startsWith('\t') || text.startsWith('stack traceback'))) {
    errorLines.push(text);
  } else {
    flushError();
  }
  clearTimeout(errorTimer);
  errorTimer = setTimeout(flushError, 50);
}

// ---------------------------------------------------------------------------
// Overlay UI

function showStatus(text, { progress = null, error = false } = {}) {
  overlay.hidden = false;
  overlay.classList.remove('leaving');
  statusEl.textContent = text;
  statusEl.classList.toggle('error', error);
  const bar = progressEl.parentElement;
  bar.hidden = progress === false;
  progressEl.classList.toggle('indeterminate', progress === null);
  progressEl.style.width = typeof progress === 'number' ? `${Math.round(progress * 100)}%` : '';
}

// The splash fades into the game.
function hideOverlay() {
  overlay.classList.add('leaving');
  setTimeout(() => {
    if (overlay.classList.contains('leaving')) overlay.hidden = true;
  }, 450);
}

// Errors and the end of the game take the splash's place.
function showMessage(text, options) {
  overlay.classList.add('message');
  showStatus(text, options);
}

function fail(text) {
  showMessage(text, { progress: false, error: true });
  playButton.hidden = true;
  post({ type: 'error', text });
}

// ---------------------------------------------------------------------------
// Canvas fitting. SDL sizes the canvas to the LÖVE window; we scale it to fit
// the page while keeping its aspect ratio.

function fitCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const sw = window.innerWidth;
  const sh = window.innerHeight;

  // Resizable games get the whole page, like a maximized desktop window
  // (they receive love.resize). Fixed-size games are scaled to fit.
  if (running && Module?._loveweb_fit_window?.(sw, sh)) {
    canvas.style.setProperty('--fit-w', `${sw}px`);
    canvas.style.setProperty('--fit-h', `${sh}px`);
    return;
  }

  // SDL keeps the backing store at pixel size; the logical size is what the
  // game asked for, which SDL writes as the element's inline CSS size.
  const w = parseFloat(canvas.style.width) || canvas.width;
  const h = parseFloat(canvas.style.height) || canvas.height;
  if (!w || !h) return;
  let scale = Math.min(sw / w, sh / h);
  if (scaleMode === 'integer') scale = Math.max(1, Math.floor(scale * dpr) / dpr);
  else if (scaleMode === 'none') scale = 1;
  canvas.style.setProperty('--fit-w', `${Math.floor(w * scale)}px`);
  canvas.style.setProperty('--fit-h', `${Math.floor(h * scale)}px`);
}

new MutationObserver(fitCanvas).observe(canvas, { attributes: true, attributeFilter: ['width', 'height', 'style'] });
window.addEventListener('resize', fitCanvas);

$('#btn-fullscreen').addEventListener('click', () => {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen?.();
  canvas.focus();
});

// ---------------------------------------------------------------------------
// File dialogs. Browsers only open file pickers in response to a click or
// key press. love.window.showFileDialog runs later (in the game's frame), so
// unless the key press or click that led to it still counts, the engine asks
// for a click on this bar first; confirm() opens the picker from that click.
// Saving always goes through the bar: it stands in for the save dialog, and
// the file is downloaded once the game has written it.

let dialogRequest = null;

function requestGesture({ type, title, acceptLabel, cancelLabel, name }, confirm, cancel) {
  const bar = $('#dialog-bar');
  const ok = $('#dialog-ok');
  const no = $('#dialog-cancel');
  const what = type === 'savefile' ? `Save “${name}”` : type === 'openfolder' ? 'Open a folder' : 'Open a file';
  $('#dialog-title').textContent = title ? `${title}${type === 'savefile' ? ` (${name})` : ''}` : what;
  ok.textContent = acceptLabel || (type === 'savefile' ? 'Save' : type === 'openfolder' ? 'Choose folder…' : 'Choose file…');
  no.textContent = cancelLabel || 'Cancel';

  const close = () => {
    bar.hidden = true;
    ok.onclick = no.onclick = null;
    dialogRequest = null;
    canvas.focus();
  };
  ok.onclick = () => { confirm(); close(); };
  no.onclick = () => { close(); cancel(); };
  dialogRequest = { cancel: no.onclick };
  bar.hidden = false;
  ok.focus();
}

// Keys pressed on the bar aren't game input.
for (const type of ['keydown', 'keyup', 'keypress']) {
  $('#dialog-bar').addEventListener(type, (e) => {
    e.stopPropagation();
    if (type === 'keydown' && e.key === 'Escape') dialogRequest?.cancel();
  });
}

// ---------------------------------------------------------------------------
// Engine

let Module = null;
let running = false;
let syncing = false;

// Combined download progress of the engine and the game.
const downloads = { engine: [0, 0], game: [0, 0] };

function showDownloadProgress() {
  if (running || overlay.hidden) return;
  const loaded = downloads.engine[0] + downloads.game[0];
  const total = downloads.engine[1] + downloads.game[1];
  if (total > 0 && loaded < total)
    showStatus(embedded ? `Loading… ${(loaded / 1e6).toFixed(1)} / ${(total / 1e6).toFixed(1)} MB` : '', { progress: loaded / total });
}

// Size of a response body as it will be read, or 0 if unknown (compressed
// responses report the compressed length).
function bodySize(res) {
  return res.headers.get('content-encoding') ? 0 : Number(res.headers.get('content-length')) || 0;
}

// Downloads the engine with progress, compiling it while it streams in.
function instantiateEngine(imports, done) {
  fetch(new URL(`love.wasm?v=${BUILD}`, import.meta.url)).then((res) => {
    if (!res.ok) throw new Error(`love.wasm: ${res.status} ${res.statusText}`);
    downloads.engine[1] = bodySize(res);
    const counted = res.body.pipeThrough(new TransformStream({
      transform(chunk, controller) {
        downloads.engine[0] += chunk.length;
        showDownloadProgress();
        controller.enqueue(chunk);
      },
    }));
    const wasm = new Response(counted, { headers: { 'Content-Type': 'application/wasm' } });
    return WebAssembly.instantiateStreaming(wasm, imports);
  }).then(({ instance, module }) => done(instance, module))
    .catch((e) => fail(`Could not load the LÖVE engine.\n${e?.message || e}`));
  return {};
}

const engineReady = createLove({
  canvas,
  locateFile: (path, prefix) => `${prefix}${path}?v=${BUILD}`,
  instantiateWasm: instantiateEngine,
  print: (t) => output('out', t),
  printErr: (t) => output('err', t),
  onLoveQuit: (code) => onQuit(code),
  loveRequestGesture: requestGesture,
  onAbort: (what) => fail(`The engine crashed: ${what}`),
}).then((M) => {
  Module = M;
  return M;
}).catch((e) => {
  fail(`Could not load the LÖVE engine.\n${e?.message || e}`);
  throw e;
});

function syncSaves(populate = false) {
  if (!Module || syncing) return Promise.resolve();
  syncing = true;
  return new Promise((resolve) => {
    Module.FS.syncfs(populate, (err) => {
      syncing = false;
      if (err) console.warn('Save data sync failed:', err);
      resolve();
    });
  });
}

function mkdirp(FS, path) {
  let cur = '';
  for (const part of path.split('/').filter(Boolean)) {
    cur += '/' + part;
    if (!FS.analyzePath(cur).exists) FS.mkdir(cur);
  }
}

async function mountSaves(M) {
  mkdirp(M.FS, SAVE_ROOT);
  try {
    M.FS.mount(M.IDBFS, {}, SAVE_ROOT);
    await syncSaves(true);
  } catch (e) {
    console.warn('Persistent saves unavailable:', e);
  }
  setInterval(() => running && syncSaves(false), 2000);
  document.addEventListener('visibilitychange', () => document.hidden && syncSaves(false));
  window.addEventListener('pagehide', () => syncSaves(false));
}

// Load save data from IndexedDB as soon as the engine is ready, so it's done
// by the time the player presses Play.
const savesReady = engineReady.then(mountSaves);

function writeFiles(M, files) {
  for (const [rawPath, data] of Object.entries(files)) {
    const path = `${GAME_DIR}/${rawPath.replace(/^\/+/, '')}`;
    mkdirp(M.FS, path.slice(0, path.lastIndexOf('/')));
    M.FS.writeFile(path, typeof data === 'string' ? data : new Uint8Array(data));
  }
}

/** game: {love: Uint8Array} | {files: {path: string|Uint8Array}} */
async function start(game) {
  if (running) return;
  if (embedded) showStatus('Starting…', { progress: null });
  const M = await engineReady;
  await savesReady;

  let arg;
  if (game.love) {
    M.FS.writeFile(GAME_FILE, new Uint8Array(game.love));
    arg = GAME_FILE;
  } else {
    mkdirp(M.FS, GAME_DIR);
    writeFiles(M, game.files);
    arg = GAME_DIR;
  }

  running = true;
  hideOverlay();
  $('#toolbar').hidden = false;
  canvas.focus();
  post({ type: 'started' });

  // SDL writes the LÖVE window title to document.title. Report it to the
  // embedding page, but keep a packaged game's page title over LÖVE's
  // default "Untitled".
  const pageTitle = document.title;
  let lastTitle = '';
  const onTitle = () => {
    if (document.title === 'Untitled' && pageTitle !== 'Cupid Player') document.title = pageTitle;
    if (document.title !== lastTitle) {
      lastTitle = document.title;
      post({ type: 'title', title: lastTitle });
    }
  };
  new MutationObserver(onTitle).observe(document.head, { subtree: true, childList: true, characterData: true });

  M._ljit_configure?.(jitEnabled ? 1 : 0, 0);
  try {
    M.callMain([arg]);
  } catch (e) {
    if (e?.name !== 'ExitStatus') fail(`The engine crashed while starting: ${e?.message || e}`);
  }
  fitCanvas();
}

async function onQuit(code) {
  running = false;
  // A dialog left open has no game to report back to.
  $('#dialog-bar').hidden = true;
  dialogRequest = null;
  flushError();
  await syncSaves(false);
  post({ type: 'quit', code });
  if (!embedded) {
    showMessage(code ? `The game exited with code ${code}.` : 'The game has quit.', { progress: false });
    playButton.hidden = false;
    playButton.onclick = () => location.reload();
  }
}

// ---------------------------------------------------------------------------
// Getting the game

async function fetchWithProgress(url) {
  // Revalidate so an updated game is never served stale from the HTTP cache.
  const res = await fetch(url, { cache: 'no-cache' });
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  downloads.game[1] = bodySize(res);
  if (!res.body) return new Uint8Array(await res.arrayBuffer());
  const reader = res.body.getReader();
  const chunks = [];
  let received = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    received += value.length;
    downloads.game[0] = received;
    showDownloadProgress();
  }
  const out = new Uint8Array(received);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return out;
}

// ---------------------------------------------------------------------------
// The splash: splash.png at the top of the .love, if there is one, or
// Cupid's heart. It stays up while the game loads, and for at least its
// duration, then fades into the game. splash.json, next to it, can change
// the rest:
//
//   { "image": "splash.png", "background": "#1a1c2c", "duration": 2,
//     "madeWith": true, "pixel": false }

const SPLASH_DEFAULTS = { image: '', background: '', duration: 1, madeWith: true };

/** One file out of a zip (stored or deflated), or null. */
async function readZipFile(zip, name) {
  const view = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  let end = zip.length - 22;
  while (end >= 0 && view.getUint32(end, true) !== 0x06054b50) end--;
  if (end < 0) return null;
  let at = view.getUint32(end + 16, true);
  const count = view.getUint16(end + 10, true);
  const decoder = new TextDecoder();
  for (let i = 0; i < count && view.getUint32(at, true) === 0x02014b50; i++) {
    const method = view.getUint16(at + 10, true);
    const size = view.getUint32(at + 20, true);
    const nameLength = view.getUint16(at + 28, true);
    const extra = view.getUint16(at + 30, true) + view.getUint16(at + 32, true);
    const local = view.getUint32(at + 42, true);
    if (decoder.decode(zip.subarray(at + 46, at + 46 + nameLength)) === name) {
      const start = local + 30 + view.getUint16(local + 26, true) + view.getUint16(local + 28, true);
      const data = zip.subarray(start, start + size);
      if (method === 0) return data;
      if (method !== 8 || typeof DecompressionStream === 'undefined') return null;
      const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
      return new Uint8Array(await new Response(stream).arrayBuffer());
    }
    at += 46 + nameLength + extra;
  }
  return null;
}

/** "#1a1c2c" or [r, g, b] (0 to 1) as a CSS color, or ''. */
function cssColor(c) {
  if (typeof c === 'string' && /^#[0-9a-f]{3,8}$/i.test(c)) return c;
  if (Array.isArray(c) && c.length >= 3 && c.every((n) => typeof n === 'number'))
    return `rgb(${c.slice(0, 3).map((n) => Math.round(Math.min(1, Math.max(0, n)) * 255)).join(' ')}${c.length > 3 ? ` / ${c[3]}` : ''})`;
  return '';
}

/** True for a background light enough to want dark text on it. */
function isLight(c) {
  let rgb = Array.isArray(c) ? c.slice(0, 3) : null;
  if (typeof c === 'string' && /^#[0-9a-f]{6}$/i.test(c)) rgb = [1, 3, 5].map((i) => parseInt(c.slice(i, i + 2), 16) / 255);
  return !!rgb && 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2] > 0.6;
}

async function readSplash(game) {
  if (!game.love) return { ...SPLASH_DEFAULTS };
  let settings = {};
  try {
    const json = await readZipFile(game.love, 'splash.json');
    if (json) settings = JSON.parse(new TextDecoder().decode(json)) || {};
  } catch { /* not a zip, or no readable splash.json */ }
  const splash = { ...SPLASH_DEFAULTS, image: 'splash.png', ...(settings && typeof settings === 'object' ? settings : {}) };
  if (typeof splash.image === 'string' && splash.image) {
    const bytes = await readZipFile(game.love, splash.image.replace(/^\/+/, '')).catch(() => null);
    splash.imageUrl = bytes ? URL.createObjectURL(new Blob([bytes], { type: /\.svg$/i.test(splash.image) ? 'image/svg+xml' : '' })) : '';
  }
  return splash;
}

function showSplash(splash) {
  const background = cssColor(splash.background);
  if (background) overlay.style.setProperty('--splash-bg', background);
  overlay.classList.toggle('light', isLight(splash.background));
  splashImage.hidden = !splash.imageUrl;
  if (splash.imageUrl) splashImage.src = splash.imageUrl;
  splashImage.classList.toggle('pixel', !!splash.pixel);
  // An SVG element has no hidden property, so set the attribute itself.
  splashMark.toggleAttribute('hidden', !!splash.imageUrl || !splash.madeWith);
  splashCaption.hidden = !splash.madeWith;
  overlay.classList.remove('message');
  overlay.hidden = false;
}

async function startWithSplash(game, name) {
  document.title = params.get('title') || name || 'LÖVE game';
  if (embedded) return start(game);
  const splash = await readSplash(game);
  showSplash(splash);
  const shown = new Promise((resolve) => setTimeout(resolve, Math.max(0, Math.min(10, Number(splash.duration) || 0)) * 1000));
  await Promise.all([engineReady, savesReady]);
  progressEl.classList.remove('indeterminate');
  progressEl.style.width = '100%';
  await shown;
  return start(game);
}

function acceptDroppedFiles() {
  filePicker.hidden = false;
  const input = filePicker.querySelector('input');
  const load = async (file) => {
    if (!file) return;
    filePicker.hidden = true;
    startWithSplash({ love: new Uint8Array(await file.arrayBuffer()) }, file.name.replace(/\.(love|zip)$/i, ''));
  };
  input.addEventListener('change', () => load(input.files[0]));
  window.addEventListener('dragover', (e) => { e.preventDefault(); document.body.classList.add('dragging'); });
  window.addEventListener('dragleave', () => document.body.classList.remove('dragging'));
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    document.body.classList.remove('dragging');
    if (!running) load(e.dataTransfer.files[0]);
  });
}

if (embedded) {
  showStatus('Loading engine…', { progress: null });
  window.addEventListener('message', (e) => {
    if (e.source !== window.parent || !e.data || typeof e.data !== 'object') return;
    const msg = e.data;
    if (msg.type === 'run') start(msg.love ? { love: msg.love } : { files: msg.files || {} });
    else if (msg.type === 'stop') Module?._loveweb_quit();
    else if (msg.type === 'pause') Module?._loveweb_set_paused(msg.paused ? 1 : 0);
    else if (msg.type === 'focus') canvas.focus();
  });
  engineReady.then(() => post({ type: 'ready' }));
} else if (gameUrl) {
  // Cupid's splash until the game's own arrives with the game.
  showSplash({ ...SPLASH_DEFAULTS });
  showStatus('', { progress: 0 });
  fetchWithProgress(gameUrl)
    .then((love) => startWithSplash({ love }, decodeURIComponent(gameUrl.split('/').pop().replace(/\.(love|zip)$/i, ''))))
    .catch((e) => fail(`Could not download ${gameUrl}: ${e.message}`));
} else {
  showMessage('No game loaded.', { progress: false });
  acceptDroppedFiles();
}

canvas.addEventListener('pointerdown', () => canvas.focus());
