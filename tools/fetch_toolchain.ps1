# tools/fetch_toolchain.ps1 -- the Windows arm of tools/fetch_toolchain.sh: the PINNED wasm32 toolchain into
# tools/toolchain/ (gitignored), sha256-verified against tools/toolchain/PINS.sha256 (the SAME pin file the bash
# fetcher reads; the Windows x86_64 rows). Native PowerShell 5.1+ / 7: no Git Bash, no MSYS2, no curl.exe needed.
#
#   powershell -ExecutionPolicy Bypass -File tools\fetch_toolchain.ps1 [-Force] [-Verify]
#
#   wasi-sdk 24.0   (clang 18 + wasm-ld + the wasi-libc/libc++ sysroot)  -> tools\toolchain\wasi-sdk\   (bin\clang.exe ...)
#   wabt     1.0.42 (wasm2c, wasm-objdump, wasm-validate)                -> tools\toolchain\wabt\       (bin\wasm2c.exe ...)
#   libclang 18.1.1 python wheel (gen_vtable_slots.py)              -> tools\toolchain\libclang\   (…\clang\native\libclang.dll)
#
# The archives are .tar.gz: Windows 10 1803+ ships bsdtar as C:\Windows\System32\tar.exe, which this script uses; the
# wheel is a zip (Expand-Archive). A download whose digest does not match its pin is DELETED and the script stops
# (exit 4); an artifact without a pin stops it too (exit 3) -- never an unpinned toolchain. -Force re-extracts even
# when the tools are present (a half-extracted tree); -Verify only re-hashes what is already downloaded.
#
#
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$Verify
)
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Dest = Join-Path $Here "toolchain"
$Pins = Join-Path $Dest "PINS.sha256"

$WasiSdkVersion = "24.0"
$WasiSdkTag     = "wasi-sdk-24"
$WabtVersion    = "1.0.42"
$LibclangVersion = "18.1.1"

if (-not [Environment]::Is64BitOperatingSystem) { throw "fetch_toolchain.ps1: a 64-bit Windows host is required (x86_64 pins only)" }
$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne "AMD64") { throw "fetch_toolchain.ps1: unsupported host architecture '$arch' (PINS.sha256 carries Windows x86_64 only; ARM64 Windows is not a lane)" }

$WasiTgz = "wasi-sdk-$WasiSdkVersion-x86_64-windows.tar.gz"
$WasiUrl = "https://github.com/WebAssembly/wasi-sdk/releases/download/$WasiSdkTag/$WasiTgz"
$WabtTgz = "wabt-$WabtVersion-windows-x64.tar.gz"
$WabtUrl = "https://github.com/WebAssembly/wabt/releases/download/$WabtVersion/$WabtTgz"
$LibclangWhl = "libclang-$LibclangVersion-py2.py3-none-win_amd64.whl"
$LibclangUrl = "https://files.pythonhosted.org/packages/py2.py3/l/libclang/$LibclangWhl"

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
if (-not (Test-Path $Pins)) { throw "fetch_toolchain.ps1: $Pins missing (a checkout without tools/toolchain/PINS.sha256?)" }

function Get-Pin([string]$name) {
    # one "<sha256>  <file>" line per artifact; comments start with '#'
    foreach ($line in Get-Content $Pins) {
        if ($line -match '^\s*#' -or $line -match '^\s*$') { continue }
        $parts = $line -split '\s+', 2
        if ($parts.Count -eq 2 -and $parts[1].Trim() -eq $name) { return $parts[0].Trim().ToLower() }
    }
    return $null
}

function Fetch([string]$url, [string]$name) {
    $file = Join-Path $Dest $name
    if (Test-Path $file) { return }
    if ($Verify) { throw "fetch_toolchain.ps1: -Verify but $name is not downloaded" }
    Write-Host "fetch_toolchain: downloading $name"
    $part = "$file.part"
    if (Test-Path $part) { Remove-Item -Force $part }
    # TLS 1.2 for PowerShell 5.1 (github.com and files.pythonhosted.org refuse older); -UseBasicParsing for 5.1 without IE.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $savedProgress = $ProgressPreference
    $ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest's progress bar makes a 100 MB download ten times slower on 5.1
    try {
        Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing -MaximumRedirection 10
    } finally { $ProgressPreference = $savedProgress }
    Move-Item -Force $part $file
}

function Verify-Pin([string]$name) {
    $file = Join-Path $Dest $name
    $want = Get-Pin $name
    if (-not $want) { throw "fetch_toolchain.ps1: NO PIN for $name in PINS.sha256 -- refusing (exit 3)"; }
    $got = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
    if ($want -ne $got) {
        Remove-Item -Force $file
        Write-Error "fetch_toolchain.ps1: sha256 MISMATCH for $name`n  want $want`n  got  $got`n  (the download was deleted; a proxy or a truncated transfer -- run again; if it repeats, the upstream asset changed: do NOT edit the pin, report it)"
        exit 4
    }
    Write-Host "fetch_toolchain: verified $name"
}

Fetch $WasiUrl $WasiTgz;         Verify-Pin $WasiTgz
Fetch $WabtUrl $WabtTgz;         Verify-Pin $WabtTgz
Fetch $LibclangUrl $LibclangWhl; Verify-Pin $LibclangWhl
if ($Verify) { Write-Host "fetch_toolchain: pins OK (-Verify: nothing extracted)"; exit 0 }

$tar = Join-Path $env:SystemRoot "System32\tar.exe"
if (-not (Test-Path $tar)) { $tar = (Get-Command tar -ErrorAction SilentlyContinue).Source }
if (-not $tar) { throw "fetch_toolchain.ps1: no tar.exe (Windows 10 1803+ ships bsdtar in System32; or install Git for Windows and put its usr\bin on PATH)" }

function Extract-Tgz([string]$name, [string]$dirName, [string]$probe) {
    $dir = Join-Path $Dest $dirName
    if (-not $Force -and (Test-Path (Join-Path $dir $probe))) { return }
    $tmp = "$dir.tmp"
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Write-Host "fetch_toolchain: extracting $name -> $dirName\"
    # bsdtar: the archive's single top-level directory is stripped (as the bash fetcher's --strip-components=1)
    & $tar -xzf (Join-Path $Dest $name) -C $tmp --strip-components=1
    if ($LASTEXITCODE -ne 0) { throw "fetch_toolchain.ps1: tar failed on $name ($LASTEXITCODE)" }
    if (-not (Test-Path (Join-Path $tmp $probe))) { throw "fetch_toolchain.ps1: $name did not contain $probe after extraction (archive layout changed?)" }
    Move-Item -Force $tmp $dir
}

Extract-Tgz $WasiTgz "wasi-sdk" "bin\clang.exe"
Extract-Tgz $WabtTgz "wabt" "bin\wasm2c.exe"

$libclangDir = Join-Path $Dest "libclang"
$libclangProbe = Join-Path $libclangDir "libclang-$LibclangVersion.data\platlib\clang\cindex.py"
if ($Force -or -not (Test-Path $libclangProbe)) {
    $tmp = "$libclangDir.tmp"
    if (Test-Path $libclangDir) { Remove-Item -Recurse -Force $libclangDir }
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Write-Host "fetch_toolchain: extracting $LibclangWhl -> libclang\"
    # a wheel is a zip; Expand-Archive wants the .zip extension
    $zip = Join-Path $Dest "$LibclangWhl.zip"
    Copy-Item -Force (Join-Path $Dest $LibclangWhl) $zip
    try { Expand-Archive -Path $zip -DestinationPath $tmp -Force } finally { Remove-Item -Force $zip }
    if (-not (Test-Path (Join-Path $tmp "libclang-$LibclangVersion.data\platlib\clang\cindex.py"))) { throw "fetch_toolchain.ps1: the libclang wheel did not contain clang/cindex.py" }
    Move-Item -Force $tmp $libclangDir
}

& (Join-Path $Dest "wasi-sdk\bin\clang.exe") --version | Select-Object -First 1
& (Join-Path $Dest "wabt\bin\wasm2c.exe") --version
Write-Host "fetch_toolchain: OK -> $Dest\{wasi-sdk,wabt,libclang}"
