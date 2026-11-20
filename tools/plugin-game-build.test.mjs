// Tests for PluginGameBuild.props: the game build a plugin declares (PLUGIN_GAME_BUILT_CL)
// comes from the build_cl.txt of the Game-SDK headers it compiles against. A PluginGameBuiltCL
// pin never fails a build; when it disagrees the headers win and a warning names both numbers.
// Run: node --test tools/plugin-game-build.test.mjs
// Needs MSBuild: MSBUILD (a path), msbuild on PATH, or Visual Studio's (found with vswhere).
// Each case builds a tiny project that imports the props file, against a fake Game-SDK in a temp dir.
import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const SDK_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PROPS = join(SDK_ROOT, 'PluginGameBuild.props');

function findMsBuild() {
  if (process.env.MSBUILD && existsSync(process.env.MSBUILD)) return process.env.MSBUILD;
  const onPath = spawnSync('msbuild', ['-version', '-nologo'], { windowsHide: true, encoding: 'utf8' });
  if (onPath.status === 0) return 'msbuild';
  const vswhere = join(process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)', 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
  if (existsSync(vswhere)) {
    const r = spawnSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild', '-find', 'MSBuild\\**\\Bin\\MSBuild.exe'], { windowsHide: true, encoding: 'utf8' });
    const first = (r.stdout || '').split(/\r?\n/).find((l) => l.trim());
    if (first && existsSync(first.trim())) return first.trim();
  }
  return null;
}

const MSBUILD = findMsBuild();
const work = mkdtempSync(join(tmpdir(), 'plugin-game-build-'));
after(() => rmSync(work, { recursive: true, force: true }));

let seq = 0;
const dirOf = (name) => join(work, `${name}-${++seq}`);

// A fake Game-SDK: one folder per side, with a build_cl.txt holding the given text (none when undefined).
function makeSdk(name, sides) {
  const root = dirOf(name);
  for (const [side, marker] of Object.entries(sides)) {
    mkdirSync(join(root, side), { recursive: true });
    if (marker !== undefined) writeFileSync(join(root, side, 'build_cl.txt'), marker);
  }
  return root;
}

// A project that imports the props the way a plugin's Shared.props does, with an empty ClCompile
// target that prints the define. SharedPin stands in for a pin set in the plugin's Shared.props.
const PROJ = `<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <Configuration Condition="'$(Configuration)' == ''">Client Release</Configuration>
    <StarRuptureSDKConfigDir Condition="'$(TestConfigDir)' != ''">$(TestConfigDir)</StarRuptureSDKConfigDir>
    <PluginGameBuiltCL Condition="'$(PluginGameBuiltCL)' == '' And '$(SharedPin)' != ''">$(SharedPin)</PluginGameBuiltCL>
  </PropertyGroup>
  <Import Project="${PROPS}" />
  <ItemGroup><ClCompile Include="a.cpp" /></ItemGroup>
  <Target Name="ClCompile"><Message Importance="high" Text="DEFINES=%(ClCompile.PreprocessorDefinitions)" /></Target>
  <Target Name="Build" DependsOnTargets="ClCompile" />
</Project>
`;

function build(props, projDir = dirOf('proj')) {
  mkdirSync(projDir, { recursive: true });
  const proj = join(projDir, 'test.proj');
  writeFileSync(proj, PROJ);
  const args = [proj, '-nologo', '-v:minimal', '-nodeReuse:false', ...Object.entries(props).map(([k, v]) => `-p:${k}=${v}`)];
  const r = spawnSync(MSBUILD, args, { windowsHide: true, encoding: 'utf8', cwd: projDir });
  const out = `${r.stdout || ''}${r.stderr || ''}`;
  const m = out.match(/DEFINES=(\S*)/);
  const warnings = [...new Set([...out.matchAll(/warning (PLUGINCL\d+)/g)].map((x) => x[1]))];
  return { ok: r.status === 0, out, define: m ? m[1] : null, warnings };
}

const def = (cl) => `PLUGIN_GAME_BUILT_CL=${cl}u;`;
const opts = { skip: MSBUILD ? false : 'MSBuild not found (set MSBUILD)' };

test('a pin that matches the marker: the define is the marker, no warning', opts, () => {
  const sdk = makeSdk('match', { Client: '130045', Server: '127004' });
  const r = build({ GameSDKRoot: sdk, PluginGameBuiltCL: '130045' });
  assert.ok(r.ok, r.out);
  assert.equal(r.define, def(130045));
  assert.deepEqual(r.warnings, []);
});

test('a stale pin: the build passes, the define is the marker, a warning names both builds', opts, () => {
  const sdk = makeSdk('stale', { Client: '130045' });
  const r = build({ GameSDKRoot: sdk, PluginGameBuiltCL: '127004' });
  assert.ok(r.ok, r.out);
  assert.equal(r.define, def(130045), 'the headers win over the pin');
  assert.deepEqual(r.warnings, ['PLUGINCL004']);
  assert.match(r.out, /PluginGameBuiltCL says 127004/);
  assert.match(r.out, /are build 130045/);
  assert.match(r.out, /declares 130045/);
});

test('a pin set in the plugin\'s Shared.props, not on the command line, is handled the same way', opts, () => {
  const sdk = makeSdk('shared-pin', { Client: '130045' });
  const same = build({ GameSDKRoot: sdk, SharedPin: '130045' });
  assert.ok(same.ok, same.out);
  assert.deepEqual(same.warnings, []);
  const stale = build({ GameSDKRoot: sdk, SharedPin: '127004' });
  assert.ok(stale.ok, stale.out);
  assert.equal(stale.define, def(130045));
  assert.deepEqual(stale.warnings, ['PLUGINCL004']);
});

test('no pin: the define is the marker, no warning', opts, () => {
  const sdk = makeSdk('nopin', { Client: '130045\r\n' });
  const r = build({ GameSDKRoot: sdk });
  assert.ok(r.ok, r.out);
  assert.equal(r.define, def(130045));
  assert.deepEqual(r.warnings, []);
});

test('the Server configuration reads the Server marker', opts, () => {
  const sdk = makeSdk('server', { Client: '130045', Server: '127004' });
  const r = build({ GameSDKRoot: sdk, Configuration: 'Server Release' });
  assert.ok(r.ok, r.out);
  assert.equal(r.define, def(127004));
  const stale = build({ GameSDKRoot: sdk, Configuration: 'Server Release', PluginGameBuiltCL: '130045' });
  assert.ok(stale.ok, stale.out);
  assert.equal(stale.define, def(127004));
  assert.deepEqual(stale.warnings, ['PLUGINCL004']);
  const legacy = build({ GameSDKRoot: sdk, Configuration: 'Release' });
  assert.ok(legacy.ok, legacy.out);
  assert.equal(legacy.define, def(127004), 'Debug and Release are the Server SDK');
});

test('the Game-SDK default branch moves its marker under an unchanged pin: the build still passes', opts, () => {
  // The pin stays 127004 while the marker goes 127004 -> 130045, as after a merge of a newer Game-SDK.
  const sdk = makeSdk('moved', { Client: '127004' });
  const marker = join(sdk, 'Client', 'build_cl.txt');
  const before = build({ GameSDKRoot: sdk, SharedPin: '127004' });
  assert.ok(before.ok, before.out);
  assert.equal(before.define, def(127004));
  assert.deepEqual(before.warnings, []);
  writeFileSync(marker, '130045');
  const after = build({ GameSDKRoot: sdk, SharedPin: '127004' });
  assert.ok(after.ok, after.out);
  assert.equal(after.define, def(130045), 'the DLL now declares the headers\' build');
  assert.deepEqual(after.warnings, ['PLUGINCL004']);
  writeFileSync(marker, '127004');
  const back = build({ GameSDKRoot: sdk, SharedPin: '127004' });
  assert.ok(back.ok, back.out);
  assert.equal(back.define, def(127004));
  assert.deepEqual(back.warnings, []);
});

test('no build_cl.txt and a pin: the pin is used, with a warning that it was not checked', opts, () => {
  const sdk = makeSdk('nomarker-pin', { Client: undefined });
  const r = build({ GameSDKRoot: sdk, PluginGameBuiltCL: '127004' });
  assert.ok(r.ok, r.out);
  assert.equal(r.define, def(127004));
  assert.deepEqual(r.warnings, ['PLUGINCL001']);
  assert.match(r.out, /cannot be checked/);
  assert.match(r.out, /declares 127004/);
});

test('no build_cl.txt and no pin: no define and no warning, as before', opts, () => {
  const sdk = makeSdk('nomarker-nopin', { Client: undefined });
  const r = build({ GameSDKRoot: sdk });
  assert.ok(r.ok, r.out);
  assert.equal(r.define, '', 'nothing is defined, so the plugin declares 0');
  assert.deepEqual(r.warnings, []);
});

test('a Game-SDK folder that does not exist: the pin is used, silently', opts, () => {
  const pinned = build({ GameSDKRoot: join(work, 'does-not-exist'), PluginGameBuiltCL: '127004' });
  assert.ok(pinned.ok, pinned.out);
  assert.equal(pinned.define, def(127004));
  assert.deepEqual(pinned.warnings, []);
  const bare = build({ GameSDKRoot: join(work, 'does-not-exist') });
  assert.ok(bare.ok, bare.out);
  assert.equal(bare.define, '');
  assert.deepEqual(bare.warnings, []);
});

test('no Game-SDK folder anywhere (a plugin with no Game-SDK headers): the pin or nothing, silent', opts, () => {
  // No GameSDKRoot, no include dir, and no "StarRupture SDK" folder under SolutionDir.
  const solDir = dirOf('nosdk');
  mkdirSync(solDir, { recursive: true });
  const pinned = build({ SolutionDir: `${solDir}\\`, PluginGameBuiltCL: '130045' });
  assert.ok(pinned.ok, pinned.out);
  assert.equal(pinned.define, def(130045));
  assert.deepEqual(pinned.warnings, []);
  const bare = build({ SolutionDir: `${solDir}\\` });
  assert.ok(bare.ok, bare.out);
  assert.equal(bare.define, '');
  assert.deepEqual(bare.warnings, []);
});

test('the "StarRupture SDK" folder under SolutionDir is used when it exists', opts, () => {
  const solDir = dirOf('solsdk');
  mkdirSync(join(solDir, 'StarRupture SDK', 'Client'), { recursive: true });
  const none = build({ SolutionDir: `${solDir}\\`, PluginGameBuiltCL: '127004' });
  assert.ok(none.ok, none.out);
  assert.equal(none.define, def(127004));
  assert.deepEqual(none.warnings, ['PLUGINCL001'], 'the SDK folder exists but has no marker');
  writeFileSync(join(solDir, 'StarRupture SDK', 'Client', 'build_cl.txt'), '130045');
  const marked = build({ SolutionDir: `${solDir}\\`, PluginGameBuiltCL: '127004' });
  assert.ok(marked.ok, marked.out);
  assert.equal(marked.define, def(130045));
  assert.deepEqual(marked.warnings, ['PLUGINCL004']);
});

test('a marker that is empty or not a number counts as missing', opts, () => {
  for (const text of ['', '  \r\n', 'latest']) {
    const sdk = makeSdk('junk', { Client: text });
    const pinned = build({ GameSDKRoot: sdk, PluginGameBuiltCL: '127004' });
    assert.ok(pinned.ok, pinned.out);
    assert.equal(pinned.define, def(127004), JSON.stringify(text));
    assert.deepEqual(pinned.warnings, ['PLUGINCL001']);
    const bare = build({ GameSDKRoot: sdk });
    assert.ok(bare.ok, bare.out);
    assert.equal(bare.define, '', JSON.stringify(text));
  }
});

test('the headers on the include path decide, not GameSDKRoot', opts, () => {
  const a = makeSdk('root-a', { Client: '127004' });
  const b = makeSdk('root-b', { Client: '130045' });
  const dirs = `${join(b, 'Client')}\\%3B${join(b, 'Client', 'SDK')}\\`; // %3B: a ; on the command line
  const r = build({ GameSDKRoot: a, TestConfigDir: dirs, PluginGameBuiltCL: '127004' });
  assert.ok(r.ok, r.out);
  assert.equal(r.define, def(130045));
  assert.deepEqual(r.warnings, ['PLUGINCL004']);
  assert.match(r.out, /are build 130045/);
  const bare = build({ GameSDKRoot: a, TestConfigDir: dirs });
  assert.ok(bare.ok, bare.out);
  assert.equal(bare.define, def(130045));
});

test('a relative GameSDKRoot is read from the project folder', opts, () => {
  const projDir = dirOf('rel');
  mkdirSync(join(projDir, 'sdk', 'Client'), { recursive: true });
  writeFileSync(join(projDir, 'sdk', 'Client', 'build_cl.txt'), '130045');
  const r = build({ GameSDKRoot: 'sdk', PluginGameBuiltCL: '127004' }, projDir);
  assert.ok(r.ok, r.out);
  assert.equal(r.define, def(130045));
  assert.deepEqual(r.warnings, ['PLUGINCL004']);
});

test('the props imported before the include path is set: the build passes and warns', opts, () => {
  // The marker is read where the props are imported; a later StarRuptureSDKConfigDir points elsewhere.
  const a = makeSdk('early-a', { Client: '127004' });
  const b = makeSdk('early-b', { Client: '130045' });
  const projDir = dirOf('early');
  mkdirSync(projDir, { recursive: true });
  const proj = join(projDir, 'test.proj');
  writeFileSync(proj, PROJ.replace('<Import Project="' + PROPS + '" />', '<Import Project="' + PROPS + '" />\n  <PropertyGroup><StarRuptureSDKConfigDir>' + join(b, 'Client') + '\\</StarRuptureSDKConfigDir></PropertyGroup>'));
  const r = spawnSync(MSBUILD, [proj, '-nologo', '-v:minimal', '-nodeReuse:false', `-p:GameSDKRoot=${a}`], { windowsHide: true, encoding: 'utf8', cwd: projDir });
  const out = `${r.stdout || ''}${r.stderr || ''}`;
  assert.equal(r.status, 0, out);
  assert.match(out, /warning PLUGINCL005/);
  assert.match(out, /DEFINES=PLUGIN_GAME_BUILT_CL=127004u;/);
});

const REAL_SDK = join(SDK_ROOT, 'StarRupture SDK');
test('this repo\'s own Game-SDK: Client 130045, Server 127004, a stale pin only warns', { skip: !MSBUILD ? 'MSBuild not found (set MSBUILD)' : !existsSync(join(REAL_SDK, 'Client', 'build_cl.txt')) ? 'submodule not checked out' : false }, () => {
  const c = build({ GameSDKRoot: REAL_SDK, Configuration: 'Client Release' });
  assert.ok(c.ok, c.out);
  assert.equal(c.define, def(130045));
  const s = build({ GameSDKRoot: REAL_SDK, Configuration: 'Server Release' });
  assert.ok(s.ok, s.out);
  assert.equal(s.define, def(127004));
  const stale = build({ GameSDKRoot: REAL_SDK, Configuration: 'Client Release', PluginGameBuiltCL: '127004' });
  assert.ok(stale.ok, stale.out);
  assert.equal(stale.define, def(130045));
  assert.deepEqual(stale.warnings, ['PLUGINCL004']);
});
