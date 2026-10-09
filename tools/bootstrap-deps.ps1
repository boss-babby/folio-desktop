# Folio - fetch the Windows prebuilts. PowerShell twin of bootstrap-deps.sh.
#
# Idempotent and user-scoped: everything lands in %LOCALAPPDATA%\folio-deps,
# nothing is installed system-wide and nothing needs administrator rights.
#
#   powershell -ExecutionPolicy Bypass -File tools\bootstrap-deps.ps1
#
# Versions are pinned. Skia's prebuilt is the only hard pin that matters:
# the archive has to match the headers the code was written against, and
# skia-pack rebuilds tags in place often enough that "latest" is a trap.

$ErrorActionPreference = 'Stop'

$SkiaTag    = 'm144-e2e6623374-4'
$SkiaAsset  = "Skia-$SkiaTag-windows-Release-x64.zip"
$SkiaUrl    = "https://github.com/JetBrains/skia-pack/releases/download/$SkiaTag/$SkiaAsset"
$PdfiumTag  = 'chromium/7442'
$PdfiumUrl  = "https://github.com/bblanchon/pdfium-binaries/releases/latest/download/pdfium-win-x64.tgz"

# %LOCALAPPDATA%\folio-deps by default. FOLIO_DEPS_DIR overrides it, which
# is what CI uses: the Actions cache needs a path it can name in YAML, and
# the `env` context does not expose the runner's LOCALAPPDATA.
$Root = if ($env:FOLIO_DEPS_DIR) { $env:FOLIO_DEPS_DIR }
        else { Join-Path $env:LOCALAPPDATA 'folio-deps' }
New-Item -ItemType Directory -Force -Path $Root | Out-Null

function Get-Archive($Url, $OutFile) {
    if (Test-Path $OutFile) {
        Write-Host "  cached  $(Split-Path $OutFile -Leaf)"
        return
    }
    Write-Host "  fetch   $(Split-Path $OutFile -Leaf)"
    # TLS 1.2 for Windows PowerShell 5.1, whose default still excludes it.
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
}

Write-Host 'Folio dependencies (Windows x64)'

# --- Skia --------------------------------------------------------------------
$SkiaDir = Join-Path $Root 'skia'
if (-not (Test-Path (Join-Path $SkiaDir 'out\Release-x64\skia.lib'))) {
    $zip = Join-Path $Root $SkiaAsset
    Get-Archive $SkiaUrl $zip
    Write-Host '  unpack  skia'
    if (Test-Path $SkiaDir) { Remove-Item -Recurse -Force $SkiaDir }
    Expand-Archive -Path $zip -DestinationPath $SkiaDir -Force
} else {
    Write-Host '  present skia'
}

# --- PDFium ------------------------------------------------------------------
$PdfiumDir = Join-Path $Root 'pdfium'
if (-not (Test-Path (Join-Path $PdfiumDir 'lib\pdfium.dll.lib'))) {
    $tgz = Join-Path $Root 'pdfium-win-x64.tgz'
    Get-Archive $PdfiumUrl $tgz
    Write-Host '  unpack  pdfium'
    New-Item -ItemType Directory -Force -Path $PdfiumDir | Out-Null
    # tar.exe ships with Windows 10 1803+ and handles .tgz natively.
    tar -xzf $tgz -C $PdfiumDir
} else {
    Write-Host '  present pdfium'
}

# --- PoDoFo ------------------------------------------------------------------
#
# No official Windows prebuilt exists, so this is the one dependency that
# needs vcpkg. It is optional: FOLIO_WITH_PODOFO=OFF drops the annotation
# *writing* path and leaves everything else working, which is the right
# default for a first Windows build.
$Podofo = Join-Path $Root 'vcpkg\installed\x64-windows\lib\podofo.lib'
if (Test-Path $Podofo) {
    Write-Host '  present podofo (vcpkg)'
} else {
    Write-Host '  skip    podofo - build with -DFOLIO_WITH_PODOFO=OFF, or:'
    Write-Host "            git clone https://github.com/microsoft/vcpkg $Root\vcpkg"
    Write-Host "            $Root\vcpkg\bootstrap-vcpkg.bat"
    Write-Host "            $Root\vcpkg\vcpkg install podofo:x64-windows"
}

Write-Host ''
Write-Host "Dependencies in: $Root"
Write-Host 'Configure with:'
Write-Host "  cmake -B build -G `"Visual Studio 17 2022`" -A x64 -DFOLIO_DEPS_DIR=$Root"
Write-Host '  cmake --build build --config Release'
