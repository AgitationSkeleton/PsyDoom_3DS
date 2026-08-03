<#
.SYNOPSIS
    Builds PsyDoom for the Nintendo 3DS on Windows.

.DESCRIPTION
    You supply your own PlayStation Doom disc. Nothing copyrighted is in this repository, so the disc image, the game
    data extracted from it and the banner music taken from its audio track are all produced here from your own copy.

    See tools/3ds/README.md for the full explanation.

.EXAMPLE
    .\tools\3ds\build.ps1 -DiscDir 'D:\PSX Doom Discs'

.EXAMPLE
    .\tools\3ds\build.ps1 -DiscDir 'D:\PSX Doom Discs' -Edition doom -SkipCia
#>
param(
    [Parameter(Mandatory = $true)]
    [string] $DiscDir,

    [ValidateSet('doom', 'final_doom', 'master_edition')]
    [string[]] $Edition = @('doom', 'final_doom', 'master_edition'),

    [ValidateRange(1, 64)]
    [int] $Jobs = [Environment]::ProcessorCount,

    [switch] $SkipCia,

    [switch] $ForceRomfs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# ------------------------------------------------------------------------------------------------------------------
# What each edition is called, and how to recognise its disc
# ------------------------------------------------------------------------------------------------------------------
$Editions = @{
    doom           = @{ Stem = 'PsyDoom-Doom';           Title = 'PsyDoom';                ProductCode = 'CTR-H-PDM1'; UniqueId = '0xD0010'; CuePattern = 'doom';   CueName = 'Doom.cue' }
    final_doom     = @{ Stem = 'PsyDoom-Final-Doom';     Title = 'PsyDoom Final Doom';     ProductCode = 'CTR-H-PDM2'; UniqueId = '0xD0011'; CuePattern = 'final';  CueName = 'FinalDoom.cue' }
    master_edition = @{ Stem = 'PsyDoom-Master-Edition'; Title = 'PsyDoom Master Edition'; ProductCode = 'CTR-H-PDM3'; UniqueId = '0xD0012'; CuePattern = 'master'; CueName = 'PSXDOOM_BETA_4.cue' }
}

function Fail([string] $Message) { throw $Message }

# ------------------------------------------------------------------------------------------------------------------
# Prerequisites
# ------------------------------------------------------------------------------------------------------------------
$DevkitPro = if ($env:DEVKITPRO) { $env:DEVKITPRO } else { 'C:\devkitPro' }

if (-not (Test-Path -LiteralPath (Join-Path $DevkitPro 'devkitARM'))) {
    Fail "No devkitARM under $DevkitPro. Install devkitPro with the 3ds-dev package, or set DEVKITPRO."
}

$ToolchainFile = Join-Path $DevkitPro 'cmake\3DS.cmake'
if (-not (Test-Path -LiteralPath $ToolchainFile)) { Fail "Missing $ToolchainFile. Update devkitPro." }

# devkitPro ships its own cmake under msys2; prefer that, since the toolchain file expects msys style paths
$Cmake = Join-Path $DevkitPro 'msys2\usr\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $Cmake)) { $Cmake = (Get-Command cmake.exe -ErrorAction Stop).Source }

foreach ($tool in @('ffmpeg.exe', 'python.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool is not on PATH. ffmpeg cuts the banner music from your disc; python packs the game data."
    }
}

if (-not (Test-Path -LiteralPath $DiscDir)) { Fail "Disc folder does not exist: $DiscDir" }

# makerom and bannertool are not part of devkitPro. Look for them, and say clearly what to do if they are absent.
$MakeRom = $null
$BannerTool = $null

if (-not $SkipCia) {
    $binDir = Join-Path $PSScriptRoot 'bin'

    foreach ($candidate in @(
        (Get-Command makerom.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
        (Join-Path $binDir 'makerom.exe')
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { $MakeRom = $candidate; break }
    }

    foreach ($candidate in @(
        (Get-Command bannertool.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
        (Join-Path $binDir 'bannertool.exe')
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { $BannerTool = $candidate; break }
    }

    if ((-not $MakeRom) -or (-not $BannerTool)) {
        Fail @"
makerom.exe and bannertool.exe are needed to build a .cia and were not found.

Put them on PATH, or in tools\3ds\bin\. Prebuilt Windows binaries are published by their authors:
  makerom     https://github.com/3DSGuy/Project_CTR/releases
  bannertool  https://github.com/Steveice10/bannertool/releases

Or pass -SkipCia to build only the .3dsx, which needs neither.
"@
    }
}

# ------------------------------------------------------------------------------------------------------------------
# devkitPro's toolchain file is written for its own MSYS environment, so every path handed to CMake - including
# DEVKITPRO and DEVKITARM themselves - has to be in MSYS form. Windows form fails inside the toolchain file rather
# than at the command line, which makes it look like a CMake bug rather than a path one.
# ------------------------------------------------------------------------------------------------------------------
function ConvertTo-MsysPath([string] $Path) {
    $resolved = [System.IO.Path]::GetFullPath($Path)

    if ($resolved -notmatch '^([A-Za-z]):\\(.*)$') {
        Fail "Cannot convert path to MSYS form: $resolved"
    }

    return '/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2].Replace('\', '/')
}

$env:DEVKITPRO = ConvertTo-MsysPath $DevkitPro
$env:DEVKITARM = ConvertTo-MsysPath (Join-Path $DevkitPro 'devkitARM')

# ------------------------------------------------------------------------------------------------------------------
# Finding the disc
# ------------------------------------------------------------------------------------------------------------------
function Find-EditionCue([string] $EditionName) {
    $pattern = $Editions[$EditionName].CuePattern

    Get-ChildItem -LiteralPath $DiscDir -Filter *.cue -Recurse -File | ForEach-Object {
        $name = $_.Name.ToLowerInvariant()

        # For plain 'doom' take care not to match 'final doom' or the master edition
        if ($EditionName -eq 'doom' -and ($name -match 'final|master')) { return }
        if ($name -like "*$pattern*") { return $_.FullName }
    } | Select-Object -First 1
}

# ------------------------------------------------------------------------------------------------------------------
# Build
# ------------------------------------------------------------------------------------------------------------------
foreach ($name in $Edition) {
    Write-Host "==> $name"
    $info = $Editions[$name]

    $cue = Find-EditionCue $name
    if (-not $cue) { Fail "No .cue file for '$name' under $DiscDir. See tools/3ds/README.md for what is expected." }
    Write-Host "  disc: $cue"

    # ---- game data ------------------------------------------------------------------------------------------------
    $romfsDir = Join-Path $RepoRoot "romfs\$name\psydoom"
    $archive = Join-Path $romfsDir 'disc.zip'

    if ((Test-Path -LiteralPath $archive) -and (-not $ForceRomfs)) {
        Write-Host '  disc archive already built (use -ForceRomfs to redo it)'
    } else {
        New-Item -ItemType Directory -Force -Path $romfsDir | Out-Null
        & python (Join-Path $PSScriptRoot 'pack_disc.py') --cue $cue --cue-name $info.CueName --output $archive
        if ($LASTEXITCODE) { Fail 'Packing the disc failed.' }
    }

    # ---- banner audio, cut from the disc's own music track ---------------------------------------------------------
    $pkgDir = Join-Path $RepoRoot "packaging\$name"
    $bannerWav = Join-Path $pkgDir 'banner.wav'

    if (Test-Path -LiteralPath $bannerWav) {
        Write-Host '  banner audio already built'
    } else {
        $cueLines = Get-Content -LiteralPath $cue
        $files = $cueLines | Select-String -Pattern '^\s*FILE\s+"(.+?)"' | ForEach-Object { $_.Matches[0].Groups[1].Value }
        $trackBin = if ($files.Count -ge 2) { Join-Path (Split-Path -Parent $cue) $files[1] } else { $null }

        if ($trackBin -and (Test-Path -LiteralPath $trackBin)) {
            Write-Host "  banner audio: from $(Split-Path -Leaf $trackBin)"

            # 16364 Hz stereo, exactly three seconds. Not preferences: banner audio outside them plays as noise.
            & ffmpeg -hide_banner -loglevel error -y `
                -f s16le -ar 44100 -ac 2 -ss 5 -i $trackBin `
                -t 3 -af 'volume=0.55,afade=t=in:st=0:d=0.10,afade=t=out:st=2.75:d=0.25' `
                -ar 16364 -ac 2 -c:a pcm_s16le $bannerWav
            if ($LASTEXITCODE) { Fail 'Building the banner audio failed.' }

            & python (Join-Path $PSScriptRoot 'check_banner_audio.py') $bannerWav
            if ($LASTEXITCODE) { Fail 'The banner audio is not usable.' }
        } else {
            Write-Warning '  could not find the disc''s music track, so the banner will be silent'
            & ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=16364:cl=stereo -t 3 -c:a pcm_s16le $bannerWav
        }
    }

    # ---- compile ---------------------------------------------------------------------------------------------------
    $buildDir = Join-Path $RepoRoot "build-3ds-$name"
    $distDir = Join-Path $RepoRoot "dist\$name"
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null

    $configureArgs = @(
        '-S', (ConvertTo-MsysPath $RepoRoot),
        '-B', (ConvertTo-MsysPath $buildDir),
        '-G', 'Unix Makefiles',
        "-DCMAKE_TOOLCHAIN_FILE=$(ConvertTo-MsysPath $ToolchainFile)",
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_DEPENDS_USE_COMPILER=FALSE',
        '-DPSYDOOM_INCLUDE_VULKAN_RENDERER=OFF',
        '-DPSYDOOM_INCLUDE_LAUNCHER=OFF',
        "-DPSYDOOM_3DS_VARIANT=$name",
        "-DPSYDOOM_3DS_ROMFS_DIR=$(ConvertTo-MsysPath (Join-Path $RepoRoot "romfs\$name"))",
        "-DPSYDOOM_3DS_ICON=$(ConvertTo-MsysPath (Join-Path $pkgDir 'icon.png'))"
    )

    & $Cmake @configureArgs | Out-Null
    if ($LASTEXITCODE) { Fail 'CMake configure failed.' }

    & $Cmake --build (ConvertTo-MsysPath $buildDir) --parallel $Jobs
    if ($LASTEXITCODE) { Fail 'Build failed.' }

    Copy-Item (Join-Path $buildDir 'game\PsyDoom.3dsx') (Join-Path $distDir "$($info.Stem).3dsx") -Force
    Copy-Item (Join-Path $buildDir 'game\PsyDoom.smdh') (Join-Path $distDir "$($info.Stem).smdh") -Force

    if ($SkipCia) {
        Write-Host "  built $($info.Stem).3dsx (skipping the .cia)"
        continue
    }

    # ---- package ---------------------------------------------------------------------------------------------------
    $bannerBnr = Join-Path $buildDir 'banner.bnr'
    & $BannerTool makebanner -i (Join-Path $pkgDir 'banner.png') -a $bannerWav -o $bannerBnr | Out-Null
    if ($LASTEXITCODE) { Fail 'bannertool failed.' }

    & $MakeRom -f cia -o (Join-Path $distDir "$($info.Stem).cia") -target t -desc app:2.50 `
        -rsf (Join-Path $RepoRoot 'packaging\PsyDoom.rsf') `
        -elf (Join-Path $buildDir 'game\PsyDoom.elf') `
        -icon (Join-Path $buildDir 'game\PsyDoom.smdh') `
        -banner $bannerBnr `
        "-DAPP_TITLE=$($info.Title)" `
        "-DAPP_PRODUCT_CODE=$($info.ProductCode)" `
        "-DAPP_UNIQUE_ID=$($info.UniqueId)" `
        "-DDIR_ROMFS=$(Join-Path $RepoRoot "romfs\$name")"
    if ($LASTEXITCODE) { Fail 'makerom failed.' }

    Write-Host "  built $($info.Stem).3dsx and $($info.Stem).cia"
}

Write-Host ''
Write-Host 'Done. Packages are in dist/:'
foreach ($name in $Edition) {
    Get-ChildItem -LiteralPath (Join-Path $RepoRoot "dist\$name") -File -ErrorAction SilentlyContinue |
        Select-Object Name, Length
}
