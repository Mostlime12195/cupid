// JS functions callable from the LÖVE web runtime (C++ side).
addToLibrary({
  loveweb_on_quit: (code, restart) => {
    if (Module.onLoveQuit) Module.onLoveQuit(code, !!restart);
  },
});
