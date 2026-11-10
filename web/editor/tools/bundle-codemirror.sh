#!/bin/sh
# Rebuilds web/editor/vendor/codemirror.js: CodeMirror 6 and its
# dependencies as one file, so the editor loads it in one request from its
# own host. Needs Node.js. To update a package, change its version here and
# in the header this writes.
set -e
out="$(cd "$(dirname "$0")/.." && pwd)/vendor/codemirror.js"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
cat > package.json <<'JSON'
{ "private": true, "dependencies": {
  "@codemirror/state": "6.5.2", "@codemirror/view": "6.38.1", "@codemirror/language": "6.11.2",
  "@codemirror/commands": "6.8.1", "@codemirror/search": "6.5.11", "@codemirror/autocomplete": "6.18.6",
  "@codemirror/legacy-modes": "6.5.1", "@lezer/common": "1.2.3", "@lezer/highlight": "1.2.1",
  "@marijn/find-cluster-break": "1.0.2", "crelt": "1.0.6", "style-mod": "4.1.2", "w3c-keyname": "2.2.8",
  "esbuild": "0.25.10" } }
JSON
cat > entry.js <<'JS'
export * as state from '@codemirror/state';
export * as view from '@codemirror/view';
export * as language from '@codemirror/language';
export * as commands from '@codemirror/commands';
export * as search from '@codemirror/search';
export * as autocomplete from '@codemirror/autocomplete';
export * as highlight from '@lezer/highlight';
export * as lua from '@codemirror/legacy-modes/mode/lua';
export * as clike from '@codemirror/legacy-modes/mode/clike';
export * as javascript from '@codemirror/legacy-modes/mode/javascript';
JS
npm install --no-audit --no-fund --silent
./node_modules/.bin/esbuild entry.js --bundle --format=esm --minify --target=es2020 --legal-comments=none --outfile=bundle.js
{
  cat <<'HEADER'
// CodeMirror 6 and its dependencies in one file, made by
// web/editor/tools/bundle-codemirror.sh. Versions:
//   @codemirror/state 6.5.2, view 6.38.1, language 6.11.2, commands 6.8.1,
//   search 6.5.11, autocomplete 6.18.6, legacy-modes 6.5.1 (lua, clike,
//   javascript), @lezer/common 1.2.3, @lezer/highlight 1.2.1,
//   @marijn/find-cluster-break 1.0.2, crelt 1.0.6, style-mod 4.1.2,
//   w3c-keyname 2.2.8.
// MIT License. Copyright (C) 2018-2021 by Marijn Haverbeke <marijn@haverbeke.berlin>
// and others. https://codemirror.net/
HEADER
  cat bundle.js
} > "$out"
echo "wrote $out"
