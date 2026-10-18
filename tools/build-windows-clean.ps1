# Build and verify a portable Windows x64 Folio package without generating
# build/dependency/staging files inside the source checkout.
# Run on Windows with VS 2022 C++ workload + CMake >= 3.28 (no admin needed):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-windows-clean.ps1
# The only output outside %TEMP% is the final Folio-windows-x64.zip next to
# the source checkout, or -OutputPath when specified.
[CmdletBinding()]
param(
    [string]$OutputPath,
    [ValidateSet('MSVC', 'ClangCL')][string]$Compiler = 'MSVC'
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputPath) {
    $OutputPath = Join-Path (Split-Path $repo -Parent) 'Folio-windows-x64.zip'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ([IO.Path]::GetExtension($OutputPath) -ne '.zip') {
    throw 'OutputPath must name a .zip package'
}
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'A real Windows host/runner with Windows SDK is required; Linux cannot verify this build'
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake >= 3.28 is required'
}
if (-not (Get-Command robocopy -ErrorAction SilentlyContinue)) {
    throw 'Windows robocopy is required to stage a complete copy of uncommitted source'
}

# Unique run directory: no concurrent run can delete another run's files.
# Never delete a caller-supplied path or any source-tree directory.
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('folio-windows-' + [guid]::NewGuid().ToString('N'))
if ($scratch.StartsWith(($repo.TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'TEMP resolves inside the source checkout; move TEMP outside it before building'
}
if ($OutputPath.StartsWith(($scratch.TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Final ZIP path cannot be inside the temporary build tree'
}
$oldDeps = $env:FOLIO_DEPS_DIR
$oldPath = $env:PATH
$oldCI = $env:CI

function Invoke-Checked([string]$Program, [string[]]$ToolArgs) {
    & $Program @ToolArgs
    if ($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)" }
}

try {
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    $source = Join-Path $scratch 'source'
    $build = Join-Path $scratch 'build'
    $deps = Join-Path $scratch 'deps'
    $stage = Join-Path $scratch 'package'
    New-Item -ItemType Directory -Force -Path $source, $deps, $stage | Out-Null

    # git archive HEAD would silently OMIT the uncommitted/new source files in
    # this workspace. Robocopy includes them, but excludes all generated and
    # final outputs. Build tools may generate tokens in this TEMP source copy.
    & robocopy $repo $source /E /NFL /NDL /NJH /NJS /NP `
        /XD .git build .cxx .gradle .cache node_modules dist out target `
        /XF *.apk *.zip
    if ($LASTEXITCODE -ge 8) { throw "Source staging failed (robocopy $LASTEXITCODE)" }
    foreach ($path in @('CMakeLists.txt', 'tools\bootstrap-deps.ps1',
                        'assets\fonts', 'documents', 'docs\RUNNING-WINDOWS.txt')) {
        if (-not (Test-Path (Join-Path $source $path))) {
            throw "Staged source is missing $path"
        }
    }

    # CMake regenerates Tokens.h from the JSON in this staged checkout.
    # An Actions checkout containing the new PageStage.cpp but an older
    # design-tokens.json silently overwrites the correct header and only
    # reports C2039 after the entire dependency/bootstrap and C++ build.
    $pageStage = Join-Path $source 'folio-ui\components\PageStage.cpp'
    $tokensJson = Join-Path $source 'design-tokens.json'
    $tokensHeader = Join-Path $source 'folio-ui\include\folio\ui\Tokens.h'
    if ((Select-String -LiteralPath $pageStage -Pattern 'tokens::dur::desktopZoom' -SimpleMatch -Quiet)) {
        $durations = (Get-Content -LiteralPath $tokensJson -Raw | ConvertFrom-Json).motion.duration
        if (-not $durations.PSObject.Properties['desktopZoom'] -or
            -not (Select-String -LiteralPath $tokensHeader -Pattern 'int desktopZoom' -SimpleMatch -Quiet)) {
            throw ('Incomplete source revision: PageStage.cpp uses tokens::dur::desktopZoom, ' +
                   'but the staged design-tokens.json and regenerated Tokens.h must BOTH define it. ' +
                   'Commit/sync all three files from the same source revision before rerunning Windows CI.')
        }
    }

    $env:FOLIO_DEPS_DIR = $deps
    Invoke-Checked 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $source 'tools\bootstrap-deps.ps1'))
    foreach ($path in @('skia\out\Release-windows-x64\skia.lib',
                        'pdfium\lib\pdfium.dll.lib', 'pdfium\bin\pdfium.dll')) {
        if (-not (Test-Path (Join-Path $deps $path))) { throw "Missing dependency $path" }
    }
    # CTest executables are in the build tree, not beside pdfium.dll. Make
    # the temporary DLL visible to tests; restore the caller's PATH in finally.
    $env:PATH = (Join-Path $deps 'pdfium\bin') + [IO.Path]::PathSeparator + $oldPath

    $cmakeArgs = @('-S', $source, '-B', $build,
                   '-DFOLIO_BUILD_UI=ON', '-DFOLIO_BUILD_TESTS=ON',
                   '-DFOLIO_WITH_PODOFO=OFF', "-DFOLIO_DEPS_DIR=$deps")
    if ($Compiler -eq 'MSVC') {
        $cmakeArgs += @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    } else {
        # Call from an x64 VS developer shell (or the msvc-dev-cmd Actions
        # step). clang-cl still needs the MSVC linker libraries and SDK.
        if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
            throw 'ClangCL needs clang-cl plus the x64 VS developer environment'
        }
        $cmakeArgs += @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                        '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl')
    }
    Invoke-Checked 'cmake' $cmakeArgs
    $cache = Join-Path $build 'CMakeCache.txt'
    foreach ($required in @('^FOLIO_PLATFORM_BACKEND:INTERNAL=win32$',
                             '^FOLIO_BUILD_UI:BOOL=ON$',
                             '^FOLIO_BUILD_TESTS:BOOL=ON$',
                             '^FOLIO_WITH_PODOFO:BOOL=OFF$')) {
        if (-not (Select-String -Path $cache -Pattern $required -Quiet)) {
            throw "Unexpected Windows configuration: missing $required"
        }
    }
    Invoke-Checked 'cmake' @('--build', $build, '--config', 'Release', '--parallel', '2')

    # CTest exits 0 when there are NO tests. Also fail if PDF/Skia/UI test
    # targets silently vanish: a green core-only run is not a full Windows
    # reader build. CMake 3.28+ provides this machine-readable inventory.
    $testJson = & ctest --test-dir $build -C Release --show-only=json-v1
    if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate the Windows test suite' }
    $tests = @(($testJson -join "`n" | ConvertFrom-Json).tests | ForEach-Object { $_.name })
    if ($tests.Count -lt 19) {
        throw "Only $($tests.Count) Windows tests discovered; expected at least 19"
    }
    foreach ($name in @('engine.pdfium', 'engine.pdfium.export',
                         'ui.pdfium.saveas', 'ui.pdfium.visible-tiles',
                         'ui.navigation-recents', 'ui.recent-covers',
                         'ui.partial-redraw', 'render.skia',
                         'integration.pipeline')) {
        if ($tests -notcontains $name) { throw "Required Windows test was not discovered: $name" }
    }
    Invoke-Checked 'ctest' @('--test-dir', $build, '-C', 'Release', '--output-on-failure')

    $exe = if ($Compiler -eq 'MSVC') {
        Join-Path $build 'folio-ui\Release\folio-desktop.exe'
    } else {
        Join-Path $build 'folio-ui\folio-desktop.exe'
    }
    if (-not (Test-Path $exe)) { throw 'The real Windows GUI executable was not built' }
    Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'Folio.exe')
    Copy-Item -LiteralPath (Join-Path $deps 'pdfium\bin\pdfium.dll') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $source 'assets') -Destination (Join-Path $stage 'assets') -Recurse
    Copy-Item -LiteralPath (Join-Path $source 'documents') -Destination (Join-Path $stage 'documents') -Recurse
    Copy-Item -LiteralPath (Join-Path $source 'docs\RUNNING-WINDOWS.txt') -Destination (Join-Path $stage 'README.txt')
    foreach ($path in @('Folio.exe', 'pdfium.dll', 'assets\fonts', 'README.txt')) {
        if (-not (Test-Path (Join-Path $stage $path))) { throw "Incomplete package: $path" }
    }

    # PE OptionalHeader.Subsystem = 2: GUI, not 3: external CMD console.
    $bytes = [IO.File]::ReadAllBytes((Join-Path $stage 'Folio.exe'))
    if ($bytes.Length -lt 256) { throw 'Folio.exe is truncated' }
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($pe -lt 64 -or $pe + 4 + 20 + 70 -gt $bytes.Length -or
        [Text.Encoding]::ASCII.GetString($bytes, $pe, 4) -ne "PE`0`0") {
        throw 'Folio.exe is not a valid PE image'
    }
    if ([BitConverter]::ToUInt16($bytes, $pe + 4 + 20 + 68) -ne 2) {
        throw 'Folio.exe is not a GUI-subsystem binary (would open CMD)'
    }
    $env:PATH = $stage + [IO.Path]::PathSeparator + $oldPath
    $env:CI = '1'   # --help must not open a blocking message box
    Invoke-Checked (Join-Path $stage 'Folio.exe') @('--help')

    $archive = Join-Path $scratch 'Folio-windows-x64.zip'
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $entries = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        foreach ($name in @('Folio.exe', 'pdfium.dll', 'README.txt')) {
            if ($entries -notcontains $name) { throw "Zip is missing $name" }
        }
        if (-not ($entries | Where-Object { $_ -like 'assets/fonts/*' })) {
            throw 'Zip has no fonts'
        }
    } finally { $zip.Dispose() }

    $parent = Split-Path $OutputPath -Parent
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    # Only replace an existing deliverable AFTER the new archive passed all
    # checks; a failed build leaves the previous deliverable untouched.
    $incoming = Join-Path $parent ('.folio-incoming-' + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        Copy-Item -LiteralPath $archive -Destination $incoming
        if ((Get-FileHash -Algorithm SHA256 $archive).Hash -ne
            (Get-FileHash -Algorithm SHA256 $incoming).Hash) { throw 'Package copy checksum mismatch' }
        Move-Item -LiteralPath $incoming -Destination $OutputPath -Force
    } finally {
        if (Test-Path $incoming) { Remove-Item -LiteralPath $incoming -Force }
    }
    Write-Host "Windows package: $OutputPath"
    Write-Host "SHA-256: $((Get-FileHash -Algorithm SHA256 $OutputPath).Hash.ToLowerInvariant())"
} finally {
    $env:FOLIO_DEPS_DIR = $oldDeps
    $env:PATH = $oldPath
    $env:CI = $oldCI
    if (Test-Path $scratch) {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction Stop
        Write-Host "Removed temporary build, source copy, deps and staging: $scratch"
    }
}
