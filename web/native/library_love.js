// JS functions callable from the LÖVE web runtime (C++ side).
//
// Optional hooks the page can set on the Module object (the player does):
//   onLoveQuit(code, restart)        the game quit
//   loveRequestGesture(info, ok, cancel)
//                                    show a button so a file dialog can be
//                                    opened from a click; call ok() inside
//                                    the click handler, or cancel()
//   loveDownloadFile(name, bytes)    replaces the default <a download>
//   onLoveWindowMode(fullscreen)     the game set its window mode (setMode,
//                                    setFullscreen); fullscreen is what it
//                                    asked for. Fullscreen and borderless
//                                    windows fill the page like resizable ones.
addToLibrary({
  loveweb_on_quit: (code, restart) => {
    if (Module.onLoveQuit) Module.onLoveQuit(code, !!restart);
  },

  loveweb_window_mode_changed: (fullscreen) => {
    if (Module.onLoveWindowMode) Module.onLoveWindowMode(!!fullscreen);
  },

  // ---------------------------------------------------------------------------
  // require("https"): the lua-https backend in
  // love/src/libraries/luahttps/src/emscripten. A synchronous XMLHttpRequest
  // blocks like lua-https does on desktop.
  //
  // Returns the HTTP status; 0 with an error message as the body for network
  // errors (including CORS refusals), like lua-https' NSURL backend; or -1
  // with an error message if the URL can't be requested at all.

  loveweb_https_request__deps: ['malloc', '$UTF8ToString', '$stringToNewUTF8'],
  loveweb_https_request: (url, method, headers, nheaders, body, bodylen, outbody, outbodylen, outheaders) => {
    const output = (bytes) => {
      const ptr = _malloc(bytes.length || 1);
      HEAPU8.set(bytes, ptr);
      HEAPU32[outbody >> 2] = ptr;
      HEAP32[outbodylen >> 2] = bytes.length;
    };
    const fail = (status, message) => {
      output(new TextEncoder().encode(message));
      return status;
    };

    url = UTF8ToString(url);
    method = UTF8ToString(method);

    // Relative URLs are resolved against the page, so a game can reach files
    // next to it without knowing where it is hosted.
    let target;
    try {
      target = new URL(url, document.baseURI);
    } catch (e) {
      return fail(-1, `Invalid url: ${url}`);
    }
    if (target.protocol !== 'https:' && target.protocol !== 'http:')
      return fail(-1, 'Unknown url schema');

    const xhr = new XMLHttpRequest();
    try {
      xhr.open(method, target.href, false);
      // Synchronous requests can't ask for an ArrayBuffer. With this charset
      // every byte of the body arrives as one char code.
      xhr.overrideMimeType('text/plain; charset=x-user-defined');
      for (let i = 0; i < nheaders; i++) {
        const name = UTF8ToString(HEAPU32[(headers >> 2) + i * 2]);
        const value = UTF8ToString(HEAPU32[(headers >> 2) + i * 2 + 1]);
        // Browsers refuse some headers (Host, User-Agent, Content-Length...).
        try { xhr.setRequestHeader(name, value); } catch (e) { /* skip it */ }
      }
      const data = bodylen > 0 && method !== 'GET' && method !== 'HEAD'
        ? HEAPU8.slice(body, body + bodylen) : null;
      xhr.send(data);
    } catch (e) {
      let msg = `${e?.message || e}`;
      if (target.origin !== location.origin)
        msg += ' (cross-origin requests only work if the server allows them with CORS headers)';
      return fail(0, msg);
    }
    if (xhr.status === 0)
      return fail(0, 'Network error');

    const text = xhr.responseText;
    const bytes = new Uint8Array(text.length);
    for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i) & 0xff;
    output(bytes);
    HEAPU32[outheaders >> 2] = stringToNewUTF8(xhr.getAllResponseHeaders());
    return xhr.status;
  },

  // ---------------------------------------------------------------------------
  // love.window.showFileDialog (patched in love/src/modules/window/sdl/Window.cpp).
  //
  // openfile / openfolder: a browser file picker; picked files are copied to
  // /picked/<n>/ in the in-memory filesystem and their paths are returned, so
  // games read them as on desktop (love.filesystem.openNativeFile,
  // mountFullPath, io.open).
  // savefile: returns /picked/<n>/<name>; when the game has written and
  // closed that file it is offered as a download.
  //
  // Pickers only open from a click or key press. If the game's call isn't
  // close enough to one (navigator.userActivation), the page is asked to show
  // a button first (Module.loveRequestGesture).

  $loveDialogs__deps: ['$FS', '$stringToNewUTF8', 'free', 'loveweb_file_dialog_add', 'loveweb_file_dialog_done'],
  $loveDialogs: {
    count: 0,
    queue: [],
    busy: false,
    saves: {},
    hooked: false,

    request(d) {
      loveDialogs.queue.push(d);
      if (!loveDialogs.busy) loveDialogs.next();
    },

    next() {
      const d = loveDialogs.queue.shift();
      loveDialogs.busy = !!d;
      if (!d) return;
      let finished = false;
      const done = (paths, filter, err) => {
        if (finished) return;
        finished = true;
        loveDialogs.finish(d.state, paths, filter, err);
        loveDialogs.next();
      };
      try {
        loveDialogs.show(d, done);
      } catch (e) {
        done([], -1, `${e?.message || e}`);
      }
    },

    finish(state, paths, filter, err) {
      for (const path of paths) {
        const p = stringToNewUTF8(path);
        _loveweb_file_dialog_add(state, p);
        _free(p);
      }
      const e = err ? stringToNewUTF8(err) : 0;
      _loveweb_file_dialog_done(state, filter, e);
      if (e) _free(e);
    },

    show(d, done) {
      const save = d.type === 'savefile';
      const name = save ? loveDialogs.saveName(d) : '';
      const go = save
        ? () => done([loveDialogs.prepareSave(name)], loveDialogs.filterIndex(d.filters, name))
        : () => loveDialogs.pick(d, done);

      // Saving always asks first (it stands in for the save dialog, and the
      // click lets the download start without the browser objecting).
      if (!save && navigator.userActivation?.isActive) return go();
      const ask = Module.loveRequestGesture;
      if (ask) {
        ask({ type: d.type, title: d.title, acceptLabel: d.acceptLabel, cancelLabel: d.cancelLabel, name },
          go, () => done([], -1));
      } else if (save) {
        go();
      } else {
        done([], -1, 'File dialogs can only be opened from a key press or click on the web.');
      }
    },

    pick(d, done) {
      const folder = d.type === 'openfolder';
      const input = document.createElement('input');
      input.type = 'file';
      const accept = loveDialogs.accept(d.filters);
      if (accept && !folder) input.accept = accept;
      input.multiple = !!d.multiSelect;
      if (folder) input.webkitdirectory = true;
      input.style.display = 'none';
      document.body.append(input);

      let settled = false;
      const settle = (files) => {
        if (settled) return;
        settled = true;
        input.remove();
        window.removeEventListener('focus', onFocus);
        if (!files || files.length === 0) return done([], -1);
        loveDialogs.store(files, folder).then(
          (paths) => done(paths, folder ? -1 : loveDialogs.filterIndex(d.filters, files[0].name)),
          (e) => done([], -1, `Could not read the chosen files: ${e?.message || e}`));
      };
      input.addEventListener('change', () => settle(Array.from(input.files || [])));
      input.addEventListener('cancel', () => settle(null));
      // Browsers without the cancel event: focus returns when the picker
      // closes; nothing picked by then means it was cancelled.
      const onFocus = () => setTimeout(() => { if (!input.files?.length) settle(null); }, 1000);
      if (!('oncancel' in input)) window.addEventListener('focus', onFocus);
      input.click();
    },

    async store(files, folder) {
      const dir = `/picked/${++loveDialogs.count}`;
      loveDialogs.mkdirp(dir);
      const clean = (p) => p.split('/').filter((s) => s && s !== '.' && s !== '..').join('/');
      const paths = [];
      let root = null;
      for (const file of files) {
        const rel = clean(folder && file.webkitRelativePath ? file.webkitRelativePath : file.name) || 'file';
        const path = `${dir}/${rel}`;
        loveDialogs.mkdirp(path.slice(0, path.lastIndexOf('/')));
        FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
        if (folder) root ??= rel.includes('/') ? `${dir}/${rel.split('/')[0]}` : dir;
        else paths.push(path);
      }
      return folder ? [root] : paths;
    },

    // SDL filter patterns are extension lists like "png;jpg"; "*" is anything.
    extensions(pattern) {
      return pattern.split(';').map((p) => p.trim().replace(/^\*?\./, '').toLowerCase()).filter(Boolean);
    },

    accept(filters) {
      const out = [];
      for (const f of filters) {
        for (const ext of loveDialogs.extensions(f.pattern)) {
          if (ext === '*') return '';
          out.push(`.${ext}`);
        }
      }
      return out.join(',');
    },

    filterIndex(filters, name) {
      const ext = name.includes('.') ? name.split('.').pop().toLowerCase() : '';
      return filters.findIndex((f) => loveDialogs.extensions(f.pattern).some((e) => e === '*' || e === ext));
    },

    saveName(d) {
      let name = d.defaultName.split(/[\\/]/).pop() || '';
      if (!name) {
        const ext = d.filters.length ? loveDialogs.extensions(d.filters[0].pattern)[0] : null;
        name = ext && ext !== '*' ? `untitled.${ext}` : 'untitled';
      }
      return name;
    },

    prepareSave(name) {
      const dir = `/picked/${++loveDialogs.count}`;
      loveDialogs.mkdirp(dir);
      const path = `${dir}/${name}`;
      loveDialogs.hookClose();
      loveDialogs.saves[path] = { name, timer: 0 };
      return path;
    },

    // A file counts as saved when a stream that could write to it is closed
    // (fclose, love's File:close, io.close...). Writes in quick succession
    // become one download.
    hookClose() {
      if (loveDialogs.hooked) return;
      loveDialogs.hooked = true;
      const close = FS.close;
      FS.close = function (stream) {
        const path = stream.path;
        const wrote = (stream.flags & 3) !== 0; // O_WRONLY or O_RDWR
        const result = close.apply(this, arguments);
        if (wrote && path && loveDialogs.saves[path]) loveDialogs.saved(path);
        return result;
      };
    },

    saved(path) {
      const s = loveDialogs.saves[path];
      clearTimeout(s.timer);
      s.timer = setTimeout(() => {
        let data;
        try { data = FS.readFile(path); } catch (e) { return; }
        (Module.loveDownloadFile || loveDialogs.download)(s.name, data);
      }, 100);
    },

    download(name, data) {
      const url = URL.createObjectURL(new Blob([data], { type: 'application/octet-stream' }));
      const a = document.createElement('a');
      a.href = url;
      a.download = name;
      a.style.display = 'none';
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    },

    mkdirp(path) {
      let cur = '';
      for (const part of path.split('/').filter(Boolean)) {
        cur += '/' + part;
        if (!FS.analyzePath(cur).exists) FS.mkdir(cur);
      }
    },
  },

  loveweb_show_file_dialog__deps: ['$loveDialogs', '$UTF8ToString'],
  loveweb_show_file_dialog: (state, type, title, acceptLabel, cancelLabel, defaultName, filters, nfilters, multi) => {
    const list = [];
    for (let i = 0; i < nfilters; i++) {
      list.push({
        name: UTF8ToString(HEAPU32[(filters >> 2) + i * 2]),
        pattern: UTF8ToString(HEAPU32[(filters >> 2) + i * 2 + 1]),
      });
    }
    loveDialogs.request({
      state,
      type: ['openfile', 'openfolder', 'savefile'][type] || 'openfile',
      title: UTF8ToString(title),
      acceptLabel: UTF8ToString(acceptLabel),
      cancelLabel: UTF8ToString(cancelLabel),
      defaultName: UTF8ToString(defaultName),
      filters: list,
      multiSelect: !!multi,
    });
  },

  // ---------------------------------------------------------------------------
  // Text input. SDL's web backend only turns keypress events into text, which
  // misses IME composition, emoji pickers, dictation and on-screen keyboards.
  //
  // While SDL text input is active (love.keyboard.hasTextInput()), a hidden
  // text field takes keyboard focus instead of the canvas. Key events still
  // bubble to the window where SDL handles them (love.keypressed), but all
  // text comes from the field: SDL's keypress handler is skipped for it, and
  // what the field receives is sent to the engine as text events once it is
  // committed (love.textinput), with composition in progress reported as
  // love.textedited.

  $loveText__deps: ['$stringToNewUTF8', 'free', 'loveweb_text_input', 'loveweb_text_editing', 'loveweb_text_key'],
  $loveText: {
    field: null,
    active: false,
    area: null,
    composing: false,
    lastKey: null,
    lastKeyTime: 0,

    // Phones and tablets: no hover, coarse pointer.
    touchOnly() {
      return !!globalThis.matchMedia?.('(hover: none) and (pointer: coarse)').matches;
    },

    canvas() {
      return Module.canvas;
    },

    init() {
      if (loveText.field) return;
      const f = loveText.field = document.createElement('textarea');
      f.className = 'love-text-input';
      f.setAttribute('aria-label', 'Game text input');
      f.setAttribute('autocomplete', 'off');
      f.setAttribute('autocorrect', 'off');
      f.setAttribute('autocapitalize', 'off');
      f.setAttribute('spellcheck', 'false');
      f.tabIndex = -1;
      Object.assign(f.style, {
        position: 'fixed', left: '0', top: '0', width: '1px', height: '1px',
        margin: '0', padding: '0', border: '0', outline: 'none', resize: 'none', overflow: 'hidden',
        opacity: '0', background: 'transparent', color: 'transparent', caretColor: 'transparent',
        // 16px stops iOS from zooming in on focus.
        font: '16px/1 sans-serif', whiteSpace: 'pre', pointerEvents: 'none',
      });
      document.body.append(f);

      // Text typed into the field arrives through 'input' below, so SDL's
      // keypress handler (on the window) must not see it too. Keys that are
      // part of an IME composition aren't game key presses.
      window.addEventListener('keypress', (e) => {
        if (e.target === f) e.stopImmediatePropagation();
      }, true);
      const composingKey = (e) => {
        if (e.target !== f) return;
        if (e.type === 'keydown') {
          loveText.lastKey = e.key;
          loveText.lastKeyTime = performance.now();
        }
        if (e.isComposing || e.keyCode === 229) e.stopImmediatePropagation();
      };
      window.addEventListener('keydown', composingKey, true);
      window.addEventListener('keyup', composingKey, true);

      f.addEventListener('beforeinput', (e) => {
        if (e.isComposing || loveText.composing) return;
        const t = e.inputType || '';
        // Some on-screen keyboards only report Backspace, Delete and Enter
        // as edits (keydown is "Unidentified"); send those as key presses.
        // Real key presses already reached SDL, which cancels Backspace's
        // and Delete's edits, and Enter's line break is cancelled here.
        const real = (key) => loveText.lastKey === key && performance.now() - loveText.lastKeyTime < 200;
        if (t === 'insertLineBreak' || t === 'insertParagraph') {
          e.preventDefault();
          if (!real('Enter')) _loveweb_text_key(2);
        } else if (t.startsWith('delete')) {
          e.preventDefault();
          const forward = t.endsWith('Forward');
          if (!real(forward ? 'Delete' : 'Backspace')) _loveweb_text_key(forward ? 1 : 0);
        } else if (t.startsWith('history') || t.startsWith('format') || t === 'insertFromPaste' || t === 'insertFromDrop') {
          // Pasting is left to the game (love.system.getClipboardText), as
          // on desktop.
          e.preventDefault();
        }
      });
      f.addEventListener('compositionstart', () => {
        loveText.flush();
        loveText.composing = true;
      });
      f.addEventListener('compositionend', () => {
        loveText.composing = false;
        loveText.send(_loveweb_text_editing, '', 0, 0);
        // Chrome has updated the field by now; browsers that update it in an
        // input event right after compositionend are flushed by that event.
        setTimeout(loveText.flush, 0);
      });
      f.addEventListener('input', (e) => {
        if (loveText.composing || e.isComposing) loveText.editing();
        else loveText.flush();
      });
      f.addEventListener('blur', () => {
        if (loveText.composing) {
          loveText.composing = false;
          loveText.send(_loveweb_text_editing, '', 0, 0);
          f.value = '';
        } else {
          loveText.flush();
        }
      });

      // Keyboard focus goes to the field instead of the canvas: when the
      // canvas gets it, is clicked, or the page gets focus back.
      const canvas = loveText.canvas();
      const takeFocus = () => {
        if (loveText.active && document.activeElement !== f) loveText.focusField();
      };
      canvas?.addEventListener('focus', takeFocus);
      canvas?.addEventListener('mousedown', takeFocus);
      window.addEventListener('focus', () => {
        const el = document.activeElement;
        if (!loveText.touchOnly() && (el === canvas || el === document.body || el === null)) takeFocus();
      });
      // Touch devices only show their keyboard when the field is focused
      // during a tap, and hide it without blurring the field on Android.
      const tap = (e) => {
        if (!loveText.active || e.pointerType === 'mouse') return;
        if (document.activeElement === f) f.blur();
        loveText.focusField();
      };
      canvas?.addEventListener('pointerup', tap);
      canvas?.addEventListener('touchend', tap);
      window.addEventListener('resize', () => loveText.place());
    },

    send(fn, text, ...args) {
      const p = stringToNewUTF8(text);
      fn(p, ...args);
      _free(p);
    },

    // Everything in the field outside a composition is committed text.
    flush() {
      const f = loveText.field;
      const value = f.value;
      if (!value) return;
      f.value = '';
      const text = value.replace(/[\u0000-\u001f\u007f]/g, '');
      if (text) loveText.send(_loveweb_text_input, text);
    },

    editing() {
      const f = loveText.field;
      const chars = (s) => Array.from(s).length;
      const start = f.selectionStart ?? f.value.length;
      const end = f.selectionEnd ?? start;
      loveText.send(_loveweb_text_editing, f.value,
        chars(f.value.slice(0, start)), chars(f.value.slice(start, end)));
    },

    focusField() {
      loveText.place();
      loveText.field.focus({ preventScroll: true });
    },

    // Put the field over love.keyboard.setTextInput's rectangle (or the
    // bottom left of the game), so IME candidate windows appear there.
    place() {
      const f = loveText.field;
      const a = loveText.area;
      const canvas = loveText.canvas();
      if (!f || !a || !canvas) return;
      const r = canvas.getBoundingClientRect();
      const sx = a.winw > 0 ? r.width / a.winw : 1;
      const sy = a.winh > 0 ? r.height / a.winh : 1;
      let left = r.left, top = r.bottom - 20, width = 1, height = 20;
      if (a.w > 0 && a.h > 0) {
        left = r.left + a.x * sx;
        top = r.top + a.y * sy;
        width = a.w * sx;
        height = a.h * sy;
      }
      Object.assign(f.style, {
        left: `${Math.round(left)}px`, top: `${Math.round(Math.max(0, top))}px`,
        width: `${Math.max(1, Math.round(width))}px`, height: `${Math.max(1, Math.round(height))}px`,
      });
    },

    setState(active, x, y, w, h, winw, winh) {
      loveText.init();
      const was = loveText.active;
      loveText.active = !!active;
      loveText.area = { x, y, w, h, winw, winh };
      loveText.place();

      const f = loveText.field;
      const canvas = loveText.canvas();
      const focused = document.activeElement;
      if (loveText.active && !was) {
        // Take focus from the canvas (or nothing), but never from the page
        // around an embedded player. On touch devices this shows the
        // keyboard, so only do it right after a tap.
        const idle = focused === canvas || focused === document.body || focused === null;
        const allowed = loveText.touchOnly() ? navigator.userActivation?.isActive : document.hasFocus();
        if (idle && allowed) loveText.focusField();
      } else if (!loveText.active && was && focused === f) {
        f.blur();
        canvas?.focus({ preventScroll: true });
      }
    },
  },

  loveweb_text_input_state__deps: ['$loveText'],
  loveweb_text_input_state: (active, x, y, w, h, winw, winh) => {
    loveText.setState(active, x, y, w, h, winw, winh);
  },

  // Whether text input starts enabled with the window: yes, as on desktop,
  // except on touch devices (as on Android and iOS), where enabling it
  // brings up the on-screen keyboard.
  loveweb_text_input_default__deps: ['$loveText'],
  loveweb_text_input_default: () => (loveText.touchOnly() ? 0 : 1),
});
