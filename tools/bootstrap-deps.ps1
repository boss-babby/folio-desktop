# Folio - fetch the Windows prebuilts. PowerShell twin of bootstrap-deps.sh.
#
# Idempotent and user-scoped: by default it uses %LOCALAPPDATA%\folio-deps;
# build-windows-clean.ps1 overrides FOLIO_DEPS_DIR into its unique TEMP tree
# and removes that tree after packaging. No administrator rights required.
#
#   powershell -ExecutionPolicy Bypass -File tools\bootstrap-deps.ps1
#
# Both binary packages are pinned and checked by SHA-256. Never use a
# floating release: PDFium's public header/API and its import library must
# match the same revision as the Linux/Android builds.

$ErrorActionPreference = 'Stop'

$SkiaTag    = 'm144-e2e6623374-4'
$SkiaAsset  = "Skia-$SkiaTag-windows-Release-x64.zip"
$SkiaUrl    = "https://github.com/JetBrains/skia-pack/releases/download/$SkiaTag/$SkiaAsset"
$SkiaSha256 = '50869b098a4f72f5843687093a1a8fdd94e5d61d98397024bb4e521227fedcba'
$PdfiumTag = 'chromium/7568'
$PdfiumAsset = 'pdfium-win-x64.tgz'
$PdfiumSha256 = 'be2945e35d7efbdd70ecf9b29053e46a927ff98aa5b3c8ab52584d6e7d92c07b'
$PdfiumUrl = "https://github.com/bblanchon/pdfium-binaries/releases/download/$PdfiumTag/$PdfiumAsset"

# %LOCALAPPDATA%\folio-deps by default. FOLIO_DEPS_DIR overrides it, which
# is what CI uses: the Actions cache needs a path it can name in YAML, and
# the `env` context does not expose the runner's LOCALAPPDATA.
$Root = if ($env:FOLIO_DEPS_DIR) { $env:FOLIO_DEPS_DIR }
        else { Join-Path $env:LOCALAPPDATA 'folio-deps' }
New-Item -ItemType Directory -Force -Path $Root | Out-Null

function Get-Archive($Url, $OutFile, $Sha256) {
    if (Test-Path $OutFile) {
        $found = (Get-FileHash -Algorithm SHA256 $OutFile).Hash.ToLowerInvariant()
        if ($found -eq $Sha256) {
            Write-Host "  cached  $(Split-Path $OutFile -Leaf)"
            return
        }
        Write-Host "  stale   $(Split-Path $OutFile -Leaf); deleting checksum mismatch"
        Remove-Item -Force $OutFile
    }
    Write-Host "  fetch   $(Split-Path $OutFile -Leaf)"
    # TLS 1.2 for Windows PowerShell 5.1, whose default still excludes it.
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
    $found = (Get-FileHash -Algorithm SHA256 $OutFile).Hash.ToLowerInvariant()
    if ($found -ne $Sha256) {
        Remove-Item -Force $OutFile
        throw "Archive checksum mismatch: $Url (expected $Sha256, got $found)"
    }
}

Write-Host 'Folio dependencies (Windows x64)'

# --- Skia --------------------------------------------------------------------
$SkiaDir = Join-Path $Root 'skia'
if (-not (Test-Path (Join-Path $SkiaDir 'out\Release-windows-x64\skia.lib'))) {
    $zip = Join-Path $Root $SkiaAsset
    Get-Archive $SkiaUrl $zip $SkiaSha256
    Write-Host '  unpack  skia'
    if (Test-Path $SkiaDir) { Remove-Item -Recurse -Force $SkiaDir }
    Expand-Archive -Path $zip -DestinationPath $SkiaDir -Force
    if (-not (Test-Path (Join-Path $SkiaDir 'out\Release-windows-x64\skia.lib')) -or
        -not (Test-Path (Join-Path $SkiaDir 'include\core\SkCanvas.h'))) {
        throw 'Skia archive lacks the expected m144 Windows x64 layout'
    }
} else {
    Write-Host '  present skia'
}

# --- PDFium ------------------------------------------------------------------
$PdfiumDir = Join-Path $Root 'pdfium'
$pdfiumVersion = Join-Path $PdfiumDir 'VERSION'
if (-not (Test-Path (Join-Path $PdfiumDir 'lib\pdfium.dll.lib')) -or
    -not (Test-Path (Join-Path $PdfiumDir 'bin\pdfium.dll')) -or
    -not (Test-Path $pdfiumVersion) -or
    -not (Select-String -Path $pdfiumVersion -Pattern '^BUILD=7568$' -Quiet)) {
    $tgz = Join-Path $Root 'pdfium-win-x64-7568.tgz'
    Get-Archive $PdfiumUrl $tgz $PdfiumSha256
    Write-Host '  unpack  pdfium'
    if (Test-Path $PdfiumDir) { Remove-Item -Recurse -Force $PdfiumDir }
    New-Item -ItemType Directory -Force -Path $PdfiumDir | Out-Null
    # tar.exe ships with Windows 10 1803+ and handles .tgz natively.
    tar -xzf $tgz -C $PdfiumDir
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path (Join-Path $PdfiumDir 'lib\pdfium.dll.lib')) -or
        -not (Test-Path (Join-Path $PdfiumDir 'bin\pdfium.dll')) -or
        -not (Test-Path (Join-Path $PdfiumDir 'include\fpdf_annot.h'))) {
        throw 'PDFium archive extraction failed or expected files are missing'
    }
} else {
    Write-Host '  present pdfium'
}

# --- PoDoFo ------------------------------------------------------------------
#
# No official Windows prebuilt exists, so this is the one dependency that
# needs vcpkg. With FOLIO_WITH_PODOFO=OFF, the narrower PDFium writer
# handles PDF copy export; page-tree editing remains PoDoFo-only.
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
Write-Host "  cmake -B build -G `"Visual Studio 17 2022`" -A x64 -DFOLIO_WITH_PODOFO=OFF -DFOLIO_DEPS_DIR=$Root"
Write-Host '  cmake --build build --config Release'
