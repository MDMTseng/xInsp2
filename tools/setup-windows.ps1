<#
.SYNOPSIS
    One-click setup of xInsp2's required (and optional) C++ toolchain on Windows.

.DESCRIPTION
    Installs what the backend needs to BUILD and to RUN (it compiles inspection
    scripts / project plugins on the fly with cl.exe):

      Required:  MSVC Build Tools (Desktop C++ workload) + OpenCV
      Optional:  libjpeg-turbo (-IncludeOptional)   [Intel IPP is manual]

    Required items use the same default locations the backend probes, so no
    environment variables are needed afterwards. Re-runnable: anything already
    present is detected and skipped. Finishes with a health check.

    See docs/guides/install.md for the manual walkthrough + the in-editor
    "Project Settings -> C++ Toolchain" panel.

.PARAMETER IncludeOptional
    Also install libjpeg-turbo (fast JPEG encode; otherwise stb fallback).

.PARAMETER OpenCvVersion
    OpenCV release to fetch (default 4.10.0). Extracted to C:\opencv.

.PARAMETER SkipVS
    Skip the Visual Studio Build Tools step (e.g. you already have full VS).

.PARAMETER SkipOpenCV
    Skip the OpenCV step.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1 -IncludeOptional
#>
[CmdletBinding()]
param(
    [switch]$IncludeOptional,
    [string]$OpenCvVersion = "4.10.0",
    [switch]$SkipVS,
    [switch]$SkipOpenCV
)

$ErrorActionPreference = "Stop"
function Info($m){ Write-Host "[setup] $m" -ForegroundColor Cyan }
function Ok($m)  { Write-Host "  OK   $m" -ForegroundColor Green }
function Warn($m){ Write-Host "  WARN $m" -ForegroundColor Yellow }
function Err($m) { Write-Host "  FAIL $m" -ForegroundColor Red }

# --- preconditions --------------------------------------------------------
if ($env:OS -ne "Windows_NT") { Err "Windows-only. (Linux setup is deferred — see docs/design/linux-port.md.)"; exit 1 }
$winget = Get-Command winget -ErrorAction SilentlyContinue
if (-not $winget) {
    Err "winget not found. Install 'App Installer' from the Microsoft Store, then re-run."
    exit 1
}
$elevated = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltinRole]::Administrator)

# --- 1. MSVC Build Tools (Desktop C++ workload) ---------------------------
function Test-Vcvars {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -find "VC\Auxiliary\Build\vcvars64.bat" 2>$null | Select-Object -First 1
}
if (-not $SkipVS) {
    Info "Checking MSVC C++ toolchain..."
    $vcvars = Test-Vcvars
    if ($vcvars) { Ok "MSVC already present: $vcvars" }
    else {
        if (-not $elevated) { Warn "VS Build Tools install needs Administrator. Re-run this script elevated, or install VS manually." }
        Info "Installing Visual Studio 2022 Build Tools + Desktop C++ workload (this is large)..."
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
            --accept-source-agreements --accept-package-agreements `
            --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        $vcvars = Test-Vcvars
        if ($vcvars) { Ok "MSVC installed: $vcvars" } else { Err "MSVC still not detected — install the 'Desktop development with C++' workload manually." }
    }
}

# --- 2. OpenCV (required) -------------------------------------------------
# winget has no clean OpenCV that lays out build\x64\vcXX\lib, so fetch the
# official self-extracting release and unpack to C:\opencv (the probe default).
function Test-OpenCV {
    foreach ($p in @("C:\opencv\opencv\build","C:\opencv\build")) {
        if (Test-Path (Join-Path $p "include\opencv2\core.hpp")) { return $p }
    }
    if ($env:OpenCV_DIR -and (Test-Path (Join-Path $env:OpenCV_DIR "..\..\..\include\opencv2\core.hpp"))) { return $env:OpenCV_DIR }
    return $null
}
if (-not $SkipOpenCV) {
    Info "Checking OpenCV..."
    $cv = Test-OpenCV
    if ($cv) { Ok "OpenCV already present: $cv" }
    else {
        $url = "https://github.com/opencv/opencv/releases/download/$OpenCvVersion/opencv-$OpenCvVersion-windows.exe"
        $sfx = Join-Path $env:TEMP "opencv-$OpenCvVersion-windows.exe"
        Info "Downloading OpenCV $OpenCvVersion ..."
        try { Invoke-WebRequest -Uri $url -OutFile $sfx -UseBasicParsing }
        catch { Err "OpenCV download failed ($url). Download manually and extract to C:\opencv."; }
        if (Test-Path $sfx) {
            Info "Extracting to C:\opencv (self-extracting archive)..."
            & $sfx -o"C:\opencv" -y | Out-Null   # 7z SFX args
            Remove-Item $sfx -ErrorAction SilentlyContinue
            $cv = Test-OpenCV
            if ($cv) { Ok "OpenCV installed: $cv" } else { Err "OpenCV extracted but core.hpp not found under C:\opencv." }
        }
    }
}

# --- 3. Optional: libjpeg-turbo ------------------------------------------
if ($IncludeOptional) {
    Info "Installing libjpeg-turbo (optional accelerator)..."
    if (Test-Path "C:\libjpeg-turbo64\include\turbojpeg.h") { Ok "libjpeg-turbo already present." }
    else {
        winget install --id libjpeg-turbo.libjpeg-turbo.VC -e --accept-source-agreements --accept-package-agreements
        if (Test-Path "C:\libjpeg-turbo64\include\turbojpeg.h") { Ok "libjpeg-turbo installed." } else { Warn "libjpeg-turbo not at C:\libjpeg-turbo64 — set TURBOJPEG_ROOT if elsewhere." }
    }
    Info "Intel IPP has no winget package — install manually from Intel and it auto-detects under C:\Intel\ipp\<ver>."
}

# --- 4. Health summary ----------------------------------------------------
Info "Toolchain health:"
function Check($label,$path,$required){
    if ($path) { Ok ("{0,-16} {1}" -f $label, $path) }
    elseif ($required) { Err ("{0,-16} MISSING (required)" -f $label) }
    else { Warn ("{0,-16} not installed (optional)" -f $label) }
}
Check "MSVC vcvars"   (Test-Vcvars) $true
Check "OpenCV"        (Test-OpenCV) $true
$tj = if (Test-Path "C:\libjpeg-turbo64\include\turbojpeg.h") { "C:\libjpeg-turbo64" } else { $null }
Check "libjpeg-turbo" $tj $false
$ipp = (Get-ChildItem "C:\Intel\ipp" -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName "include\ippi.h") } | Select-Object -First 1).FullName
Check "Intel IPP"     $ipp $false

Write-Host ""
Info "Done. Open a project in VS Code and check Project Settings -> C++ Toolchain to verify in-editor."
