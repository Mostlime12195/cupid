// JS functions callable from the LÖVE web runtime (C++ side).
addToLibrary({
  loveweb_on_quit: (code, restart) => {
    if (Module.onLoveQuit) Module.onLoveQuit(code, !!restart);
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
});
