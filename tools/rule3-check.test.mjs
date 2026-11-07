// Tests for rule3-check.mjs. Run: node --test tools/rule3-check.test.mjs
// Builds a tiny fake Dumper-7 SDK and a fake plugin in a temp dir for each case.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { check, main, applyAllow, stripNonCode, declaresMigrated, EXIT_OK, EXIT_USAGE, EXIT_VIOLATION } from './rule3-check.mjs';

const member = (type, name, off) => `\t${type.padEnd(45)} ${name};`.padEnd(70) + ` // 0x${off.toString(16).padStart(4, '0')}(0x0008)(ZeroConstructor)`;

const CHIMERA = [
  '// Class Chimera.CrHealth',
  'class UCrHealth final : public UObject',
  '{',
  'public:',
  member('float', 'CurrentHealth', 0x40),
  member('bool', 'bGameOnlyFlag', 0x44),
  member('struct FCrItem', 'Held', 0x48),
  member('TArray<struct FCrItem>', 'Items', 0x58),
  '\tclass UCrHealth* GetSelf() const;',
  '};',
  '',
  'struct FCrItem final',
  '{',
  'public:',
  member('int32', 'Count', 0x0),
  '};',
  '',
].join('\n');

const ENGINE = [
  'class UEngineThing : public UObject',
  '{',
  'public:',
  member('float', 'MaxWalkSpeed', 0x100),
  member('bool', 'bGameOnlyFlag', 0x104), // same name on an engine class: not a suspect
  '};',
  '',
].join('\n');

const ALLOW = {
  gamePackages: [{ pattern: '^Chimera', reason: 'game code' }],
  enginePackages: [{ names: ['Engine', 'CoreUObject'], reason: 'engine layout' }],
  exceptions: [],
};

function fixture(files, { allow = ALLOW } = {}) {
  const root = mkdtempSync(join(tmpdir(), 'rule3-'));
  const sdk = join(root, 'SDK');
  const src = join(root, 'src');
  mkdirSync(sdk);
  mkdirSync(src);
  writeFileSync(join(sdk, 'Chimera_classes.hpp'), CHIMERA);
  writeFileSync(join(sdk, 'Engine_classes.hpp'), ENGINE);
  for (const [name, body] of Object.entries(files)) writeFileSync(join(src, name), body);
  const allowFile = join(root, 'allow.json');
  writeFileSync(allowFile, JSON.stringify(allow));
  return { root, sdk, src, allowFile, done: () => rmSync(root, { recursive: true, force: true }) };
}

function run(files, opts = {}) {
  const f = fixture(files, opts);
  try {
    return check({ plugin: 'Fake', sdkDir: f.sdk, sources: [f.src], allowFile: f.allowFile, mode: opts.mode ?? 'report' });
  } finally { f.done(); }
}

const kinds = (r) => r.findings.map((x) => x.kind);

test('flags a direct member read through a typed pointer', () => {
  const r = run({ 'a.cpp': 'void f(SDK::UCrHealth* h) { float x = h->CurrentHealth; }\n' });
  assert.deepEqual(kinds(r), ['member']);
  assert.equal(r.findings[0].what, 'UCrHealth::CurrentHealth');
  assert.equal(r.findings[0].line, 1);
});

test('does not flag an engine class member', () => {
  const r = run({ 'a.cpp': 'void f(SDK::UEngineThing* e) { float x = e->MaxWalkSpeed; }\n' });
  assert.deepEqual(r.findings, []);
});

test('follows auto, casts and method return types', () => {
  const r = run({ 'a.cpp': [
    'void f(SDK::UObject* o) {',
    '  auto* h = static_cast<SDK::UCrHealth*>(o);',
    '  auto* s = h->GetSelf();',
    '  s->CurrentHealth = 1.0f;',
    '}',
  ].join('\n') });
  assert.deepEqual(kinds(r), ['member']);
  assert.equal(r.findings[0].line, 4);
});

test('flags a suspect member on a receiver it cannot type', () => {
  const r = run({ 'a.cpp': 'void f() { auto* x = Unknown(); x->bGameOnlyFlag = true; }\n' });
  // bGameOnlyFlag also exists on an engine class, so it is not a suspect name
  assert.deepEqual(r.findings, []);
  const r2 = run({ 'a.cpp': 'void f() { auto* x = Unknown(); x->CurrentHealth = 1; }\n' });
  assert.deepEqual(kinds(r2), ['suspect']);
});

test('a member of the plugin\'s own struct is not a game member', () => {
  const r = run({ 'a.cpp': [
    'struct Entry { float CurrentHealth; };',
    'void f(Entry& e) { e.CurrentHealth = 2.0f; }',
  ].join('\n') });
  assert.deepEqual(r.findings, []);
});

test('flags sizeof, stride and by-value of a game struct', () => {
  const r = run({ 'a.cpp': [
    'size_t n = sizeof(SDK::FCrItem);',
    'std::vector<int> v; SDK::TArray<SDK::FCrItem> arr;',
    'void g() { SDK::FCrItem local; }',
  ].join('\n') });
  assert.deepEqual(kinds(r).sort(), ['byvalue', 'sizeof', 'stride']);
});

test('SR_SIZE_CHECK of the struct suppresses sizeof, stride and by-value', () => {
  const r = run({ 'a.cpp': [
    'SR_SIZE_CHECK(FCrItem)',
    'size_t n = sizeof(SDK::FCrItem);',
    'SDK::TArray<SDK::FCrItem> arr;',
  ].join('\n') });
  assert.deepEqual(r.findings, []);
  assert.equal(r.sizeChecked, 2);
});

test('flags compiled offsets and named offset constants, not float constants', () => {
  const r = run({ 'a.cpp': [
    'void f(void* p) { auto v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(p) + 0x38); }',
    'constexpr int kThingOffset = 0x2998;',
    'constexpr float kFloorOffset = 200.0;',
  ].join('\n') });
  assert.deepEqual(kinds(r).sort(), ['offset', 'offset']);
});

test('flags a plugin struct laid over game memory', () => {
  const r = run({ 'a.cpp': [
    'struct HealthView { float a; float b; };',
    'void f(void* p) { auto* v = reinterpret_cast<HealthView*>(p); }',
  ].join('\n') });
  assert.deepEqual(kinds(r), ['overlay']);
});

test('an exception with a reason removes the finding; a stale one is reported', () => {
  const allow = { ...ALLOW, exceptions: [
    { plugin: 'Fake', file: 'a.cpp', kind: 'offset', what: 'kThingOffset', reason: 'engine layout' },
    { plugin: 'Fake', file: 'gone.cpp', reason: 'was needed once' },
  ] };
  const r = run({ 'a.cpp': 'constexpr int kThingOffset = 0x2998;\n' }, { allow });
  assert.deepEqual(r.findings, []);
  assert.equal(r.allowed.length, 1);
  assert.equal(r.stale.length, 1);
});

test('an exception without a reason is refused', () => {
  const allow = { ...ALLOW, exceptions: [{ plugin: 'Fake', file: 'a.cpp' }] };
  assert.throws(() => run({ 'a.cpp': 'int x;\n' }, { allow }), /no reason/);
});

test('applyAllow matches file by path suffix and plugin', () => {
  const allow = { exceptions: [{ plugin: 'Other', file: 'a.cpp', reason: 'r' }, { plugin: 'Fake', file: 'a.cpp', reason: 'r' }] };
  const f = { file: 'sub/a.cpp', kind: 'member', what: 'X::y' };
  assert.equal(applyAllow([f], allow, 'Fake').allowed.length, 1);
  assert.equal(applyAllow([f], { exceptions: [allow.exceptions[0]] }, 'Fake').kept.length, 1);
});

test('an allow entry matches from any checkout path (repo-relative)', () => {
  const f = fixture({});
  try {
    // two checkouts of one repo at different depths, each with a .git entry
    for (const dir of ['checkout', join('wt', 'deep', 'checkout')]) {
      const repo = join(f.root, dir);
      mkdirSync(join(repo, 'src'), { recursive: true });
      writeFileSync(join(repo, '.git'), 'gitdir: elsewhere');
      writeFileSync(join(repo, 'src', 'a.cpp'), 'constexpr int kThingOffset = 0x2998;');
      writeFileSync(f.allowFile, JSON.stringify({ ...ALLOW, exceptions: [{ plugin: 'Fake', file: 'src/a.cpp', kind: 'offset', what: 'kThingOffset', reason: 'engine layout' }] }));
      for (const source of [repo, join(repo, 'src')]) {
        const r = check({ plugin: 'Fake', sdkDir: f.sdk, sources: [source], allowFile: f.allowFile, mode: 'report' });
        assert.deepEqual(r.findings, [], source);
        assert.equal(r.allowed.length, 1, source);
        assert.equal(r.allowed[0].file, 'src/a.cpp');
      }
    }
  } finally { f.done(); }
});

test('an alignof allow entry carries kind, what and a reason, and matches only its file', () => {
  const entry = { plugin: 'Fake', file: 'a.cpp', kind: 'sizeof', what: 'alignof(FCrItem)', reason: 'alignment input; fails closed' };
  const allow = { ...ALLOW, exceptions: [entry] };
  const body = 'size_t x = alignof(SDK::FCrItem);';
  const ok = run({ 'a.cpp': body }, { allow });
  assert.deepEqual(ok.findings, []);
  assert.equal(ok.allowed[0].reason, 'alignment input; fails closed');
  assert.equal(run({ 'b.cpp': body }, { allow }).findings.length, 1);
});

test('comments and strings are not scanned', () => {
  const r = run({ 'a.cpp': [
    '// h->CurrentHealth',
    '/* sizeof(SDK::FCrItem) */',
    'const char* s = "h->CurrentHealth + 0x38";',
  ].join('\n') });
  assert.deepEqual(r.findings, []);
  assert.equal(stripNonCode('a // b\nc').includes('b'), false);
});

test('PLUGIN_MAP_MIGRATED turns auto mode into enforcement', () => {
  const body = 'void f(SDK::UCrHealth* h) { h->CurrentHealth = 0; }\n';
  const notMigrated = run({ 'a.cpp': body }, { mode: 'auto' });
  assert.equal(notMigrated.enforced, false);
  const migrated = run({ 'a.cpp': body, 'm.h': 'static const unsigned kFlags = PLUGIN_MAP_MIGRATED;\n' }, { mode: 'auto' });
  assert.equal(migrated.enforced, true);
  assert.equal(declaresMigrated(stripNonCode('kFlags = PLUGIN_MAP_MIGRATED;')), true);
  assert.equal(declaresMigrated(stripNonCode('// PLUGIN_MAP_MIGRATED')), false);
});

test('exit codes: findings fail under enforce, pass in report, bad usage is 2', () => {
  const f = fixture({ 'a.cpp': 'void f(SDK::UCrHealth* h) { h->CurrentHealth = 0; }\n' });
  const out = join(f.root, 'out.json');
  const base = ['--plugin', 'Fake', '--sdk', f.sdk, '--source', f.src, '--allow', f.allowFile];
  const write = process.stdout.write.bind(process.stdout);
  process.stdout.write = () => true;
  try {
    assert.equal(main([...base, '--mode', 'enforce', '--json', out]), EXIT_VIOLATION);
    assert.equal(JSON.parse(readFileSync(out, 'utf8')).status, 'fail');
    assert.equal(main([...base, '--mode', 'report', '--json', out]), EXIT_OK);
    assert.equal(JSON.parse(readFileSync(out, 'utf8')).status, 'findings');
    assert.equal(main([...base, '--mode', 'bogus']), EXIT_USAGE);
    assert.equal(main(['--plugin', 'Fake']), EXIT_USAGE);
  } finally {
    process.stdout.write = write;
    f.done();
  }
});

test('a clean plugin reports clean and passes under enforce', () => {
  const f = fixture({ 'a.cpp': 'int main() { return 0; }\n' });
  const out = join(f.root, 'out.json');
  const write = process.stdout.write.bind(process.stdout);
  process.stdout.write = () => true;
  try {
    assert.equal(main(['--plugin', 'Fake', '--sdk', f.sdk, '--source', f.src, '--allow', f.allowFile, '--mode', 'enforce', '--json', out]), EXIT_OK);
    assert.equal(JSON.parse(readFileSync(out, 'utf8')).status, 'clean');
  } finally {
    process.stdout.write = write;
    f.done();
  }
});

test('a stride stepped by ElementAt with the SR_SIZE spec is covered', () => {
  const r = run({ 'a.cpp': [
    'inline auto Item_Size = SR_SIZE(FCrItem);',
    'const uint8_t* f(SDK::TArray<SDK::FCrItem>& arr) { return PluginMap::ElementAt(arr, 1, Fields::Item_Size); }',
  ].join('\n') });
  assert.deepEqual(r.findings, []);
});

test('a stride stepped by index * spec.resolvedSize is covered', () => {
  const r = run({ 'a.cpp': [
    'inline auto Item_Size = SR_SIZE(FCrItem);',
    'const uint8_t* g(SDK::TArray<SDK::FCrItem>& arr, int i) {',
    '  return reinterpret_cast<const uint8_t*>(arr.GetData()) + i * Item_Size.resolvedSize;',
    '}',
    'size_t h(const uint8_t* data, int i) { return i * Item_Size.resolvedSize; }',
  ].join('\n') });
  assert.deepEqual(r.findings, []);
});

test('a typed arr[i] next to an ElementAt in the same file is still a stride', () => {
  const r = run({ 'a.cpp': [
    'inline auto Item_Size = SR_SIZE(FCrItem);',
    'const uint8_t* a(SDK::TArray<SDK::FCrItem>& live) { return PluginMap::ElementAt(live, 1, Item_Size); }',
    'int b(SDK::TArray<SDK::FCrItem>& arr, int i) { auto& x = arr[i]; return &x != nullptr; }',
    'void c(SDK::TArray<SDK::FCrItem>& more, int i) { PluginMap::ElementAt(more, 0, Item_Size); auto* p = more.GetData() + i; }',
  ].join('\n') });
  assert.deepEqual(kinds(r), ['stride', 'stride']);
  assert.deepEqual(r.findings.map((x) => x.line), [3, 4]);
});

test('a TArray argument passed inline to ElementAt is not a stride', () => {
  const r = run({ 'a.cpp': [
    'inline auto Item_Size = SR_SIZE(FCrItem);',
    'const uint8_t* f(void* raw) { return PluginMap::ElementAt(*static_cast<SDK::TArray<SDK::FCrItem>*>(raw), 2, Item_Size); }',
    'const uint8_t* g(const SDK::TArray<SDK::FCrItem>& arr) { return arr.Num() ? PluginMap::ElementAt(arr, 0, Item_Size) : nullptr; }',
  ].join('\n') });
  assert.deepEqual(r.findings, []);
});

test('a TArray pointer that is null-tested, counted and stepped by ElementAt is not a stride', () => {
  const r = run({ 'a.cpp': [
    'inline auto Item_Size = SR_SIZE(FCrItem);',
    'const uint8_t* f(SDK::TArray<SDK::FCrItem>* items) {',
    '  if (!items || items->Num() == 0) return nullptr;',
    '  return PluginMap::ElementAt(*items, 0, Item_Size);',
    '}',
  ].join('\n') });
  assert.deepEqual(r.findings, []);
});

test('aliasing, iterating or taking GetData() of a TArray stays a stride beside an ElementAt', () => {
  const r = run({ 'a.cpp': [
    'inline auto Item_Size = SR_SIZE(FCrItem);',
    'void a(SDK::TArray<SDK::FCrItem>& x) { PluginMap::ElementAt(x, 0, Item_Size); auto& y = x; }',
    'void b(SDK::TArray<SDK::FCrItem>& x) { PluginMap::ElementAt(x, 0, Item_Size); for (auto& e : x) {} }',
    'void c(SDK::TArray<SDK::FCrItem>& x) { PluginMap::ElementAt(x, 0, Item_Size); auto* p = x.GetData(); }',
  ].join('\n') });
  assert.deepEqual(kinds(r), ['stride', 'stride', 'stride']);
});

test('mentioning SR_SIZE does not excuse a sizeof or a stride in a file that never steps by it', () => {
  const r = run({
    'a.cpp': 'inline auto Item_Size = SR_SIZE(FCrItem);\nsize_t n = sizeof(SDK::FCrItem);\n',
    'b.cpp': 'void h(SDK::TArray<SDK::FCrItem>& arr) { auto n = arr.Num() * sizeof(SDK::FCrItem); }\n',
    'c.cpp': 'void k(SDK::TArray<SDK::FCrItem>& arr) { auto p = PluginMap::ElementAt(arr, 0, Other_Size); }\n',
  });
  assert.deepEqual(kinds(r).sort(), ['sizeof', 'sizeof', 'stride', 'stride']);
});
