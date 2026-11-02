// Smitten editor: application glue.

import {
  h, icon, hydrateIcons, openDialog, promptDialog, confirmDialog, showMenu, toast, downloadBlob, formatBytes, timeAgo, isMac,
} from './ui.js';
import * as store from './fs-store.js';
import { kindOf, mimeOf, classifyBytes, languageLabel, iconFor, baseName, dirName, byteSize } from './file-types.js';
import { createEditor } from './code-editor.js';
import { createLoveCompletions } from './completions.js';
import { ConsolePanel, findLocation } from './console.js';
import { Runner, openPopup, showInPopup } from './runner.js';
import { FileTree, readDataTransfer } from './file-tree.js';
import { initLayout } from './layout.js';
import { BLANK, loadExampleIndex, loadTemplateFiles } from './templates.js';
import { buildLove, buildWebExport, unzipToProjectFiles, slugify } from './exporter.js';
import { createShareLink, readShareHash } from './share.js';

const $ = (sel) => document.querySelector(sel);
const LS = {
  get(key, fallback = null) {
    try { const v = localStorage.getItem(key); return v === null ? fallback : JSON.parse(v); } catch { return fallback; }
  },
  set(key, value) {
    try { localStorage.setItem(key, JSON.stringify(value)); } catch { /* ignore */ }
  },
};

const state = {
  project: null,
  entries: new Map(), // path -> {path, kind, data}
  tabs: [],
  active: null,
  saved: new Map(), // path -> text as last saved (open text files)
  dirty: new Set(),
  errors: new Map(), // path -> {line, message}
  autorun: LS.get('lws.autorun', false),
  api: null,
  hasRun: false,
  lastError: false,
};

let editor;
let runner;
let consolePanel;
let tree;

// ---------------------------------------------------------------------------
// Boot

async function init() {
  hydrateIcons();
  initLayout($('#app'));
  setSidebarVisible(LS.get('lws.sidebar', innerWidth > 860), false);
  updateThemeButton();
  setAutorun(state.autorun);

  consolePanel = new ConsolePanel($('#console-output'), {
    filter: $('#console-filter'), level: $('#console-level'), autoscroll: $('#console-autoscroll'),
    clear: $('#console-clear'), badge: $('#console-badge'),
  }, {
    fileExists: (p) => state.entries.get(p)?.kind === 'file',
    onOpenLocation: (path, line) => openFile(path, { line }),
  });

  runner = new Runner($('#game-stage'), {
    onState: onRunnerState,
    onLog: (level, text) => {
      // A Lua panic kills the engine without going through LÖVE's error screen.
      if (/^PANIC: /.test(text)) onGameError(text);
      else consolePanel.log(level, text);
    },
    onError: onGameError,
    onTitle: (title) => { $('#game-title').textContent = title || state.project?.name || 'Game'; },
    onQuit: (code) => consolePanel.info(code ? `Game exited with code ${code}.` : 'Game exited.'),
    onShortcut: (e) => handleShortcut(e, true),
  });

  tree = new FileTree($('#file-tree'), {
    onOpen: (path, opts) => openFile(path, { focus: opts?.focusEditor }),
    onContextMenu: showTreeMenu,
    onMove: moveEntry,
    onDropFiles: async (dt, dir) => importFiles(await readDataTransfer(dt), dir),
    isDirty: (p) => state.dirty.has(p),
    onEmptyAction: () => newFile(''),
    onKeyAction: (action, path, kind) => {
      if (action === 'rename') renameEntry(path, kind);
      if (action === 'delete') deleteEntry(path, kind);
    },
  });

  wireUi();
  loadApi();

  editor = await createEditor($('#editor-host'), {
    onChange: onEditorChange,
    onCursor: (line, col, sel) => {
      $('#status-cursor').textContent = `Ln ${line}, Col ${col}${sel > 1 ? ` (${sel} cursors)` : ''}`;
    },
  });
  attachCompletions();
  $('#app').classList.remove('booting');

  let opened = false;
  try {
    opened = await importShareHash();
  } catch (e) {
    toast(`Could not open the shared project: ${e.message}`, { type: 'error', timeout: 6000 });
  }
  if (!opened) {
    const projects = await store.listProjects();
    const last = LS.get('lws.lastProject');
    const target = projects.find((p) => p.id === last) || projects[0];
    if (target) await openProject(target.id);
    else {
      const p = await store.createProject('My First Game', BLANK.files);
      await openProject(p.id);
      consolePanel.info('Welcome to Smitten! Press F5 to run, or open the project menu to try the examples.');
    }
  }
  window.addEventListener('hashchange', () => importShareHash().catch((e) => toast(e.message, { type: 'error' })));
}

async function loadApi() {
  try {
    const res = await fetch('love-api.json', { cache: 'no-cache' });
    state.api = await res.json();
    $('#status-engine').textContent = `LÖVE ${state.api.version}`;
    attachCompletions();
  } catch (e) {
    console.warn('love-api.json could not be loaded', e);
  }
}

function attachCompletions() {
  if (!editor || editor.kind !== 'codemirror') return;
  editor.completionSource = createLoveCompletions(state.api, editor.cm, () => [...state.entries.values()].filter((e) => e.kind === 'file').map((e) => e.path));
}

// ---------------------------------------------------------------------------
// Projects

async function openProject(id) {
  if (state.project && state.dirty.size) await saveAll({ quiet: true });
  const project = await store.getProject(id);
  if (!project) return;
  runner.destroy();
  for (const path of state.tabs) editor.close(path);
  Object.assign(state, { project, tabs: [], active: null, hasRun: false, lastError: false });
  onRunnerState('idle');
  state.saved.clear();
  state.dirty.clear();
  state.errors.clear();
  LS.set('lws.lastProject', id);

  $('#project-name').textContent = project.name;
  $('#game-title').textContent = project.name;
  document.title = `${project.name} · Smitten`;
  tree.setProject(id);
  await refreshEntries();

  const tabs = (project.tabs || []).filter((p) => state.entries.get(p)?.kind === 'file');
  if (!tabs.length && state.entries.has('main.lua')) tabs.push('main.lua');
  state.tabs = tabs;
  const active = tabs.includes(project.active) ? project.active : tabs[0];
  if (active) await openFile(active, { persist: false });
  else showEmptyEditor();
  renderTabs();
  consolePanel.separator(`Opened ${project.name}`);
}

async function refreshEntries() {
  const list = await store.listEntries(state.project.id);
  state.entries = new Map(list.map((e) => [e.path, e]));
  tree.setEntries(list);
  const files = list.filter((e) => e.kind === 'file');
  const size = files.reduce((n, e) => n + byteSize(e.data), 0);
  $('#sidebar-footer').replaceChildren(
    h('span', {}, `${files.length} file${files.length === 1 ? '' : 's'}`),
    h('span', {}, formatBytes(size)),
  );
}

let persistTimer = 0;
function persistTabs() {
  clearTimeout(persistTimer);
  const id = state.project?.id;
  const data = { tabs: [...state.tabs], active: state.active };
  persistTimer = setTimeout(() => { if (id) store.updateProject(id, data).catch(() => {}); }, 300);
}

async function uniqueProjectName(base) {
  const names = new Set((await store.listProjects()).map((p) => p.name));
  if (!names.has(base)) return base;
  for (let i = 2; ; i++) if (!names.has(`${base} ${i}`)) return `${base} ${i}`;
}

async function createFromTemplate(template, name) {
  const files = await loadTemplateFiles(template);
  const project = await store.createProject(await uniqueProjectName(name || template.name), files);
  await openProject(project.id);
  toast(`Created “${project.name}”`, { type: 'success' });
  return project;
}

async function importShareHash() {
  const shared = await readShareHash();
  if (!shared) return false;
  history.replaceState(null, '', location.pathname + location.search);
  const project = await store.createProject(await uniqueProjectName(shared.name), shared.files);
  await openProject(project.id);
  toast(`Opened shared project “${project.name}”`, { type: 'success' });
  return true;
}

async function openArchiveAsProject(file) {
  try {
    const files = await unzipToProjectFiles(new Uint8Array(await file.arrayBuffer()));
    if (!Object.keys(files).length) throw new Error('The archive is empty.');
    const name = file.name.replace(/\.(love|zip)$/i, '') || 'Imported game';
    const project = await store.createProject(await uniqueProjectName(name), files);
    await openProject(project.id);
    toast(`Imported ${Object.keys(files).length} files from ${file.name}`, { type: 'success' });
    if (!files['main.lua']) toast('This archive has no main.lua at its root, so it will not run as is.', { type: 'error', timeout: 6000 });
  } catch (e) {
    toast(`Could not open ${file.name}: ${e.message}`, { type: 'error', timeout: 6000 });
  }
}

async function openProjectsDialog() {
  const [projects, examples] = await Promise.all([store.listProjects(), loadExampleIndex()]);
  const templates = [BLANK, ...examples];
  let chosen = BLANK;
  let projectList = projects;

  const list = h('div', { class: 'project-list', role: 'list' });
  const nameInput = h('input', { class: 'input', type: 'text', placeholder: 'Project name', value: 'My Game', 'aria-label': 'New project name', spellcheck: 'false' });
  const grid = h('div', { class: 'template-grid', role: 'group', 'aria-label': 'Templates' });
  let dlgFinish = null;

  const renderList = () => {
    list.replaceChildren(...projectList.map((p) => {
      const current = p.id === state.project?.id;
      const actionBtn = (name, title, fn) => h('button', {
        type: 'button', class: 'icon-btn small', title, 'aria-label': `${title} ${p.name}`,
        onclick: (e) => { e.stopPropagation(); fn(); },
      }, icon(name, 15));
      const item = h('div', {
        class: `project-item ${current ? 'current' : ''}`, role: 'listitem', tabindex: '0',
        onclick: async () => { dlgFinish?.(null); if (!current) await openProject(p.id); },
        onkeydown: (e) => { if (e.key === 'Enter' && e.target === item) item.click(); },
      },
      h('span', { class: current ? 'heart' : 'preview-icon' }, icon(current ? 'heart' : 'folder', 16)),
      h('div', { class: 'project-info' },
        h('div', { class: 'project-title' }, p.name),
        h('div', { class: 'project-meta' }, current ? 'Open now · ' : '', `Edited ${timeAgo(p.updated)}`)),
      h('div', { class: 'project-actions' },
        actionBtn('pencil', 'Rename', async () => {
          const name = await promptDialog({ title: 'Rename project', label: 'Name', value: p.name, okLabel: 'Rename', validate: validateProjectName });
          if (!name) return;
          await store.updateProject(p.id, { name });
          if (current) {
            state.project.name = name;
            $('#project-name').textContent = name;
            document.title = `${name} · Smitten`;
          }
          projectList = await store.listProjects();
          renderList();
        }),
        actionBtn('copy', 'Duplicate', async () => {
          if (current) await saveAll({ quiet: true });
          await store.duplicateProject(p.id, await uniqueProjectName(`${p.name} copy`));
          projectList = await store.listProjects();
          renderList();
          toast(`Duplicated “${p.name}”`, { type: 'success' });
        }),
        actionBtn('trash', 'Delete', async () => {
          const ok = await confirmDialog({
            title: 'Delete project?',
            message: `“${p.name}” and all of its files will be deleted from this browser. This cannot be undone.`,
            okLabel: 'Delete project', danger: true,
          });
          if (!ok) return;
          await store.deleteProject(p.id);
          projectList = await store.listProjects();
          if (current) {
            if (projectList.length) await openProject(projectList[0].id);
            else {
              const np = await store.createProject('My Game', BLANK.files);
              await openProject(np.id);
              projectList = await store.listProjects();
            }
          }
          renderList();
          toast(`Deleted “${p.name}”`);
        })));
      return item;
    }));
    if (!projectList.length) list.append(h('p', { class: 'projects-footer-note' }, 'No projects yet.'));
  };

  const renderGrid = () => {
    grid.replaceChildren(...templates.map((t) => h('button', {
      type: 'button', class: 'template-card', 'aria-pressed': String(t === chosen),
      onclick: () => {
        const prevDefault = chosen === BLANK ? 'My Game' : chosen.name;
        if (!nameInput.value.trim() || nameInput.value === prevDefault) nameInput.value = t === BLANK ? 'My Game' : t.name;
        chosen = t;
        renderGrid();
      },
      ondblclick: () => create(),
    }, h('strong', {}, t.name), h('span', {}, t.description))));
  };

  const create = async () => {
    const name = nameInput.value.trim() || chosen.name;
    dlgFinish?.(null);
    try {
      await createFromTemplate(chosen, name);
    } catch (e) {
      toast(`Could not create the project: ${e.message}`, { type: 'error' });
    }
  };
  nameInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); create(); } });

  renderList();
  renderGrid();
  const body = h('div', { class: 'projects-layout' },
    h('div', {}, h('h3', { class: 'section-title' }, 'Your projects'), list),
    h('div', {},
      h('h3', { class: 'section-title' }, 'New project'),
      grid,
      h('div', { class: 'new-project-row' }, nameInput, h('button', { type: 'button', class: 'btn primary', onclick: create }, icon('plus'), 'Create'))));

  const res = await openDialog({
    title: 'Projects',
    body,
    wide: true,
    actions: [
      { label: 'Open .love / .zip…', value: 'open-archive', ghost: true, left: true },
      { label: 'Close', value: null },
    ],
    onOpen: (dlg, finish) => { dlgFinish = finish; },
    initialFocus: '.project-item.current',
  });
  if (res === 'open-archive') $('#input-open-project').click();
}

function validateProjectName(name) {
  if (!name) return 'Please enter a name.';
  if (name.length > 80) return 'That name is too long.';
  return '';
}

// ---------------------------------------------------------------------------
// Tabs & files

function isText(path) {
  return typeof state.entries.get(path)?.data === 'string';
}

async function openFile(path, { line, focus = false, persist = true } = {}) {
  const entry = state.entries.get(path);
  if (!entry || entry.kind !== 'file') {
    toast(`“${path}” does not exist in this project.`, { type: 'error' });
    return;
  }
  if (!state.tabs.includes(path)) {
    const i = state.tabs.indexOf(state.active);
    state.tabs.splice(i < 0 ? state.tabs.length : i + 1, 0, path);
  }
  state.active = path;
  $('#editor-empty').hidden = true;

  if (typeof entry.data === 'string') {
    hidePreview();
    if (!editor.has(path)) state.saved.set(path, entry.data);
    editor.open(path, entry.data);
    const err = state.errors.get(path);
    if (err) editor.setError(path, err.line, err.message);
    if (line) editor.gotoLine(line);
    else if (focus) editor.focus();
  } else {
    editor.hide();
    showPreview(entry);
    $('#status-cursor').textContent = '';
  }
  $('#status-lang').textContent = languageLabel(path, entry.data);
  tree.setActive(path);
  renderTabs();
  if (persist) persistTabs();
}

function renderTabs() {
  const host = $('#tabs');
  host.replaceChildren(...state.tabs.map((path) => {
    const entry = state.entries.get(path);
    const selected = path === state.active;
    const name = baseName(path);
    const dupe = state.tabs.some((p) => p !== path && baseName(p) === name);
    const tab = h('div', {
      class: `tab ${state.dirty.has(path) ? 'dirty' : ''} ${state.errors.has(path) ? 'has-error' : ''}`,
      role: 'tab', tabindex: selected ? '0' : '-1', 'aria-selected': String(selected), title: path,
      dataset: { path },
    },
    icon(iconFor(path, entry?.data), 14),
    h('span', { class: 'tab-name' }, name),
    dupe && dirName(path) ? h('span', { class: 'tab-dir' }, dirName(path)) : null,
    h('button', { type: 'button', class: 'tab-close', tabindex: '-1', title: 'Close (Alt+W)', 'aria-label': `Close ${name}` }, icon('x', 13)));
    tab.addEventListener('click', (e) => {
      if (e.target.closest('.tab-close')) closeTab(path);
      else if (path !== state.active) openFile(path);
    });
    tab.addEventListener('auxclick', (e) => { if (e.button === 1) { e.preventDefault(); closeTab(path); } });
    tab.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openFile(path, { focus: true }); }
    });
    return tab;
  }));
  host.querySelector('[aria-selected="true"]')?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}

async function closeTab(path) {
  if (!path || !state.tabs.includes(path)) return;
  if (state.dirty.has(path)) {
    const res = await openDialog({
      title: 'Save changes?',
      body: h('p', { class: 'dialog-message' }, `“${baseName(path)}” has unsaved changes.`),
      actions: [
        { label: "Don't save", value: 'discard', ghost: true, left: true },
        { label: 'Cancel', value: null },
        { label: 'Save', value: 'save', primary: true, submit: true },
      ],
    });
    if (!res) return;
    if (res === 'save') await saveFile(path);
  }
  const i = state.tabs.indexOf(path);
  state.tabs.splice(i, 1);
  editor.close(path);
  state.saved.delete(path);
  state.dirty.delete(path);
  if (state.active === path) {
    state.active = null;
    const next = state.tabs[Math.min(i, state.tabs.length - 1)];
    if (next) await openFile(next);
    else showEmptyEditor();
  }
  renderTabs();
  tree.render();
  persistTabs();
}

function showEmptyEditor() {
  state.active = null;
  editor?.hide();
  hidePreview();
  $('#editor-empty').hidden = false;
  $('#status-cursor').textContent = '';
  $('#status-lang').textContent = '';
  tree.setActive(null);
  persistTabs();
}

function onEditorChange(path) {
  if (!path) return;
  const wasDirty = state.dirty.has(path);
  const isDirty = editor.getText(path) !== state.saved.get(path);
  if (wasDirty === isDirty) return;
  if (isDirty) state.dirty.add(path);
  else state.dirty.delete(path);
  renderTabs();
  tree.render();
}

async function saveFile(path) {
  if (!editor.has(path)) return false;
  const text = editor.getText(path);
  await store.writeFile(state.project.id, path, text);
  const entry = state.entries.get(path);
  if (entry) entry.data = text;
  state.saved.set(path, text);
  state.dirty.delete(path);
  return true;
}

async function saveAll({ quiet = false } = {}) {
  const paths = [...state.dirty];
  for (const p of paths) await saveFile(p);
  if (paths.length) {
    renderTabs();
    tree.render();
    if (!quiet) flashStatus(`Saved ${paths.length} file${paths.length === 1 ? '' : 's'}`);
  }
  return paths.length;
}

async function saveActive() {
  if (!state.active || !isText(state.active)) return;
  await saveFile(state.active);
  renderTabs();
  tree.render();
  flashStatus(`Saved ${baseName(state.active)}`);
  if (state.autorun) run({ focus: false });
}

let statusTimer = 0;
function flashStatus(text) {
  const el = $('#status-left');
  el.textContent = text;
  clearTimeout(statusTimer);
  statusTimer = setTimeout(() => { el.textContent = ''; }, 2500);
}

// ---------------------------------------------------------------------------
// Binary previews

let previewUrl = null;
let fontCounter = 0;

function hidePreview() {
  $('#preview').hidden = true;
  $('#preview').replaceChildren();
  if (previewUrl) URL.revokeObjectURL(previewUrl);
  previewUrl = null;
}

function showPreview(entry) {
  hidePreview();
  const box = $('#preview');
  box.hidden = false;
  const kind = kindOf(entry.path, entry.data);
  const blob = new Blob([entry.data], { type: mimeOf(entry.path) });
  previewUrl = URL.createObjectURL(blob);
  const size = formatBytes(entry.data.byteLength);
  const downloadBtn = h('button', { type: 'button', class: 'btn small', onclick: () => downloadBlob(blob, baseName(entry.path)) }, icon('download', 14), 'Download');

  if (kind === 'image') {
    const frame = h('div', { class: 'preview-checker' });
    const img = h('img', { src: previewUrl, alt: baseName(entry.path) });
    const dims = h('span', {}, '…');
    img.addEventListener('load', () => {
      dims.replaceChildren(h('strong', {}, `${img.naturalWidth} × ${img.naturalHeight}`), ' px');
      if (img.naturalWidth <= 128 && img.naturalHeight <= 128) {
        frame.classList.add('pixelated');
        img.style.width = `${Math.min(512, img.naturalWidth * Math.floor(256 / Math.max(img.naturalWidth, img.naturalHeight) || 1))}px`;
      }
    });
    img.addEventListener('error', () => dims.replaceChildren('This image format cannot be previewed in the browser, but LÖVE may still load it.'));
    frame.append(img);
    box.append(frame, h('div', { class: 'preview-meta' }, h('strong', {}, entry.path), dims, h('span', {}, size), downloadBtn));
  } else if (kind === 'audio') {
    box.append(
      h('div', { class: 'preview-icon' }, icon('music', 48)),
      h('div', { class: 'preview-audio' }, h('audio', { controls: true, src: previewUrl, preload: 'metadata' })),
      h('div', { class: 'preview-meta' }, h('strong', {}, entry.path), h('span', {}, size), downloadBtn),
    );
  } else if (kind === 'font') {
    const family = `lws-preview-${++fontCounter}`;
    const sampleText = 'The quick brown fox jumps over the lazy dog. 0123456789 ÄÖÜ ❤';
    const samples = [14, 20, 32, 48].map((px) => h('div', { class: 'sample', style: `font-family: "${family}", var(--font-ui); font-size: ${px}px` }, sampleText));
    const input = h('input', { class: 'input', type: 'text', value: sampleText, 'aria-label': 'Sample text', spellcheck: 'false' });
    input.addEventListener('input', () => { for (const s of samples) s.textContent = input.value; });
    const status = h('span', {}, '');
    new FontFace(family, entry.data).load().then((face) => { document.fonts.add(face); }).catch(() => {
      status.textContent = 'This font cannot be previewed in the browser.';
    });
    box.append(h('div', { class: 'preview-font' }, input, ...samples),
      h('div', { class: 'preview-meta' }, h('strong', {}, entry.path), h('span', {}, size), status, downloadBtn));
  } else {
    box.append(
      h('div', { class: 'preview-icon' }, icon('binary', 48)),
      h('p', {}, 'This is a binary file and can’t be edited here.'),
      h('div', { class: 'preview-meta' }, h('strong', {}, entry.path), h('span', {}, size), downloadBtn),
    );
  }
}

// ---------------------------------------------------------------------------
// File operations

const INVALID_NAME = /[\\:*?"<>|]/;

function validatePath(path, { allowExisting = null } = {}) {
  if (!path) return 'Please enter a name.';
  if (INVALID_NAME.test(path)) return 'Names cannot contain \\ : * ? " < > |';
  if (path.split('/').some((s) => !s || s === '.' || s === '..')) return 'That is not a valid path.';
  if (state.entries.has(path) && path !== allowExisting) return `“${path}” already exists.`;
  return '';
}

const joinPath = (dir, name) => (dir ? `${dir}/${name}` : name);

async function newFile(dir = '') {
  const defaultName = state.entries.has(joinPath(dir, 'untitled.lua')) ? '' : 'untitled.lua';
  const name = await promptDialog({
    title: 'New file', label: dir ? `In ${dir}/` : 'File name (use / for folders)', value: defaultName, placeholder: 'player.lua', okLabel: 'Create',
    selectBase: true, validate: (v) => validatePath(store.normalizePath(joinPath(dir, v))),
  });
  if (!name) return;
  const path = store.normalizePath(joinPath(dir, name));
  await store.writeFile(state.project.id, path, '');
  await refreshEntries();
  await openFile(path, { focus: true });
}

async function newFolder(dir = '') {
  const name = await promptDialog({
    title: 'New folder', label: dir ? `In ${dir}/` : 'Folder name', placeholder: 'assets', okLabel: 'Create',
    validate: (v) => validatePath(store.normalizePath(joinPath(dir, v))),
  });
  if (!name) return;
  const path = store.normalizePath(joinPath(dir, name));
  await store.createDir(state.project.id, path);
  tree.expanded.add(path);
  tree.reveal(`${path}/x`, false);
  await refreshEntries();
}

function remapPaths(moved) {
  for (const [from, to] of moved) {
    const ti = state.tabs.indexOf(from);
    if (ti >= 0) state.tabs[ti] = to;
    if (state.active === from) state.active = to;
    for (const map of [state.saved, state.errors]) {
      if (map.has(from)) { map.set(to, map.get(from)); map.delete(from); }
    }
    if (state.dirty.delete(from)) state.dirty.add(to);
    editor.rename(from, to);
  }
}

async function renameEntry(path, kind) {
  const name = await promptDialog({
    title: kind === 'dir' ? 'Rename folder' : 'Rename file', label: 'New name', value: baseName(path), okLabel: 'Rename', selectBase: kind !== 'dir',
    validate: (v) => validatePath(store.normalizePath(joinPath(dirName(path), v)), { allowExisting: path }),
  });
  if (!name) return;
  const to = store.normalizePath(joinPath(dirName(path), name));
  if (to === path) return;
  await doMove(path, to);
}

async function moveEntry(from, toDir) {
  const to = joinPath(toDir, baseName(from));
  if (state.entries.has(to)) {
    toast(`“${to}” already exists.`, { type: 'error' });
    return;
  }
  await doMove(from, to);
  if (toDir) tree.toggle(toDir, true);
}

async function doMove(from, to) {
  try {
    const moved = await store.renameEntry(state.project.id, from, to);
    remapPaths(moved);
    await refreshEntries();
    if (state.active) {
      tree.setActive(state.active);
      $('#status-lang').textContent = languageLabel(state.active, state.entries.get(state.active)?.data);
    }
    renderTabs();
    persistTabs();
  } catch (e) {
    toast(e.message, { type: 'error' });
  }
}

async function deleteEntry(path, kind) {
  const isDir = kind === 'dir';
  const inside = isDir ? [...state.entries.values()].filter((e) => e.kind === 'file' && e.path.startsWith(`${path}/`)).length : 0;
  const ok = await confirmDialog({
    title: isDir ? 'Delete folder?' : 'Delete file?',
    message: isDir
      ? `“${path}” and the ${inside} file${inside === 1 ? '' : 's'} in it will be deleted. This cannot be undone.`
      : `“${path}” will be deleted. This cannot be undone.`,
    okLabel: 'Delete', danger: true,
  });
  if (!ok) return;
  const affected = state.tabs.filter((p) => p === path || p.startsWith(`${path}/`));
  const wasActive = affected.includes(state.active);
  for (const p of affected) {
    state.dirty.delete(p);
    state.errors.delete(p);
    state.saved.delete(p);
    editor.close(p);
  }
  state.tabs = state.tabs.filter((p) => !affected.includes(p));
  await store.deleteEntry(state.project.id, path);
  await refreshEntries();
  if (wasActive) {
    state.active = null;
    if (state.tabs.length) await openFile(state.tabs[0]);
    else showEmptyEditor();
  }
  renderTabs();
  persistTabs();
  toast(`Deleted ${baseName(path)}`);
}

async function duplicateFile(path) {
  if (state.dirty.has(path)) await saveFile(path);
  const base = baseName(path);
  const dot = base.lastIndexOf('.');
  const stem = dot > 0 ? base.slice(0, dot) : base;
  const ext = dot > 0 ? base.slice(dot) : '';
  let n = 1;
  let target;
  do { target = joinPath(dirName(path), `${stem} copy${n > 1 ? ` ${n}` : ''}${ext}`); n++; } while (state.entries.has(target));
  await store.writeFile(state.project.id, target, state.entries.get(path).data);
  await refreshEntries();
  await openFile(target);
}

function downloadFile(path) {
  const entry = state.entries.get(path);
  const data = editor.has(path) ? editor.getText(path) : entry.data;
  downloadBlob(new Blob([data], { type: mimeOf(path) }), baseName(path));
}

function showTreeMenu(path, kind, point) {
  const dir = kind === 'dir' ? path : kind === 'file' ? dirName(path) : '';
  const items = [];
  if (kind === 'file') {
    items.push({ label: 'Open', icon: 'file', action: () => openFile(path, { focus: true }) }, '-');
  }
  items.push(
    { label: dir ? 'New file here…' : 'New file…', icon: 'filePlus', action: () => newFile(dir) },
    { label: dir ? 'New folder here…' : 'New folder…', icon: 'folderPlus', action: () => newFolder(dir) },
  );
  if (kind === 'root') items.push({ label: 'Import files…', icon: 'upload', action: () => importTarget('files', '') });
  if (kind !== 'root') {
    items.push('-', { label: 'Rename…', icon: 'pencil', shortcut: 'F2', action: () => renameEntry(path, kind) });
    if (kind === 'file') {
      items.push(
        { label: 'Duplicate', icon: 'copy', action: () => duplicateFile(path) },
        { label: 'Download', icon: 'download', action: () => downloadFile(path) },
      );
    } else {
      items.push({ label: 'Import files here…', icon: 'upload', action: () => importTarget('files', path) });
    }
    items.push('-', { label: 'Delete', icon: 'trash', danger: true, shortcut: 'Del', action: () => deleteEntry(path, kind) });
  }
  showMenu(point, items);
}

// ---------------------------------------------------------------------------
// Import

let importDir = '';

function importTarget(type, dir) {
  importDir = dir;
  if (type === 'files') $('#input-files').click();
  else if (type === 'folder') $('#input-folder').click();
  else $('#input-archive').click();
}

/** items: [{path, file}] */
async function importFiles(items, dir = '') {
  if (!items.length || !state.project) return;
  const files = {};
  try {
    for (const { path, file } of items) {
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (/\.(love|zip)$/i.test(path)) {
        const unzipped = await unzipToProjectFiles(bytes);
        for (const [p, d] of Object.entries(unzipped)) files[store.normalizePath(joinPath(dir, p))] = d;
      } else {
        files[store.normalizePath(joinPath(dir, path))] = classifyBytes(path, bytes);
      }
    }
  } catch (e) {
    toast(`Import failed: ${e.message}`, { type: 'error' });
    return;
  }
  const paths = Object.keys(files);
  if (!paths.length) return;
  const existing = paths.filter((p) => state.entries.has(p));
  if (existing.length) {
    const ok = await confirmDialog({
      title: 'Replace files?',
      message: existing.length === 1
        ? `“${existing[0]}” already exists. Replace it?`
        : `${existing.length} files already exist (${existing.slice(0, 3).join(', ')}${existing.length > 3 ? ', …' : ''}). Replace them?`,
      okLabel: 'Replace', danger: true,
    });
    if (!ok) return;
  }
  await store.writeFiles(state.project.id, files);
  await refreshEntries();
  // Refresh open buffers that were replaced.
  for (const p of existing) {
    if (!editor.has(p)) continue;
    const data = files[p];
    if (typeof data === 'string') {
      editor.setText(p, data);
      state.saved.set(p, data);
      state.dirty.delete(p);
    } else {
      editor.close(p);
      state.saved.delete(p);
    }
  }
  if (state.active && existing.includes(state.active)) await openFile(state.active);
  renderTabs();
  toast(`Imported ${paths.length} file${paths.length === 1 ? '' : 's'}`, { type: 'success' });
  if (paths.length === 1) await openFile(paths[0]);
}

// ---------------------------------------------------------------------------
// Running

async function collectFiles() {
  await saveAll({ quiet: true });
  return store.getAllFiles(state.project.id);
}

/** focus: give the game keyboard focus once it starts (not for auto-run). */
async function run({ focus = true } = {}) {
  if (!state.project) return;
  const files = await collectFiles();
  if (!files['main.lua']) {
    consolePanel.error('There is no main.lua at the root of the project. LÖVE needs one to start the game.');
    toast('Add a main.lua to run the game.', { type: 'error' });
    return;
  }
  if (LS.get('lws.clearOnRun', true)) consolePanel.clear();
  consolePanel.separator(`Run · ${new Date().toLocaleTimeString()}`);
  state.errors.clear();
  editor.clearErrors();
  state.lastError = false;
  state.hasRun = true;
  renderTabs();
  $('#game-title').textContent = state.project.name;
  runner.run({ files }, { focus });
}

function stop() {
  if (runner.active) runner.stop();
}

function togglePause() {
  if (runner.state === 'running' || runner.state === 'paused') runner.setPaused(runner.state !== 'paused');
}

function onRunnerState(s) {
  const active = s === 'loading' || s === 'running' || s === 'paused' || s === 'crashed';
  $('#btn-stop').disabled = !active;
  $('#btn-restart').disabled = !active && !state.hasRun;
  $('#btn-pause').disabled = !(s === 'running' || s === 'paused');
  $('#btn-pause').setAttribute('aria-pressed', String(s === 'paused'));
  $('#btn-pause').title = s === 'paused' ? 'Resume (F8)' : 'Pause (F8)';
  $('#btn-pause').setAttribute('aria-label', s === 'paused' ? 'Resume' : 'Pause');
  $('#btn-pause').replaceChildren(icon(s === 'paused' ? 'play' : 'pause'));
  $('#btn-run').classList.toggle('running', active);
  $('#btn-run').title = active ? 'Save and restart (F5 or Ctrl+Enter)' : 'Save and run (F5 or Ctrl+Enter)';
  $('#game-empty').hidden = active;
  $('#game-loading').hidden = s !== 'loading';
  $('#game-paused').hidden = s !== 'paused';
  $('#game-empty-text').replaceChildren(...(s === 'stopped'
    ? ['Game stopped. Press ', h('kbd', {}, 'F5'), ' to run it again']
    : ['Press ', h('kbd', {}, 'F5'), ' to run your game']));
  const dot = $('#game-status');
  dot.className = `status-dot ${(state.lastError && active) || s === 'crashed' ? 'error' : s}`;
  $('#game-state-label').textContent = {
    loading: 'Starting…', running: state.lastError ? 'Error' : 'Running', paused: 'Paused', stopped: 'Stopped', crashed: 'Engine failed',
  }[s] || '';
  if (!active && s !== 'idle') $('#game-title').textContent = state.project?.name || 'Game';
}

function onGameError(text) {
  consolePanel.error(text);
  state.lastError = true;
  onRunnerState(runner.state);
  const loc = findLocation(text, (p) => state.entries.get(p)?.kind === 'file');
  if (loc) {
    const message = text.split('\n')[0];
    state.errors.set(loc.path, { line: loc.line, message });
    if (editor.has(loc.path)) editor.setError(loc.path, loc.line, message);
    renderTabs();
  }
}

async function popOutGame() {
  const win = openPopup();
  if (!win) {
    toast('The browser blocked the new window. Allow pop-ups for this site and try again.', { type: 'error', timeout: 6000 });
    return;
  }
  try {
    const files = await collectFiles();
    showInPopup(win, await buildLove(files), state.project.name);
  } catch (e) {
    win.close();
    toast(`Could not open the game: ${e.message}`, { type: 'error' });
  }
}

function toggleGameFullscreen() {
  const stage = $('#game-stage');
  if (document.fullscreenElement) document.exitFullscreen();
  else stage.requestFullscreen?.().then(() => runner.focus()).catch(() => toast('Fullscreen is not available here.', { type: 'error' }));
}

// ---------------------------------------------------------------------------
// Export & share

async function exportLove() {
  try {
    const bytes = await buildLove(await collectFiles());
    downloadBlob(new Blob([bytes], { type: 'application/zip' }), `${slugify(state.project.name)}.love`);
    toast(`Exported ${slugify(state.project.name)}.love (${formatBytes(bytes.length)})`, { type: 'success' });
  } catch (e) {
    toast(`Export failed: ${e.message}`, { type: 'error' });
  }
}

async function exportWeb() {
  flashStatus('Building web export…');
  try {
    const bytes = await buildWebExport(await collectFiles(), state.project.name);
    downloadBlob(new Blob([bytes], { type: 'application/zip' }), `${slugify(state.project.name)}-web.zip`);
    toast(`Exported ${slugify(state.project.name)}-web.zip (${formatBytes(bytes.length)}). Upload it to itch.io as an HTML game.`, { type: 'success', timeout: 6000 });
  } catch (e) {
    toast(`Web export failed: ${e.message}`, { type: 'error' });
  }
}

async function share() {
  let link;
  try {
    link = await createShareLink(state.project.name, await collectFiles());
  } catch (e) {
    await openDialog({
      title: 'Can’t create a share link',
      body: h('p', { class: 'dialog-message' }, e.message),
      actions: [
        { label: 'Export .love', value: 'love', ghost: true, left: true },
        { label: 'OK', value: null, primary: true, submit: true },
      ],
    }).then((r) => r === 'love' && exportLove());
    return;
  }
  let copied = false;
  try {
    await navigator.clipboard.writeText(link.url);
    copied = true;
  } catch { /* show the dialog instead */ }
  const input = h('input', { class: 'input', type: 'text', value: link.url, readonly: true, 'aria-label': 'Share link' });
  const copyBtn = h('button', {
    type: 'button', class: 'btn primary',
    onclick: async () => {
      input.select();
      try { await navigator.clipboard.writeText(link.url); } catch { document.execCommand('copy'); }
      toast('Link copied', { type: 'success' });
    },
  }, icon('copy'), 'Copy');
  await openDialog({
    title: 'Share link',
    body: h('div', { class: 'field' },
      h('p', { class: 'dialog-message' }, `${copied ? 'Copied to your clipboard. ' : ''}Anyone with this link gets their own copy of “${state.project.name}” (${formatBytes(link.size)} compressed, stored entirely in the link).`),
      h('div', { class: 'share-box' }, input, copyBtn)),
    actions: [{ label: 'Done', value: null, primary: true, submit: true }],
    onOpen: () => input.select(),
  });
}

function showExportMenu() {
  showMenu($('#btn-export'), [
    { heading: 'Export' },
    { label: 'Download .love', icon: 'package', action: exportLove },
    { label: 'Export for web (.zip for itch.io)', icon: 'globe', action: exportWeb },
    '-',
    { label: 'Open game in new window', icon: 'popout', action: popOutGame },
    { label: 'Copy share link', icon: 'link', action: share },
  ]);
}

function showImportMenu() {
  showMenu($('#btn-import'), [
    { label: 'Import files…', icon: 'upload', action: () => importTarget('files', '') },
    { label: 'Import folder…', icon: 'folderUp', action: () => importTarget('folder', '') },
    { label: 'Unpack .love / .zip here…', icon: 'archive', action: () => importTarget('archive', '') },
    '-',
    { label: 'Open .love / .zip as new project…', icon: 'package', action: () => $('#input-open-project').click() },
  ]);
}

// ---------------------------------------------------------------------------
// Dialogs: shortcuts, quick open

function showShortcuts() {
  const mod = isMac ? '⌘' : 'Ctrl';
  const alt = isMac ? '⌥' : 'Alt';
  const groups = [
    ['Run', [
      ['Save and run', ['F5'], [mod, 'Enter']], ['Stop', ['Shift', 'F5']], ['Restart', [mod, 'Shift', 'Enter']], ['Pause / resume', ['F8']],
    ]],
    ['Files', [
      ['Save', [mod, 'S']], ['Save all', [mod, 'Shift', 'S']], ['Quick open', [mod, 'P']], ['Projects', [mod, 'O']],
      ['New file', [alt, 'N']], ['Close tab', [alt, 'W']], ['Next / previous tab', [alt, ']'], [alt, '[']], ['Toggle file panel', [mod, 'B']],
    ]],
    ['Editing', [
      ['Autocomplete', ['Ctrl', 'Space']], ['Find', [mod, 'F']], ['Find and replace', [mod, 'H']], ['Go to line', [mod, 'G']],
      ['Toggle comment', [mod, '/']], ['Indent / outdent', ['Tab'], ['Shift', 'Tab']], ['Select next match', [mod, 'D']], ['Undo / redo', [mod, 'Z'], [mod, 'Shift', 'Z']],
    ]],
    ['Help', [['Keyboard shortcuts', ['F1']]]],
  ];
  const body = h('div', { class: 'shortcuts' });
  for (const [title, rows] of groups) {
    body.append(h('h3', {}, title));
    for (const [label, ...combos] of rows) {
      const keys = h('span', {});
      combos.forEach((combo, i) => {
        if (i) keys.append(' ');
        for (const k of combo) keys.append(h('kbd', {}, k));
      });
      body.append(h('div', { class: 'shortcut' }, h('span', {}, label), keys));
    }
  }
  openDialog({ title: 'Keyboard shortcuts', body, wide: true });
}

function fuzzyScore(query, text) {
  if (!query) return { score: 0, marks: [] };
  const q = query.toLowerCase();
  const t = text.toLowerCase();
  const marks = [];
  let ti = 0;
  let score = 0;
  for (const ch of q) {
    const i = t.indexOf(ch, ti);
    if (i < 0) return null;
    score += i === ti ? 3 : 1;
    if (i === 0 || '/._- '.includes(t[i - 1])) score += 2;
    marks.push(i);
    ti = i + 1;
  }
  return { score: score - t.length * 0.05, marks };
}

function quickOpen() {
  const files = [...state.entries.values()].filter((e) => e.kind === 'file').map((e) => e.path);
  const input = h('input', { class: 'input', type: 'text', placeholder: 'Go to file…', 'aria-label': 'File name', spellcheck: 'false', autocomplete: 'off' });
  const listEl = h('div', { class: 'quick-list', role: 'listbox' });
  let results = [];
  let sel = 0;
  let finish = null;
  const render = () => {
    const q = input.value.trim();
    results = files.map((p) => ({ p, r: fuzzyScore(q, p) })).filter((x) => x.r).sort((a, b) => b.r.score - a.r.score || a.p.localeCompare(b.p)).slice(0, 50);
    sel = Math.min(sel, Math.max(0, results.length - 1));
    listEl.replaceChildren(...results.map(({ p, r }, i) => {
      const name = h('span', {});
      [...p].forEach((ch, ci) => name.append(r.marks.includes(ci) ? h('mark', {}, ch) : ch));
      const item = h('div', { class: 'quick-item', role: 'option', 'aria-selected': String(i === sel), onclick: () => choose(i) }, icon(iconFor(p, state.entries.get(p).data), 14), name);
      return item;
    }));
    if (!results.length) listEl.append(h('div', { class: 'quick-item' }, 'No matching files'));
    listEl.querySelector('[aria-selected="true"]')?.scrollIntoView({ block: 'nearest' });
  };
  const choose = (i) => {
    const r = results[i];
    if (!r) return;
    finish?.(null);
    openFile(r.p, { focus: true });
  };
  input.addEventListener('input', () => { sel = 0; render(); });
  input.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown') { e.preventDefault(); sel = Math.min(results.length - 1, sel + 1); render(); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); sel = Math.max(0, sel - 1); render(); }
    else if (e.key === 'Enter') { e.preventDefault(); choose(sel); }
  });
  render();
  openDialog({ title: 'Go to file', body: h('div', {}, input, listEl), className: 'quick-open', onOpen: (dlg, f) => { finish = f; input.focus(); } });
}

// ---------------------------------------------------------------------------
// Shell: theme, sidebar, autorun, keyboard, drag & drop

function updateThemeButton() {
  const dark = document.documentElement.dataset.theme !== 'light';
  const btn = $('#btn-theme');
  btn.replaceChildren(icon(dark ? 'sun' : 'moon'));
  btn.title = dark ? 'Switch to light theme' : 'Switch to dark theme';
}

function toggleTheme() {
  const next = document.documentElement.dataset.theme === 'light' ? 'dark' : 'light';
  document.documentElement.dataset.theme = next;
  try { localStorage.setItem('lws.theme', next); } catch { /* ignore */ }
  updateThemeButton();
}

function setSidebarVisible(visible, persist = true) {
  $('#app').classList.toggle('sidebar-hidden', !visible);
  $('#btn-sidebar').setAttribute('aria-pressed', String(visible));
  if (persist) LS.set('lws.sidebar', visible);
}

function setAutorun(on) {
  state.autorun = on;
  $('#btn-autorun').setAttribute('aria-pressed', String(on));
  LS.set('lws.autorun', on);
}

function cycleTab(dir) {
  if (state.tabs.length < 2) return;
  const i = state.tabs.indexOf(state.active);
  openFile(state.tabs[(i + dir + state.tabs.length) % state.tabs.length]);
}

/** Returns true when the event was a shortcut (and has been handled). */
function handleShortcut(e, fromGame = false) {
  const mod = isMac ? e.metaKey : e.ctrlKey;
  const key = e.key;
  const lower = key.length === 1 ? key.toLowerCase() : key;
  if (document.querySelector('dialog[open]')) return key === 'F5' && !fromGame; // just block reloads

  if (key === 'F5' && !mod) { if (e.shiftKey) stop(); else run(); return true; }
  if (mod && key === 'Enter') { run(); return true; }
  if (key === 'F8') { togglePause(); return true; }
  if (mod && lower === 's') { if (e.shiftKey) saveAll(); else saveActive(); return true; }
  if (fromGame) return false;

  if (key === 'F1') { showShortcuts(); return true; }
  if (mod && !e.shiftKey && lower === 'p') { quickOpen(); return true; }
  if (mod && !e.shiftKey && lower === 'o') { openProjectsDialog(); return true; }
  if (mod && !e.shiftKey && lower === 'b') { setSidebarVisible($('#app').classList.contains('sidebar-hidden')); return true; }
  if (e.altKey && !mod && e.code === 'KeyW') { closeTab(state.active); return true; }
  if (e.altKey && !mod && e.code === 'KeyN') { newFile(''); return true; }
  if (e.altKey && !mod && e.code === 'BracketRight') { cycleTab(1); return true; }
  if (e.altKey && !mod && e.code === 'BracketLeft') { cycleTab(-1); return true; }
  return false;
}

function wireUi() {
  window.addEventListener('keydown', (e) => {
    if (handleShortcut(e)) {
      e.preventDefault();
      e.stopPropagation();
    }
  }, true);

  $('#btn-run').addEventListener('click', () => run());
  $('#btn-run-big').addEventListener('click', () => run());
  $('#btn-stop').addEventListener('click', stop);
  $('#btn-restart').addEventListener('click', () => run());
  $('#btn-pause').addEventListener('click', togglePause);
  $('#btn-autorun').addEventListener('click', () => {
    setAutorun(!state.autorun);
    toast(state.autorun ? 'Auto-run on: the game restarts every time you save.' : 'Auto-run off.');
  });
  $('#btn-project').addEventListener('click', openProjectsDialog);
  $('#btn-share').addEventListener('click', share);
  $('#btn-export').addEventListener('click', showExportMenu);
  $('#btn-theme').addEventListener('click', toggleTheme);
  $('#btn-help').addEventListener('click', showShortcuts);
  $('#btn-sidebar').addEventListener('click', () => setSidebarVisible($('#app').classList.contains('sidebar-hidden')));
  $('#btn-new-file').addEventListener('click', () => newFile(''));
  $('#btn-new-folder').addEventListener('click', () => newFolder(''));
  $('#btn-import').addEventListener('click', showImportMenu);
  $('#btn-game-fullscreen').addEventListener('click', toggleGameFullscreen);
  $('#btn-popout').addEventListener('click', popOutGame);
  document.addEventListener('fullscreenchange', () => { if (!document.fullscreenElement) runner.focus(); });

  const pick = (input, fn) => input.addEventListener('change', async () => {
    const files = [...input.files];
    input.value = '';
    if (files.length) await fn(files);
  });
  pick($('#input-files'), (files) => importFiles(files.map((file) => ({ path: file.name, file })), importDir));
  pick($('#input-folder'), (files) => importFiles(files.map((file) => ({ path: file.webkitRelativePath || file.name, file })), importDir));
  pick($('#input-archive'), (files) => importFiles(files.map((file) => ({ path: file.name, file })), importDir));
  pick($('#input-open-project'), (files) => openArchiveAsProject(files[0]));

  // Drag & drop anywhere (the file tree handles drops on itself).
  let depth = 0;
  const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes('Files');
  const overlay = $('#drop-overlay');
  const endDrag = () => {
    depth = 0;
    overlay.hidden = true;
    document.body.classList.remove('dragging-files');
    $('#app').classList.remove('resizing');
  };
  window.addEventListener('dragenter', (e) => {
    if (!hasFiles(e)) return;
    depth++;
    overlay.hidden = false;
    // Keep drops away from the game iframe and the code editor.
    $('#app').classList.add('resizing');
  });
  window.addEventListener('dragleave', () => {
    depth = Math.max(0, depth - 1);
    if (!depth) endDrag();
  });
  window.addEventListener('dragover', (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
  });
  window.addEventListener('drop', async (e) => {
    if (!hasFiles(e)) return;
    if (e.target.closest?.('#file-tree')) { endDrag(); return; } // the tree handles it
    e.preventDefault();
    e.stopPropagation();
    endDrag();
    const items = await readDataTransfer(e.dataTransfer);
    const archives = items.filter((i) => /\.(love|zip)$/i.test(i.path));
    if (items.length === 1 && archives.length === 1 && e.target.closest?.('#game-panel')) {
      await openArchiveAsProject(archives[0].file);
      return;
    }
    await importFiles(items, '');
  }, true);

  window.addEventListener('beforeunload', (e) => {
    if (state.dirty.size) {
      e.preventDefault();
      e.returnValue = '';
    }
  });

  // Remember open tabs when the page goes away.
  window.addEventListener('pagehide', () => {
    if (state.project) store.updateProject(state.project.id, { tabs: [...state.tabs], active: state.active }).catch(() => {});
  });
}

// For debugging and automated checks.
window.studio = {
  state,
  store,
  get editor() { return editor; },
  get runner() { return runner; },
  run, stop, openFile, openProject, saveAll, collectFiles, buildLove, buildWebExport, createShareLink, unzipToProjectFiles,
  createFromTemplate, loadExampleIndex, importFiles,
};

init().catch((e) => {
  console.error(e);
  $('#app').classList.remove('booting');
  toast(`Smitten failed to start: ${e.message}`, { type: 'error', timeout: 10000 });
});
