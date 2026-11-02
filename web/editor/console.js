// The console panel: game output, errors with clickable file:line links,
// filtering and auto-scroll.

import { h, icon } from './ui.js';

const MAX_LINES = 5000;
const LOCATION_RE = /([\w\-./]+\.lua):(\d+)/g;

/** First "file.lua:line" in text that refers to a project file. */
export function findLocation(text, fileExists) {
  for (const m of text.matchAll(LOCATION_RE)) {
    const path = m[1].replace(/^\.\//, '');
    if (fileExists(path)) return { path, line: Number(m[2]) };
  }
  return null;
}

export class ConsolePanel {
  /**
   * root: the #console-output element; controls: {filter, level, autoscroll, clear, badge}
   * opts: {fileExists(path), onOpenLocation(path, line)}
   */
  constructor(root, controls, opts) {
    this.root = root;
    this.controls = controls;
    this.opts = opts;
    this.count = 0;
    this.errors = 0;
    this.autoscroll = true;
    this.filterText = '';
    this.level = 'all';
    this.inError = false; // swallowing log lines that belong to an error report

    controls.filter.addEventListener('input', () => {
      this.filterText = controls.filter.value.toLowerCase();
      this.applyFilter();
    });
    controls.level.addEventListener('change', () => {
      this.level = controls.level.value;
      this.applyFilter();
    });
    controls.autoscroll.addEventListener('click', () => this.setAutoscroll(!this.autoscroll));
    controls.clear.addEventListener('click', () => this.clear());
    root.addEventListener('scroll', () => {
      const atBottom = root.scrollHeight - root.scrollTop - root.clientHeight < 24;
      if (!atBottom && this.autoscroll && !this.programmaticScroll) this.setAutoscroll(false);
      else if (atBottom && !this.autoscroll) this.setAutoscroll(true);
    });
    root.addEventListener('click', (e) => {
      const link = e.target.closest('.loc-link');
      if (link) {
        e.preventDefault();
        this.opts.onOpenLocation?.(link.dataset.path, Number(link.dataset.line));
      }
    });
    this.renderEmpty();
  }

  setAutoscroll(on) {
    this.autoscroll = on;
    this.controls.autoscroll.setAttribute('aria-pressed', String(on));
    if (on) this.scrollToEnd();
  }

  scrollToEnd() {
    this.programmaticScroll = true;
    this.root.scrollTop = this.root.scrollHeight;
    requestAnimationFrame(() => { this.programmaticScroll = false; });
  }

  renderEmpty() {
    if (this.count) return;
    this.root.replaceChildren(h('div', { class: 'console-empty' }, icon('terminal', 20), h('span', {}, 'Output from print() and errors will appear here.')));
  }

  /** Text with project file locations turned into links. */
  linkify(text) {
    const frag = document.createDocumentFragment();
    let last = 0;
    for (const m of text.matchAll(LOCATION_RE)) {
      const path = m[1].replace(/^\.\//, '');
      if (!this.opts.fileExists(path)) continue;
      frag.append(text.slice(last, m.index));
      frag.append(h('a', { href: '#', class: 'loc-link', dataset: { path, line: m[2] }, title: `Open ${path} at line ${m[2]}` }, m[0]));
      last = m.index + m[0].length;
    }
    frag.append(text.slice(last));
    return frag;
  }

  append(el) {
    if (!this.count) this.root.replaceChildren();
    this.count++;
    this.root.append(el);
    while (this.root.childElementCount > MAX_LINES) this.root.firstElementChild.remove();
    this.filterOne(el);
    if (this.autoscroll) this.scrollToEnd();
  }

  /** One line of game output. level: 'out' | 'err' */
  log(level, text) {
    // LÖVE's error handler prints the error and traceback; the player sends
    // the same text as one 'error' message, which we show as a block instead.
    if (text.startsWith('Error: ')) { this.inError = true; return; }
    if (this.inError && (text.startsWith('\t') || text.startsWith('stack traceback'))) return;
    this.inError = false;
    this.append(h('div', { class: `line ${level}`, dataset: { level } }, this.linkify(text)));
  }

  error(text) {
    this.inError = false;
    this.errors++;
    this.updateBadge();
    const [first, ...rest] = text.split('\n');
    const block = h('div', { class: 'line error-block', dataset: { level: 'err' } },
      h('div', { class: 'error-title' }, icon('alert', 14), h('span', {}, this.linkify(first))),
      rest.length ? h('pre', { class: 'error-trace' }, this.linkify(rest.join('\n'))) : null);
    this.append(block);
  }

  info(text, kind = 'info') {
    this.append(h('div', { class: `line meta ${kind}`, dataset: { level: 'meta' } }, text));
  }

  separator(text) {
    this.append(h('div', { class: 'line separator', dataset: { level: 'meta' } }, h('span', {}, text)));
  }

  updateBadge() {
    const b = this.controls.badge;
    b.hidden = !this.errors;
    b.textContent = this.errors > 99 ? '99+' : String(this.errors);
    b.title = `${this.errors} error${this.errors === 1 ? '' : 's'}`;
  }

  clear() {
    this.count = 0;
    this.errors = 0;
    this.inError = false;
    this.updateBadge();
    this.renderEmpty();
  }

  filterOne(el) {
    const lvl = el.dataset.level;
    let show = true;
    if (this.level === 'out') show = lvl === 'out' || lvl === 'meta';
    else if (this.level === 'err') show = lvl === 'err';
    if (show && this.filterText) show = el.textContent.toLowerCase().includes(this.filterText);
    el.hidden = !show;
  }

  applyFilter() {
    if (!this.count) return;
    for (const el of this.root.children) this.filterOne(el);
    if (this.autoscroll) this.scrollToEnd();
  }

}
