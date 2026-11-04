#!/usr/bin/env node
// Rule 3 check: a plugin never reads a game-class member directly.
//
//   node rule3-check.mjs --plugin <name> --sdk <Game-SDK Client\SDK dir> --source <dir>...
//        [--allow <rule3-allow.json>] [--mode auto|enforce|report] [--json <out.json>]
//
// Every game value goes through the loader by name (SR_FIELD, SR_BITFIELD,
// SR_SIZE_CHECK, native rows). This flags, outside the loader's lookup helpers:
//   member    `->m` or `.m` where the member is declared on a game SDK class or struct
//   suspect   `->m` or `.m` on a receiver this scan could not type, where `m` is
//             declared only on game classes (a call result, an `auto` it could not follow)
//   sizeof    sizeof or alignof of a game type, and offsetof(game type, m)
//   stride    TArray, TSet, TMap or TSparseArray of a game struct held by value
//   byvalue   a game struct held, copied or passed by value
//   overlay   reinterpret_cast or a C cast of memory to a struct the plugin declares itself
//   offset    pointer arithmetic with a hex constant, a hard address, a named offset constant
// sizeof, stride and byvalue of a struct are not flagged when the plugin lists
// SR_SIZE_CHECK(thatStruct): the loader checks the size on the running build.
//
// How it knows what is a game type: it indexes the Game-SDK's Dumper-7 headers
// (members with their types, methods with their return types, base classes,
// package). A class is an engine class only when its package is on the
// allow-list's enginePackages; every other package is the game's, so a package
// nobody reviewed is flagged, not passed. A member is looked up through the
// base classes and judged by the class that declares it. The scan then types
// each receiver from declarations in scope, member types, method return types,
// casts, `auto` initialisers and range-for, and follows the chain.
//
// Limits (a source scan, not a compiler): macros are not expanded, an `auto`
// from a function it cannot see is untyped, templates are not instantiated. An
// untyped receiver is only caught by the `suspect` rule (name declared only on
// game classes). The hex-arithmetic rule is a heuristic keyed on pointer-ish
// statements. Per-site exceptions go in the allow-list with a reason.
//
// --mode: enforce fails on any finding; report prints and passes; auto (default)
// enforces when the plugin's sources set PLUGIN_MAP_MIGRATED, else reports.
// Output lines use the MSBuild/Visual Studio error format. Exit: 0 pass,
// 3 findings under enforce, 2 usage or unreadable input.

import { readFileSync, readdirSync, statSync, writeFileSync, existsSync } from 'node:fs';
import { join, relative, sep, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

export const EXIT_OK = 0;
export const EXIT_USAGE = 2;
export const EXIT_VIOLATION = 3;

const SOURCE_EXT = /\.(cpp|cc|cxx|c|h|hpp|hxx|inl)$/i;
const SKIP_DIRS = new Set(['.git', 'build', 'obj', 'x64', 'node_modules', 'StarRupture SDK', 'game-sdk', 'plugin-sdk', 'StarRupture-ImGui', 'Tests', 'tests', 'out', 'bin', '.vs']);

const NOT_NAMES = new Set(['return', 'new', 'delete', 'if', 'else', 'for', 'while', 'switch', 'case', 'const', 'static', 'constexpr', 'auto', 'sizeof', 'alignof', 'offsetof', 'typedef', 'using', 'operator', 'throw', 'do', 'goto', 'break', 'continue', 'template', 'typename', 'class', 'struct', 'enum', 'union', 'namespace', 'public', 'private', 'protected', 'virtual', 'override', 'final', 'noexcept', 'this', 'true', 'false', 'nullptr', 'default', 'volatile', 'inline', 'mutable', 'extern', 'friend', 'explicit', 'else', 'and', 'or', 'not', 'co_return', 'co_await', 'decltype']);
const QUAL = new Set(['const', 'static', 'constexpr', 'volatile', 'inline', 'mutable', 'struct', 'class', 'enum', 'typename', 'extern', 'thread_local', 'register']);
// Pointer-like wrappers that name the type they hold.
const UNWRAP = new Set(['TObjectPtr', 'TWeakObjectPtr', 'TSharedPtr', 'TUniquePtr', 'TSharedRef', 'TLazyObjectPtr', 'TStrongObjectPtr', 'TNonNullPtr']);
const CONTAINERS = new Set(['TArray', 'TSet', 'TSparseArray', 'TMap']);
const ELEMENT_CONTAINERS = new Set(['TArray', 'TSet', 'TSparseArray']);
const CASTS = new Set(['static_cast', 'reinterpret_cast', 'const_cast', 'dynamic_cast']);
// `Name<T>(...)` that returns a T.
const GENERIC_RETURNS = new Set(['Cast', 'CastChecked', 'FindObject', 'FindObjectFast', 'FindObjectImpl', 'GetDefaultObjImpl', 'GetObjectOfClass']);
const WRAPPER_NAMES = new Set([...UNWRAP, ...CONTAINERS, 'TSubclassOf', 'PluginField', 'auto']);
const POINTERISH = new Set(['reinterpret_cast', 'uintptr_t', 'intptr_t', 'GetModuleHandleA', 'GetModuleHandleW', 'GetModuleHandle', 'DWORD64', 'SIZE_T']);
const POINTER_TYPES = new Set(['uint8_t', 'uint8', 'char', 'byte', 'BYTE', 'void', 'uintptr_t', 'intptr_t']);

// ---- source text ---------------------------------------------------------

// Comments, string and char literals, and preprocessor lines become spaces
// (newlines kept, so lines and offsets still match).
export function stripNonCode(src) {
  let out = '';
  const n = src.length;
  let lineStart = true;
  for (let i = 0; i < n; ) {
    const c = src[i];
    const d = src[i + 1];
    if (c === '\n') { out += c; i++; lineStart = true; continue; }
    if (lineStart && (c === ' ' || c === '\t' || c === '\r')) { out += c; i++; continue; }
    if (lineStart && c === '#') {
      // A preprocessor line, with its backslash continuations.
      while (i < n) {
        if (src[i] === '\n') { if (src[i - 1] === '\\' || (src[i - 1] === '\r' && src[i - 2] === '\\')) { out += '\n'; i++; continue; } break; }
        out += ' '; i++;
      }
      continue;
    }
    lineStart = false;
    if (c === '/' && d === '/') {
      while (i < n && src[i] !== '\n') { out += ' '; i++; }
    } else if (c === '/' && d === '*') {
      const end = src.indexOf('*/', i + 2);
      const stop = end < 0 ? n : end + 2;
      for (; i < stop; i++) out += src[i] === '\n' ? '\n' : ' ';
    } else if (c === 'R' && d === '"' && /[^A-Za-z0-9_]/.test(src[i - 1] ?? ' ')) {
      const open = src.indexOf('(', i + 2);
      const delim = open < 0 ? '' : src.slice(i + 2, open);
      const end = open < 0 ? -1 : src.indexOf(')' + delim + '"', open);
      const stop = end < 0 ? n : end + delim.length + 2;
      for (; i < stop; i++) out += src[i] === '\n' ? '\n' : ' ';
    } else if (c === '"' || (c === "'" && !/[0-9a-fA-F]/.test(src[i - 1] ?? ' '))) {
      out += ' '; i++;
      while (i < n && src[i] !== c && src[i] !== '\n') {
        if (src[i] === '\\') { out += ' '; i++; }
        out += ' '; i++;
      }
      if (i < n && src[i] === c) { out += ' '; i++; }
    } else {
      out += c; i++;
    }
  }
  return out;
}

const TOKEN = /0[xX][0-9a-fA-F']+[uUlL]*|\d[\w.']*|[A-Za-z_]\w*|->|::|\.\.\.|\+\+|--|[^\s]/g;

export function tokenize(code) {
  const text = [];
  const line = [];
  let ln = 1;
  let last = 0;
  TOKEN.lastIndex = 0;
  let m;
  while ((m = TOKEN.exec(code))) {
    for (let k = last; k < m.index; k++) if (code.charCodeAt(k) === 10) ln++;
    last = m.index;
    text.push(m[0]);
    line.push(ln);
  }
  return { text, line };
}

const isIdent = (t) => t !== undefined && /^[A-Za-z_]/.test(t);
const isName = (t) => isIdent(t) && !NOT_NAMES.has(t);
const isNumber = (t) => t !== undefined && /^\d/.test(t);

// ---- the Game-SDK index --------------------------------------------------

const TYPE_HEAD = /^(class|struct)\s+(?:alignas\(\w+\)\s+)?(\w+)(?:\s+final)?(?:\s*:\s*(?:public\s+)?(?:class\s+|struct\s+)?([\w:]+))?\s*$/;
const MEMBER_LINE = /^\t(\S.*?)\s+(\w+)(?:\[[^\]]*\])?\s*(?::\s*\d+)?;\s*\/\/\s*0x[0-9A-Fa-f]+\(0x/;
const MEMBER_NAME = /^\t\S.*?\s+(\w+)(?:\[[^\]]*\])?\s*(?::\s*\d+)?;\s*\/\/\s*0x[0-9A-Fa-f]+\(0x/gm;
const METHOD_LINE = /^\t(?:static\s+|virtual\s+)*(\S.*?)\s+(\w+)\s*\(.*\)\s*(?:const\s*)?(?:;|override;|$)/;

// Reads every *_classes.hpp and *_structs.hpp of a Dumper-7 SDK. `extraNames` on
// the result are identifiers that belong to no indexed game class: the function
// parameter structs' members and everything in Basic.hpp and UnrealContainers.hpp.
export function loadSdkIndex(sdkDir) {
  const types = new Map();
  const extraNames = new Set();
  const addIdentifiers = (file) => {
    if (existsSync(file)) for (const m of readFileSync(file, 'utf8').matchAll(/[A-Za-z_]\w*/g)) extraNames.add(m[0]);
  };
  addIdentifiers(join(sdkDir, 'Basic.hpp'));
  addIdentifiers(join(sdkDir, '..', 'UnrealContainers.hpp'));
  for (const f of readdirSync(sdkDir).filter((f) => /_parameters\.hpp$/.test(f))) {
    for (const m of readFileSync(join(sdkDir, f), 'utf8').matchAll(MEMBER_NAME)) extraNames.add(m[1]);
  }
  const files = readdirSync(sdkDir).filter((f) => /_(classes|structs)\.hpp$/.test(f));
  for (const f of files) {
    const pkg = f.replace(/_(classes|structs)\.hpp$/, '');
    const lines = readFileSync(join(sdkDir, f), 'utf8').split('\n');
    let cur = null;
    for (const raw of lines) {
      const ln = raw.replace(/\r$/, '');
      if (!cur) {
        const h = TYPE_HEAD.exec(ln);
        if (h) cur = { name: h[2], kind: h[1], base: h[3] ? h[3].replace(/^.*::/, '') : null, pkg, members: new Map(), methods: new Map() };
        continue;
      }
      if (ln.startsWith('};')) {
        if (!types.has(cur.name)) types.set(cur.name, cur); // first definition wins (NameCollisions)
        cur = null;
        continue;
      }
      let m = MEMBER_LINE.exec(ln);
      if (m) { cur.members.set(m[2], m[1].trim()); continue; }
      m = METHOD_LINE.exec(ln);
      if (m && !/^(return|if|for|while)\b/.test(m[1])) cur.methods.set(m[2], m[1].trim());
    }
  }
  types.extraNames = extraNames;
  return types;
}

// ---- allow-list ----------------------------------------------------------

export function loadAllow(file) {
  const a = JSON.parse(readFileSync(file, 'utf8'));
  const problems = [];
  const engine = new Set();
  for (const g of a.enginePackages ?? []) {
    if (!g.reason || !String(g.reason).trim()) problems.push(`enginePackages entry ${JSON.stringify(g.names)} has no reason`);
    for (const n of g.names ?? []) engine.add(n);
  }
  const gamePackages = (a.gamePackages ?? []).map((g) => {
    if (!g.reason || !String(g.reason).trim()) problems.push(`gamePackages entry ${g.pattern} has no reason`);
    return new RegExp(g.pattern);
  });
  const exceptions = a.exceptions ?? [];
  for (const e of exceptions) if (!e.reason || !String(e.reason).trim()) problems.push(`exception ${JSON.stringify(e)} has no reason`);
  if (problems.length) throw new Error(`${file}: ${problems.join('; ')}`);
  return { enginePackages: engine, gamePackages, exceptions };
}

// ---- type text -----------------------------------------------------------

// Parses a type at tokens[i]. Returns { type: { n, a, ind }, next } or null.
// n: the last name of a qualified name; a: template arguments; ind: any * or &.
function parseType(T, i, depth = 0) {
  let p = i;
  while (QUAL.has(T[p])) p++;
  if (!isIdent(T[p]) || (NOT_NAMES.has(T[p]) && T[p] !== 'auto')) return null;
  let n = T[p++];
  while (T[p] === '::' && isIdent(T[p + 1])) { n = T[p + 1]; p += 2; }
  let a = [];
  if (T[p] === '<' && (CONTAINERS.has(n) || UNWRAP.has(n) || n === 'TSubclassOf' || n === 'PluginField') && depth < 6) {
    const r = parseTypeArgs(T, p, depth);
    if (!r) return null;
    a = r.args;
    p = r.next;
  }
  let ind = false;
  while (T[p] === '*' || T[p] === '&' || T[p] === 'const' || T[p] === '&&') { if (T[p] !== 'const') ind = true; p++; }
  return { type: { n, a, ind }, next: p };
}

// tokens[p] is `<`; returns the parsed arguments and the index after `>`.
function parseTypeArgs(T, p, depth) {
  const args = [];
  p++;
  for (;;) {
    const r = parseType(T, p, depth + 1);
    if (!r) return null;
    args.push(r.type);
    p = r.next;
    if (T[p] === ',') { p++; continue; }
    if (T[p] === '>') return { args, next: p + 1 };
    return null;
  }
}

// Strips pointer-like wrappers: TObjectPtr<X> is X.
function unwrap(t) {
  while (t && UNWRAP.has(t.n) && t.a.length) t = { ...t.a[0], ind: true };
  if (t && t.n === 'TSubclassOf') return { n: 'UClass', a: [], ind: true };
  return t ?? null;
}

const textTypeCache = new Map();
function typeFromText(text) {
  if (textTypeCache.has(text)) return textTypeCache.get(text);
  const code = text.replace(/\b(class|struct|enum)\b/g, ' ');
  const { text: T } = tokenize(code);
  const r = parseType(T, 0);
  const t = r ? unwrap(r.type) : null;
  textTypeCache.set(text, t);
  return t;
}

// ---- the scan ------------------------------------------------------------

class Context {
  constructor({ index, allow, plugin }) {
    this.index = index;
    this.allow = allow;
    this.plugin = plugin;
    this.memberCache = new Map();
    this.sizeChecked = new Set();
    this.localTypes = new Set(); // structs and classes the plugin declares itself
    this.globals = new Map(); // name -> type | null (ambiguous)
    this.funcs = new Map();
    this.suspectNames = this.buildSuspects();
  }

  isEnginePkg(pkg) { return this.allow.enginePackages.has(pkg); }
  isGame(name) { const t = this.index.get(name); return !!t && !this.isEnginePkg(t.pkg); }
  isGameStruct(name) { const t = this.index.get(name); return !!t && t.kind === 'struct' && !this.isEnginePkg(t.pkg); }
  known(t) { return !!t && (this.index.has(t.n) || WRAPPER_NAMES.has(t.n) || this.localTypes.has(t.n)); }

  // The class that declares `member`, walking the bases.
  findMember(cls, member) {
    const key = cls + '\0' + member;
    if (this.memberCache.has(key)) return this.memberCache.get(key);
    let found = null;
    for (let c = this.index.get(cls), guard = 0; c && guard < 64; c = c.base ? this.index.get(c.base) : null, guard++) {
      if (c.members.has(member)) { found = { owner: c.name, text: c.members.get(member), game: !this.isEnginePkg(c.pkg) }; break; }
    }
    this.memberCache.set(key, found);
    return found;
  }

  methodReturn(cls, name) {
    for (let c = this.index.get(cls), guard = 0; c && guard < 64; c = c.base ? this.index.get(c.base) : null, guard++) {
      if (c.methods.has(name)) return typeFromText(c.methods.get(name));
    }
    return null;
  }

  // Member names declared only on the game's own classes (packages matching
  // gamePackages): in no other package, never as a method, not in the core headers.
  buildSuspects() {
    const game = new Set();
    const other = new Set(this.index.extraNames);
    for (const t of this.index.values()) {
      const own = this.allow.gamePackages.some((re) => re.test(t.pkg));
      for (const m of t.members.keys()) (own ? game : other).add(m);
      for (const m of t.methods.keys()) other.add(m);
    }
    const out = new Set();
    for (const m of game) if (!other.has(m) && m.length >= 4 && !/^(Pad|BitPad|Unk)_/.test(m)) out.add(m);
    return out;
  }
}

function matchClose(T) {
  const m = new Int32Array(T.length).fill(-1);
  const st = [];
  for (let i = 0; i < T.length; i++) {
    const t = T[i];
    if (t === '(' || t === '[' || t === '{') st.push(i);
    else if (t === ')' || t === ']' || t === '}') {
      if (st.length) { const o = st.pop(); m[o] = i; m[i] = o; }
    }
  }
  return m;
}

const sameType = (a, b) => JSON.stringify(a) === JSON.stringify(b);

function addGlobal(map, name, type) {
  if (!map.has(name)) map.set(name, type);
  else if (map.get(name) !== null && !sameType(map.get(name), type)) map.set(name, null);
}

// Scans one file. `collect` is the first pass: it fills ctx.globals and ctx.funcs
// and reports nothing.
function scanFile(ctx, file, code, collect) {
  const { text: T, line: L } = tokenize(code);
  const close = matchClose(T);
  const findings = [];
  const seen = new Set();
  const report = (kind, idx, what, extra = {}) => {
    if (collect) return;
    const key = kind + ':' + idx;
    if (seen.has(key)) return;
    seen.add(key);
    findings.push({ file, line: L[idx], kind, what, ...extra });
  };

  const scopes = [{ kind: 'file', syms: new Map() }];
  const parens = [];
  let lastParen = null;
  let unresolved = 0;

  const lookup = (name) => {
    for (let k = parens.length - 1; k >= 0; k--) if (parens[k].has(name)) return parens[k].get(name);
    if (lastParen && lastParen.has(name)) return lastParen.get(name);
    for (let k = scopes.length - 1; k >= 0; k--) if (scopes[k].syms.has(name)) return scopes[k].syms.get(name);
    if (ctx.globals.has(name)) return ctx.globals.get(name);
    return null;
  };

  const elemOf = (t) => (t && ELEMENT_CONTAINERS.has(t.n) && t.a[0] ? unwrap(t.a[0]) : null);

  // Parses a primary and its postfix operators from T[i]. Returns { type, end }.
  function evalChain(i) {
    let { type, end } = evalPrimary(i);
    let p = end;
    for (;;) {
      const t = T[p];
      if ((t === '.' || t === '->') && isIdent(T[p + 1])) {
        const name = T[p + 1];
        let q = p + 2;
        if (T[q] === '<') { const r = parseTypeArgs(T, q, 0); if (r) q = r.next; }
        const call = T[q] === '(';
        type = member(type, name, call, p + 1);
        p = call && close[q] >= 0 ? close[q] + 1 : q;
      } else if (t === '[' && close[p] >= 0) {
        type = type ? (ELEMENT_CONTAINERS.has(type.n) ? elemOf(type) : type.ind ? type : null) : null;
        p = close[p] + 1;
      } else if (t === '(' && close[p] >= 0) {
        type = null;
        p = close[p] + 1;
      } else break;
    }
    return { type, end: p };
  }

  // The type of `recv.name` or `recv.name(...)`; reports a game member.
  function member(recv, name, call, idx) {
    if (!recv) {
      if (!call && ctx.suspectNames.has(name)) report('suspect', idx, `.${name}`, { member: name });
      else unresolved++;
      return null;
    }
    if (CONTAINERS.has(recv.n)) {
      if (name === 'Data' || name === 'GetData') return recv;
      return null;
    }
    if (recv.n === 'PluginField') return call && name === 'In' ? unwrap(recv.a[0]) : null;
    if (!ctx.index.has(recv.n)) return null; // a plugin's own type
    if (call) return ctx.methodReturn(recv.n, name);
    const m = ctx.findMember(recv.n, name);
    if (!m) return null;
    if (m.game) report('member', idx, `${recv.n}::${name}`, { cls: recv.n, owner: m.owner, pkg: ctx.index.get(m.owner).pkg, member: name });
    return typeFromText(m.text);
  }

  function evalPrimary(i) {
    const t = T[i];
    if (t === '*' || t === '&') return evalPrimary(i + 1);
    if (t === '(') {
      const c = close[i];
      if (c < 0) return { type: null, end: i + 1 };
      // C-style cast: (Known*)operand
      const ty = parseType(T, i + 1);
      if (ty && ty.next === c && ctx.known(ty.type) && ty.type.n !== 'auto' && (isName(T[c + 1]) || T[c + 1] === '(' || T[c + 1] === '*' || T[c + 1] === '&')) {
        const op = evalChain(c + 1);
        return { type: unwrap(ty.type), end: op.end };
      }
      const inner = evalChain(i + 1);
      return { type: inner.end === c ? inner.type : null, end: c + 1 };
    }
    if (!isIdent(t) || (NOT_NAMES.has(t) && t !== 'this')) return { type: null, end: i + 1 };
    if (t === 'this') return { type: null, end: i + 1 };
    // Qualified name: A::B::C
    const segs = [t];
    let p = i + 1;
    while (T[p] === '::' && isIdent(T[p + 1])) { segs.push(T[p + 1]); p += 2; }
    if (segs[0] === 'SDK') segs.shift();
    const last = segs[segs.length - 1];
    // Name<T>(...)
    if (T[p] === '<' && (CASTS.has(last) || GENERIC_RETURNS.has(last))) {
      const r = parseTypeArgs(T, p, 0);
      if (r && T[r.next] === '(' && close[r.next] >= 0) {
        return { type: unwrap(r.args[0]), end: close[r.next] + 1 };
      }
    }
    if (T[p] === '(' && close[p] >= 0) {
      const end = close[p] + 1;
      if (segs.length >= 2) {
        const cls = segs[segs.length - 2];
        if (ctx.index.has(cls)) {
          if (last === 'GetDefaultObj') return { type: { n: cls, a: [], ind: true }, end };
          return { type: ctx.methodReturn(cls, last), end };
        }
        return { type: null, end };
      }
      return { type: ctx.funcs.get(last) ?? null, end };
    }
    // A plain name, or a namespace-qualified variable (Ns::Var) found by its last name.
    if (segs.length === 1 || !ctx.index.has(segs[segs.length - 2])) return { type: lookup(last), end: p };
    return { type: null, end: p };
  }

  const functionScope = () => {
    const k = scopes[scopes.length - 1].kind;
    return k === 'block';
  };

  function declare(name, type) {
    const sc = scopes[scopes.length - 1];
    if (parens.length) { parens[parens.length - 1].set(name, type); return; }
    sc.syms.set(name, type);
    if (sc.kind !== 'block') addGlobal(ctx.globals, name, type);
  }

  // `SR_FIELD(T, Class, Member, flags)`, or a plugin's macro over `(Class, Member)`: a
  // PluginField of the member's type, so a later `.In(object)` can be followed.
  function fieldMacroType(i) {
    if (!isName(T[i]) || T[i + 1] !== '(') return null;
    if (T[i] === 'SR_FIELD') {
      const r = parseType(T, i + 2);
      return r ? { n: 'PluginField', a: [r.type], ind: false } : null;
    }
    let p = i + 2;
    if (T[p] === 'SDK' && T[p + 1] === '::') p += 2;
    if (!isName(T[p]) || T[p + 1] !== ',' || !isName(T[p + 2]) || !ctx.index.has(T[p])) return null;
    const m = ctx.findMember(T[p], T[p + 2]);
    const mt = m ? typeFromText(m.text) : null;
    return mt ? { n: 'PluginField', a: [mt], ind: false } : null;
  }

  // A declaration `Type [*&] name` at i. Returns true when it was one.
  function tryDeclaration(i) {
    const r = parseType(T, i);
    if (!r) return false;
    const ty = r.type;
    const isAuto = ty.n === 'auto';
    // A PascalCase type that is not a game type (the plugin's own or the Plugin SDK's) is
    // declared as opaque, so a member read through it is never taken for a game member.
    const opaque = !isAuto && !ctx.known(ty);
    if (opaque && !/^[A-Z]/.test(ty.n)) return false;
    const name = T[r.next];
    if (!isName(name)) return false;
    const follow = T[r.next + 1];
    if (!['=', ';', ',', ')', '[', '{', '(', ':'].includes(follow)) return false;
    if (opaque && !['=', ';', '{', ',', ')'].includes(follow)) return false;
    let type;
    if (opaque) {
      type = { n: ty.n, a: [], ind: false };
    } else if (isAuto) {
      if (follow === '=') type = fieldMacroType(r.next + 2) ?? evalChain(r.next + 2).type;
      else if (follow === ':') type = elemOf(evalChain(r.next + 2).type);
      else return false;
    } else {
      type = unwrap(ty);
      if (!ty.ind && !CONTAINERS.has(ty.n) && !UNWRAP.has(ty.n) && ctx.isGameStruct(ty.n)) {
        report('byvalue', i, `${ty.n} ${name}`, { struct: ty.n });
      }
    }
    // A function declaration or definition: remember its return type.
    if (follow === '(' && !functionScope() && !parens.length && !isAuto) addGlobal(ctx.funcs, name, unwrap(ty));
    else declare(name, type);
    return true;
  }

  function checkContainer(i) {
    const r = parseType(T, i);
    if (!r || !CONTAINERS.has(r.type.n)) return;
    const walk = (t) => {
      for (const a of t.a) {
        if (!a.ind && ctx.isGameStruct(a.n)) report('stride', i, `${t.n}<${a.n}>`, { struct: a.n });
        walk(a);
      }
    };
    walk(r.type);
  }

  function checkSizeOf(i) {
    const t = T[i];
    if (T[i + 1] !== '(' || close[i + 1] < 0) return;
    const c = close[i + 1];
    if (t === 'offsetof') {
      const r = parseType(T, i + 2);
      if (r && ctx.isGame(r.type.n)) report('sizeof', i, `offsetof(${r.type.n}, ${T[r.next + 1] ?? ''})`, { struct: r.type.n });
      return;
    }
    const r = parseType(T, i + 2);
    let name = null;
    if (r && r.next === c && ctx.index.has(r.type.n)) name = r.type.n;
    else {
      const e = evalChain(i + 2);
      if (e.end === c && e.type && ctx.index.has(e.type.n)) name = e.type.n;
    }
    if (name && ctx.isGame(name)) report('sizeof', i, `${t}(${name})`, { struct: name });
  }

  function checkOffset(i) {
    // T[i] is a number.
    const num = T[i];
    const prev = T[i - 1];
    const hex = /^0[xX]/.test(num);
    const value = hex ? parseInt(num.replace(/'/g, '').replace(/[uUlL]+$/, ''), 16) : NaN;
    const stmtStart = (() => { let k = i; while (k > 0 && !['{', '}', ';'].includes(T[k - 1])) k--; return k; })();
    const stmtEnd = (() => { let k = i; while (k < T.length && !['{', '}', ';'].includes(T[k])) k++; return k; })();
    // named offset constant: constexpr ... kFooOffset = 56;
    if (prev === '=' && /^(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*$/.test(num) && /(?:^|[a-z_])(?:Off|Offset|OFFSET|Rva|RVA|Addr|Address)$/.test(T[i - 2] ?? '') && T.slice(stmtStart, i).some((x) => x === 'constexpr' || x === 'const')) {
      report('offset', i, `${T[i - 2]} = ${num}`);
      return;
    }
    if (!hex) return;
    // hard address: a cast of a large literal
    if (value >= 0x10000 && prev === '(' && (T[i - 2] === '>' || T[i - 2] === ')' || POINTER_TYPES.has(T[i - 3]))) {
      report('offset', i, `address ${num}`);
      return;
    }
    // base + 0x38, base - 0x10 in a pointer-ish statement
    if ((prev === '+' || prev === '-') && (isName(T[i - 2]) || T[i - 2] === ')' || T[i - 2] === ']')) {
      const toks = T.slice(stmtStart, stmtEnd);
      const pointerish = toks.some((x, k) => POINTERISH.has(x) || (POINTER_TYPES.has(x) && toks[k + 1] === '*') || (x === '*' && toks[k + 1] === '(' && (k === 0 || !isName(toks[k - 1]) && toks[k - 1] !== ')' && toks[k - 1] !== ']')));
      if (pointerish) report('offset', i, T.slice(Math.max(stmtStart, i - 7), Math.min(stmtEnd, i + 2)).join(' '));
    }
  }

  const prevIsValueEnd = (i) => {
    const p = T[i - 1];
    return p !== undefined && ((isIdent(p) && !NOT_NAMES.has(p)) || p === ')' || p === ']' || isNumber(p));
  };

  for (let i = 0; i < T.length; i++) {
    const t = T[i];
    if (t === '(') parens.push(new Map());
    else if (t === ')') { if (parens.length) { const done = parens.pop(); if (done.size || !lastParen) lastParen = done; } }
    else if (t === '{') {
      let prev = i - 1;
      while (prev >= 0 && ['const', 'noexcept', 'override', 'final', 'mutable'].includes(T[prev])) prev--;
      let kind = 'block';
      let syms = new Map();
      if (T[prev] === ')' && lastParen) syms = lastParen;
      else if (T[prev] !== ')') {
        let k = i - 1;
        while (k > 0 && !['{', '}', ';'].includes(T[k])) k--;
        const head = T.slice(k, i);
        if (head.includes('namespace')) kind = 'namespace';
        else if (head.some((x, j) => (x === 'class' || x === 'struct' || x === 'union') && head[j - 1] !== 'enum')) kind = 'class';
      }
      scopes.push({ kind, syms });
      lastParen = null;
    } else if (t === '}') { if (scopes.length > 1) scopes.pop(); }
    else if (t === ';') lastParen = null;

    if (t === 'SR_SIZE_CHECK' && T[i + 1] === '(') {
      let p = i + 2;
      if (T[p] === 'SDK' && T[p + 1] === '::') p += 2;
      if (isIdent(T[p])) ctx.sizeChecked.add(T[p]);
    }

    if ((t === 'struct' || t === 'class') && isName(T[i + 1]) && (T[i + 2] === '{' || T[i + 2] === ':' || T[i + 2] === 'final')) ctx.localTypes.add(T[i + 1]);
    if (t === 'reinterpret_cast' && T[i + 1] === '<') {
      const r = parseTypeArgs(T, i + 1, 0);
      if (r && ctx.localTypes.has(r.args[0].n)) report('overlay', i, `reinterpret_cast<${r.args[0].n}*>`, { struct: r.args[0].n });
    }

    if (isNumber(t)) { checkOffset(i); continue; }
    if (!isIdent(t) && t !== '(') continue;
    if (t === 'sizeof' || t === 'alignof' || t === 'offsetof') { checkSizeOf(i); continue; }
    if (T[i - 1] === '::' && CONTAINERS.has(t) && T[i + 1] === '<') checkContainer(i);
    if (T[i - 1] === '.' || T[i - 1] === '->' || T[i - 1] === '::') continue;

    if (isIdent(t)) {
      if (CONTAINERS.has(t) && T[i + 1] === '<') checkContainer(i);
      const declared = (ctx.index.has(t) || WRAPPER_NAMES.has(t) || QUAL.has(t) || t === 'SDK' || t === 'auto' || /^[A-Z]/.test(t)) && tryDeclaration(i);
      void declared;
    }
    if (t === '(' && prevIsValueEnd(i)) continue;
    evalChain(i);
  }
  return { findings, unresolved };
}

// ---- driver --------------------------------------------------------------

export function listSources(dirs) {
  const out = [];
  const walk = (d) => {
    for (const name of readdirSync(d)) {
      const p = join(d, name);
      const st = statSync(p);
      if (st.isDirectory()) { if (!SKIP_DIRS.has(name)) walk(p); }
      else if (SOURCE_EXT.test(name)) out.push(p);
    }
  };
  for (const d of dirs) walk(d);
  return out;
}

export function declaresMigrated(code) {
  return /\bPLUGIN_MAP_MIGRATED\b/.test(code);
}

// Applies the allow-list; returns { kept, allowed, stale }.
export function applyAllow(findings, allow, plugin) {
  const used = new Set();
  const kept = [];
  const allowed = [];
  for (const f of findings) {
    const idx = allow.exceptions.findIndex((e) =>
      (!e.plugin || e.plugin === plugin) &&
      (!e.file || f.file === e.file || f.file.endsWith('/' + e.file)) &&
      (!e.kind || e.kind === f.kind) &&
      (!e.what || f.what.includes(e.what)));
    if (idx >= 0) { used.add(idx); allowed.push({ ...f, reason: allow.exceptions[idx].reason }); } else kept.push(f);
  }
  const stale = allow.exceptions.filter((e, k) => e.plugin === plugin && !used.has(k));
  return { kept, allowed, stale };
}

export function check({ plugin, sdkDir, sources, allowFile, mode = 'auto', index = null }) {
  const allow = loadAllow(allowFile);
  const idx = index ?? loadSdkIndex(sdkDir);
  const ctx = new Context({ index: idx, allow, plugin });
  const files = listSources(sources);
  const codes = files.map((f) => ({ file: f, code: stripNonCode(readFileSync(f, 'utf8')) }));
  const root = sources.length === 1 ? sources[0] : null;
  const rel = (f) => (root ? relative(root, f) : f).split(sep).join('/');
  const migrated = codes.some((c) => declaresMigrated(c.code));
  for (const c of codes) scanFile(ctx, rel(c.file), c.code, true);
  // SR_SIZE_CHECK lists are plugin-wide; they are complete after the first pass.
  const all = [];
  let unresolved = 0;
  for (const c of codes) {
    const r = scanFile(ctx, rel(c.file), c.code, false);
    all.push(...r.findings);
    unresolved += r.unresolved;
  }
  const covered = all.filter((f) => ['sizeof', 'stride', 'byvalue'].includes(f.kind) && ctx.sizeChecked.has(f.struct));
  const flagged = all.filter((f) => !covered.includes(f));
  const { kept, allowed, stale } = applyAllow(flagged, allow, plugin);
  const enforced = mode === 'enforce' || (mode === 'auto' && migrated);
  return { plugin, mode, migrated, enforced, findings: kept, allowed, sizeChecked: covered.length, stale, unresolved, files: files.length };
}

const MESSAGES = {
  member: (f) => `direct read of game member ${f.what}${f.owner && f.owner !== f.cls ? ` (declared on ${f.owner}, package ${f.pkg})` : ''}: request it by name from the loader (SR_FIELD or SR_BITFIELD) and read it through the field`,
  suspect: (f) => `member ${f.member} is declared only on game classes and its receiver could not be typed: read it through a loader field, or add an exception with a reason`,
  sizeof: (f) => `${f.what}: a compiled game layout; list SR_SIZE_CHECK(${f.struct}) or ask the loader`,
  stride: (f) => `${f.what}: a compiled stride of a game struct; list SR_SIZE_CHECK(${f.struct})`,
  byvalue: (f) => `${f.what}: a game struct held by value uses its compiled layout; list SR_SIZE_CHECK(${f.struct}) or hold a pointer`,
  overlay: (f) => `${f.what}: the plugin's own ${f.struct} laid over memory has a compiled layout; request the member by name from the loader instead`,
  offset: (f) => `compiled offset or address (${f.what}): resolve it by name through the loader`,
};

export function formatFinding(f, severity, prefix = '') {
  return `${prefix}${f.file}(${f.line}): ${severity} RULE3 ${f.kind}: ${MESSAGES[f.kind](f)}`;
}

function parseArgs(argv) {
  const o = { source: [], mode: 'auto' };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const v = () => { if (i + 1 >= argv.length) throw new Error(`${a} needs a value`); return argv[++i]; };
    if (a === '--plugin') o.plugin = v();
    else if (a === '--sdk') o.sdk = v();
    else if (a === '--source') o.source.push(v());
    else if (a === '--allow') o.allow = v();
    else if (a === '--mode') o.mode = v();
    else if (a === '--json') o.json = v();
    else throw new Error(`unknown argument ${a}`);
  }
  if (!o.plugin || !o.sdk || !o.source.length) throw new Error('usage: rule3-check.mjs --plugin <name> --sdk <dir> --source <dir>... [--allow f] [--mode auto|enforce|report] [--json f]');
  if (!['auto', 'enforce', 'report'].includes(o.mode)) throw new Error(`--mode ${o.mode}: use auto, enforce or report`);
  o.allow ??= join(dirname(fileURLToPath(import.meta.url)), 'rule3-allow.json');
  for (const [k, p] of [['--sdk', o.sdk], ...o.source.map((s) => ['--source', s]), ['--allow', o.allow]]) if (!existsSync(p)) throw new Error(`${k} ${p}: not found`);
  return o;
}

export function main(argv) {
  let o;
  try { o = parseArgs(argv); } catch (e) { process.stderr.write(`rule3-check: ${e.message}\n`); return EXIT_USAGE; }
  let r;
  try {
    r = check({ plugin: o.plugin, sdkDir: o.sdk, sources: o.source.map((s) => s.replace(/[\\/]+$/, '') || s), allowFile: o.allow, mode: o.mode });
  } catch (e) { process.stderr.write(`rule3-check: ${e.message}\n`); return EXIT_USAGE; }
  const base = o.source.length === 1 ? o.source[0].replace(/[\\/]+$/, '') + sep : '';
  const sev = r.enforced ? 'error' : 'warning';
  for (const f of r.findings) process.stdout.write(formatFinding(f, sev, base) + '\n');
  for (const e of r.stale) process.stdout.write(`rule3-check: warning: the exception for ${e.plugin} ${e.file ?? ''} ${e.what ?? ''} matched nothing; remove it\n`);
  const counts = {};
  for (const f of r.findings) counts[f.kind] = (counts[f.kind] ?? 0) + 1;
  process.stdout.write(`rule3-check: ${r.plugin}: ${r.findings.length} finding(s) ${JSON.stringify(counts)}, ${r.allowed.length} allowed, ${r.sizeChecked} size-checked, ${r.unresolved} untyped receiver(s); ${r.files} files; ` +
    `${r.enforced ? 'ENFORCED' : `report only (${r.mode === 'auto' ? 'the plugin does not set PLUGIN_MAP_MIGRATED' : 'report mode'})`}\n`);
  if (o.json) writeFileSync(o.json, JSON.stringify({ ...r, counts, status: r.findings.length === 0 ? 'clean' : r.enforced ? 'fail' : 'findings' }, null, 2) + '\n');
  return r.enforced && r.findings.length ? EXIT_VIOLATION : EXIT_OK;
}

if (process.argv[1] && basename(process.argv[1]) === 'rule3-check.mjs') process.exitCode = main(process.argv.slice(2));
