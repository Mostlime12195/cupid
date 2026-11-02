// Instantiates functions compiled by the Lua JIT (deps/lua-5.1.5/ljit.inc)
// and installs them in the indirect function table.
addToLibrary({
  $ljit: {
    free: [],
    imports: null,
    failed: false,
  },

  ljit_instantiate__deps: ['$ljit', '$wasmTable'],
  ljit_instantiate: (ptr, size) => {
    try {
      if (!ljit.imports) {
        // Order must match the H_ enum in ljit.inc.
        const helpers = ['ljit_h_exec', 'ljit_h_compare', 'ljit_h_call', 'ljit_h_tailcall',
                         'ljit_h_return', 'ljit_h_forprep', 'ljit_h_tforloop', 'ljit_h_enter'];
        const e = { m: wasmMemory };
        helpers.forEach((name, i) => { e[`h${i}`] = wasmExports[name] || Module[`_${name}`]; });
        ljit.imports = { e };
      }
      const module = new WebAssembly.Module(HEAPU8.subarray(ptr, ptr + size));
      const fn = new WebAssembly.Instance(module, ljit.imports).exports.f;
      let index = ljit.free.pop();
      if (index === undefined) index = wasmTable.grow(1);
      wasmTable.set(index, fn);
      return index;
    } catch (e) {
      // Fall back to the interpreter for this function.
      if (!ljit.failed) {
        ljit.failed = true;
        err(`Lua JIT: could not compile a function (${e}); it will be interpreted.`);
      }
      return 0;
    }
  },

  ljit_release__deps: ['$ljit', '$wasmTable'],
  ljit_release: (index) => {
    wasmTable.set(index, null);
    ljit.free.push(index);
  },
});
