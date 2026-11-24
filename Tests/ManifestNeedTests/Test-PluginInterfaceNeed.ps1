# Checks how plugin-release.yml works out interface_version_min for the update manifest.
# It cuts Get-PluginInterfaceNeed out of the workflow file itself (between the
# "BEGIN/END plugin-interface-need" markers), builds small plugin projects in a temp
# folder and runs the function against each. No release runs. Exit code is the number of
# failed cases.
#
#   powershell -File Tests\ManifestNeedTests\Test-PluginInterfaceNeed.ps1

$ErrorActionPreference = 'Stop'
$workflow = Join-Path $PSScriptRoot '..\..\.github\workflows\plugin-release.yml'
$text = [System.IO.File]::ReadAllText((Resolve-Path $workflow))
$cut = [regex]::Match($text, '(?s)# BEGIN plugin-interface-need\r?\n(.*?)# END plugin-interface-need')
if (-not $cut.Success) { throw "plugin-interface-need markers not found in plugin-release.yml" }
$code = ($cut.Groups[1].Value -split '\r?\n' | ForEach-Object { $_ -replace '^ {10}', '' }) -join "`n"
. ([scriptblock]::Create($code))

# A header the way the workflow sees it after the interface change.
$header = @'
#define PLUGIN_INTERFACE_VERSION_MIN 70
#define PLUGIN_INTERFACE_VERSION_MAX 80
#define PLUGIN_INTERFACE_BASELINE 75
#define PLUGIN_INTERFACE_VERSION 80
'@
$oldHeader = $header -replace '(?m)^#define PLUGIN_INTERFACE_BASELINE.*\r?\n', ''

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("need-fixtures-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null

function New-Plugin([string]$name, [hashtable]$files) {
  $dir = Join-Path $root $name
  foreach ($rel in $files.Keys) {
    $path = Join-Path $dir $rel
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    [System.IO.File]::WriteAllText($path, $files[$rel])
  }
  return $dir
}

function Vcxproj([string[]]$groups) {
  '<?xml version="1.0" encoding="utf-8"?>' + "`n" + '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">' + "`n" + ($groups -join "`n") + "`n</Project>"
}
function DefGroup([string]$cond, [string]$defs) {
  $c = if ($cond) { " Condition=`"'`$(Configuration)|`$(Platform)'=='$cond'`"" } else { '' }
  "  <ItemDefinitionGroup$c><ClCompile><PreprocessorDefinitions>$defs</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>"
}

$converted   = 'PluginInfo info{}; info.interfaceVersion = PLUGIN_DECLARED_INTERFACE;'
$unconverted = 'PluginInfo info{}; info.interfaceVersion = PLUGIN_INTERFACE_VERSION;'

$cases = @()
function Case([string]$name, [string]$dir, $expect, [string]$hdr = $header) {
  $script:cases += [pscustomobject]@{ Name = $name; Dir = $dir; Expect = $expect; Header = $hdr }
}

Case 'define in a vcxproj PreprocessorDefinitions list' (New-Plugin 'vcx-list' @{
  'P.vcxproj' = Vcxproj @(DefGroup "Client Release|x64" 'WIN32;NDEBUG;PLUGIN_NEEDS_INTERFACE=77;%(PreprocessorDefinitions)')
  'src\plugin.cpp' = $converted }) 77

Case 'define in a props file, no condition' (New-Plugin 'props' @{
  'Shared.props' = Vcxproj @(DefGroup '' 'PLUGIN_NEEDS_INTERFACE=78;%(PreprocessorDefinitions)')
  'P.vcxproj' = Vcxproj @()
  'src\plugin.cpp' = $converted }) 78

Case 'per-config entries that agree (Client, Server, Debug)' (New-Plugin 'cfg-agree' @{
  'P.vcxproj' = Vcxproj @(
    (DefGroup 'Client Debug|x64' 'PLUGIN_NEEDS_INTERFACE=77'),
    (DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE=77'),
    (DefGroup 'Server Release|x64' 'WIN32;PLUGIN_NEEDS_INTERFACE=77'))
  'src\plugin.cpp' = $converted }) 77

Case 'per-config entries that disagree (Client 77, Server 78)' (New-Plugin 'cfg-disagree' @{
  'P.vcxproj' = Vcxproj @(
    (DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE=77'),
    (DefGroup 'Server Release|x64' 'PLUGIN_NEEDS_INTERFACE=78'))
  'src\plugin.cpp' = $converted }) 'ERROR:differs between configurations'

Case 'Debug differs from Release: Release value used, warning' (New-Plugin 'cfg-debug' @{
  'P.vcxproj' = Vcxproj @(
    (DefGroup 'Client Debug|x64' 'PLUGIN_NEEDS_INTERFACE=80'),
    (DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE=77'))
  'src\plugin.cpp' = $converted }) 77

Case 'Debug-only entry: Release has none, baseline used' (New-Plugin 'debug-only' @{
  'P.vcxproj' = Vcxproj @((DefGroup 'Client Debug|x64' 'PLUGIN_NEEDS_INTERFACE=80'))
  'src\plugin.cpp' = $converted }) 75

Case '#define in a header' (New-Plugin 'define-header' @{
  'P.vcxproj' = Vcxproj @()
  'src\config.h' = "#pragma once`n#define PLUGIN_NEEDS_INTERFACE 79 // uses the host rules`n"
  'src\plugin.cpp' = $converted }) 79

Case '#define in a comment is ignored' (New-Plugin 'define-comment' @{
  'P.vcxproj' = Vcxproj @()
  'src\config.h' = "// #define PLUGIN_NEEDS_INTERFACE 79`n/*`n#define PLUGIN_NEEDS_INTERFACE 79`n*/`n"
  'src\plugin.cpp' = $converted }) 75

Case 'CMake target_compile_definitions' (New-Plugin 'cmake' @{
  'CMakeLists.txt' = "add_library(p SHARED a.cpp)`ntarget_compile_definitions(p PRIVATE PLUGIN_NEEDS_INTERFACE=77) # need`n"
  'src\plugin.cpp' = $converted }) 77

Case 'not set, converted plugin: baseline' (New-Plugin 'none-converted' @{
  'P.vcxproj' = Vcxproj @((DefGroup 'Client Release|x64' 'WIN32;NDEBUG'))
  'src\plugin.cpp' = $converted }) 75

Case 'not set, unconverted plugin: header PLUGIN_INTERFACE_VERSION' (New-Plugin 'none-unconverted' @{
  'P.vcxproj' = Vcxproj @((DefGroup 'Client Release|x64' 'WIN32;NDEBUG'))
  'src\plugin.cpp' = $unconverted }) 80

Case 'unconverted plugin against the header that has no baseline yet' (New-Plugin 'none-unconverted-oldhdr' @{
  'src\plugin.cpp' = $unconverted }) 80 $oldHeader

Case 'converted plugin against a header without baseline' (New-Plugin 'converted-oldhdr' @{
  'src\plugin.cpp' = $converted }) 'ERROR:no PLUGIN_INTERFACE_BASELINE' $oldHeader

Case 'need set but PluginInfo still uses the version macro' (New-Plugin 'need-unconverted' @{
  'P.vcxproj' = Vcxproj @((DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE=77'))
  'src\plugin.cpp' = $unconverted }) 'ERROR:no source file fills PluginInfo'

Case 'out of range, too high (81)' (New-Plugin 'high' @{
  'P.vcxproj' = Vcxproj @((DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE=81'))
  'src\plugin.cpp' = $converted }) 'ERROR:outside the 70 to 80'

Case 'out of range, too low (60)' (New-Plugin 'low' @{
  'P.vcxproj' = Vcxproj @((DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE=60'))
  'src\plugin.cpp' = $converted }) 'ERROR:outside the 70 to 80'

Case 'not a number (property reference)' (New-Plugin 'nan' @{
  'P.vcxproj' = Vcxproj @((DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE=$(NeedLevel)'))
  'src\plugin.cpp' = $converted }) 'ERROR:plain whole number'

Case 'define without a value' (New-Plugin 'novalue' @{
  'P.vcxproj' = Vcxproj @((DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE'))
  'src\plugin.cpp' = $converted }) 'ERROR:plain whole number'

Case 'set outside PreprocessorDefinitions (AdditionalOptions)' (New-Plugin 'addopts' @{
  'P.vcxproj' = '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemDefinitionGroup><ClCompile><AdditionalOptions>/DPLUGIN_NEEDS_INTERFACE=77</AdditionalOptions></ClCompile></ItemDefinitionGroup></Project>'
  'src\plugin.cpp' = $converted }) 'ERROR:outside a PreprocessorDefinitions'

Case 'SDK checkout and build output are not read' (New-Plugin 'skipdirs' @{
  'P.vcxproj' = Vcxproj @()
  'src\plugin.cpp' = $converted
  'plugin-sdk\include\plugin_interface.h' = "#define PLUGIN_NEEDS_INTERFACE PLUGIN_INTERFACE_BASELINE`n"
  'plugin-sdk\ExamplePlugin\E.vcxproj' = Vcxproj @((DefGroup 'Client Release|x64' 'PLUGIN_NEEDS_INTERFACE=99'))
  'build\Client Release\junk.props' = Vcxproj @((DefGroup '' 'PLUGIN_NEEDS_INTERFACE=99')) }) 75

Case 'comment mentioning PLUGIN_DECLARED_INTERFACE does not count' (New-Plugin 'comment-converted' @{
  'src\plugin.cpp' = "// move to PLUGIN_DECLARED_INTERFACE later`n$unconverted`n" }) 80

# The header in this checkout (no baseline until the interface change lands).
$real = Join-Path $PSScriptRoot '..\..\include\plugin_interface.h'
if (Test-Path $real) {
  Case 'real include\plugin_interface.h, unconverted plugin' (New-Plugin 'real-header' @{ 'src\plugin.cpp' = $unconverted }) 'REAL' ([System.IO.File]::ReadAllText((Resolve-Path $real)))
}

$failed = 0
foreach ($c in $cases) {
  $out = @(); $err = $null
  try { $out = @(Get-PluginInterfaceNeed -PluginRoot $c.Dir -HeaderText $c.Header 6>&1) } catch { $err = $_.Exception.Message }
  $value = $out | Where-Object { $_ -is [int] } | Select-Object -Last 1
  $msgs = @($out | Where-Object { $_ -is [System.Management.Automation.InformationRecord] } | ForEach-Object { "$($_.MessageData)" })
  if ($c.Expect -is [int]) {
    $ok = ($null -eq $err) -and ($value -eq $c.Expect)
    $got = if ($err) { "ERROR: $err" } else { "$value" }
  } elseif ($c.Expect -eq 'REAL') {
    $ok = ($null -eq $err) -and ($value -is [int]) -and ($value -ge 70)
    $got = if ($err) { "ERROR: $err" } else { "$value" }
  } else {
    $pattern = $c.Expect.Substring(6)
    $ok = ($null -ne $err) -and ($err -match [regex]::Escape($pattern))
    $got = if ($err) { "ERROR: $err" } else { "no error, value $value" }
  }
  if (-not $ok) { $failed++ }
  $tag = if ($ok) { 'PASS' } else { 'FAIL' }
  $want = if ($c.Expect -is [int]) { "$($c.Expect)" } else { "$($c.Expect)" }
  Write-Output "[$tag] $($c.Name)"
  Write-Output "       expected: $want"
  Write-Output "       got:      $got"
  foreach ($m in $msgs) { Write-Output "       log:      $m" }
}
Remove-Item -Recurse -Force $root
Write-Output ""
Write-Output "$($cases.Count - $failed) of $($cases.Count) passed"
exit $failed
