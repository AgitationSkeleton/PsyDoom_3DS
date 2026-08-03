# Building PsyDoom for the Nintendo 3DS

You need your own copy of PlayStation Doom. Nothing from the game is in this repository: the disc image, the data
extracted from it and even the banner music are all produced on your machine, from the disc you point the build at.

## What you need

| | |
| --- | --- |
| **devkitPro** with the `3ds-dev` package | The compiler and 3DS libraries. https://devkitpro.org/wiki/Getting_Started |
| **CMake** 3.16 or newer | |
| **Python** 3.7 or newer | Packs the disc into the archive the game extracts on first run |
| **FFmpeg** | Cuts three seconds of banner music from your disc's audio track |
| **makerom** and **bannertool** | Only for building a `.cia`. See below. |

`makerom` and `bannertool` are not part of devkitPro:

- **Linux and macOS** — the build script fetches and builds them into `tools/3ds/bin/` the first time it needs them.
- **Windows** — download the prebuilt binaries from
  [makerom](https://github.com/3DSGuy/Project_CTR/releases) and
  [bannertool](https://github.com/Steveice10/bannertool/releases), and put them on `PATH` or in `tools/3ds/bin/`.

Neither is needed if you only want a `.3dsx` to run from the Homebrew Launcher: pass `--skip-cia` / `-SkipCia`.

## Your discs

Rip each disc you own to a `.cue` and its `.bin` track files, and put them all in one folder. The build works out which
disc is which from the cue file's name:

| Edition | Recognised by | Example |
| --- | --- | --- |
| `doom` | a cue mentioning "doom", but not "final" or "master" | `Doom (USA).cue` |
| `final_doom` | a cue mentioning "final" | `Final Doom (USA).cue` |
| `master_edition` | a cue mentioning "master" | `PSXDOOM_BETA_4 Master Edition.cue` |

The name does not otherwise matter: the cue is renamed inside the archive to whatever the game looks for.

You only need the discs for the editions you want to build. Use `--edition` to build one at a time.

## Building

**Linux and macOS**

```sh
./tools/3ds/build.sh --disc-dir ~/psx-doom-discs
```

**Windows** (PowerShell)

```powershell
.\tools\3ds\build.ps1 -DiscDir 'D:\psx-doom-discs'
```

Everything lands in `dist/<edition>/`:

- `PsyDoom-Doom.3dsx` — run from the Homebrew Launcher
- `PsyDoom-Doom.cia` — install with FBI to get an icon on the HOME Menu

Useful options:

| Shell | PowerShell | What it does |
| --- | --- | --- |
| `--edition doom` | `-Edition doom` | Build one edition rather than all three |
| `--jobs 4` | `-Jobs 4` | Parallel compile jobs |
| `--skip-cia` | `-SkipCia` | Only build the `.3dsx`, no `makerom` needed |
| `--force-romfs` | `-ForceRomfs` | Repack the disc archive even if one is already built |

## First run on the console

The packages carry the disc image compressed inside them, and the game unpacks it to
`sdmc:/3ds/PsyDoom/<edition>/disc` the first time it starts. That takes a few minutes and needs room on the card for
both the package and the extracted copy. It resumes safely if interrupted.

## If something goes wrong

Every startup writes to `sdmc:/3ds/PsyDoom/launch.log`, flushed as it goes, so it survives even if the game dies
immediately afterwards. It is rewritten on each launch, so copy it before trying again. A fatal error also shows its
message on the top screen rather than dropping straight back to the HOME Menu.

## Notes

- Banner audio has to be 16 bit stereo PCM at 16364 Hz and no longer than three seconds. Outside that it does not warn,
  it just plays ringing noise. The build checks what it produces; see `packaging/BANNER_ASSETS.md`.
- `packaging/<edition>/banner.png` and `icon.png` are the artwork used for the HOME Menu. Replace them if you like:
  256x128 and 48x48 respectively.
