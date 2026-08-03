# PsyDoom for Nintendo 3DS

A port of [PsyDoom](https://github.com/BodbDearg/PsyDoom) - Brad Harding's reverse engineered source port of
PlayStation Doom - to the Nintendo 3DS, running natively on the console's own hardware.

**You need your own copy of PlayStation Doom.** No game data is distributed here. You point the build at a disc you
own and it produces the installable packages on your machine.

Doom, Final Doom and the GEC Master Edition are all supported, each built as its own title.

## Building

See **[tools/3ds/README.md](tools/3ds/README.md)** for the full instructions. In short:

```sh
# Linux and macOS
./tools/3ds/build.sh --disc-dir ~/psx-doom-discs
```

```powershell
# Windows
.\tools\3ds\build.ps1 -DiscDir 'D:\psx-doom-discs'
```

You get a `.3dsx` to run from the Homebrew Launcher and a `.cia` to install to the HOME Menu, in `dist/<edition>/`.

Requires devkitPro with `3ds-dev`, CMake, Python 3 and FFmpeg.

## Platform Behavior

- Top screen: game output at 400x240 through a native libctru framebuffer presenter.
- The status bar can live on either screen. Moving it to the touch screen gives its rows to the 3D view.
- Bottom screen during gameplay: independently rendered automap geometry, always on. Drag it with the stylus to pan
  around; it snaps back to the player once they walk away from where it was pulled off them.
- Bottom screen in menus: the menu items themselves, touchable, with the logo or screen title moved up to the top
  screen. Tap a row to select it; tap the selected row again to act on it.
- The main menu draws itself twice, once per screen, so the top screen gets the logo on a full background of its own
  and the touch screen gets the options on a full background of its own. Nothing straddles a seam between them.
- Bottom screen on the title screen, the intro logos and the loading and connecting plaques: blank.
- Bottom screen on the intermission, the finale text screens and the credits: a second pass over just that screen's
  background. The top screen shows the screen exactly as the PlayStation composed it; the touch screen gets a whole
  image of its own rather than a copy of the top one.
- Two player local wireless for co-op and deathmatch, replacing the PlayStation link cable.
- Audio: SDL N3DS NDSP backend.
- Input: native Circle Pad, C-Stick, face buttons, D-pad, shoulders, ZL/ZR, Start, and Select.
- Storage: compressed disc data is extracted once to `sdmc:/3ds/PsyDoom/<variant>/disc`.
- New 3DS: SDL requests higher clock speed and L2 cache automatically.
- Original 3DS: remains a supported build target, pending hardware performance validation.

The first launch needs free space for both the installed package and the extracted disc image. Extraction resumes safely after interruption by retaining already-complete files.

## If it will not start

Every step of startup writes a line to `sdmc:/3ds/PsyDoom/launch.log`, flushed as it goes, so the log survives even if
the game dies immediately afterwards. If something goes wrong, that file says how far it got and what failed. It is
rewritten from scratch on every launch, so copy it before trying again.

A fatal error now puts the message on the top screen and waits for START. Previously it called `abort`, which returns
to the HOME Menu straight away - and because the process asked to exit, the system had no reason to write a crash dump
either, so the failure left nothing behind at all.

Installed CIAs get their permissions from the title's own exheader, rather than inheriting them from whatever launched
them the way a `.3dsx` does. `packaging/PsyDoom.rsf` therefore declares the memory arrangement, the services the game
opens by name, and the New 3DS clock and L2 cache request. None of that was declared before, which is why a CIA could
behave differently from the same build run as homebrew, and why an installed title had no way to reach New 3DS speeds.

## Memory

An installed title gets a 64 MiB application region. Homebrew launched from another process inherits that process's
region, which is 96 MiB, and that difference is enough to decide whether the game runs at all.

Left alone, libctru reserves 32 MiB of whatever it gets for the linear heap - the memory the GPU and the audio DSP read
directly. This game needs about 2 MiB of that, for the screen framebuffers, the GPU command buffers and the sound
mixing buffers. Under 96 MiB the other 30 MiB went unnoticed; under 64 MiB it left too little behind for the 16 MiB
zone heap the game runs from, so the allocation failed and the process died before drawing anything. The linear heap is
now asked for explicitly at 8 MiB, four times what has ever been measured in use.

## Default Controls

The defaults are chosen so that everything essential is reachable on an Old 3DS/2DS, which has no C-Stick, ZL or ZR.

- Circle Pad: analog move forward/back and turn left/right.
- C-Stick (New 3DS): analog strafe, plus a second move axis.
- D-pad: forward/back and turn; menu and automap navigation.
- A: use/open and menu confirm; automap pan.
- B: run and menu back.
- X/Y: previous/next weapon.
- L: strafe modifier (makes the D-pad turn inputs strafe); automap zoom out.
- R: attack/respawn; automap zoom in.
- ZL/ZR (New 3DS): run / attack.
- Start: pause/start.
- Select: unbound. The touch screen shows the automap for the whole of gameplay, so there is nothing to toggle;
  SELECT is left free for the player to bind to something they actually want.

The touch screen is also a control: menu rows can be tapped directly.

## Control schemes

`Options > Extra Options > Controls` picks between three ready made layouts and shows what each one does:

| Scheme | Layout |
| --- | --- |
| `Modern` (default) | The one described above: the Xbox 360 Doom II layout. |
| `PSX` | The PlayStation layout. The D-Pad moves and turns, the shoulders strafe, X fires, B strafes, A uses, Y runs. The circle pad takes the weapon groups and the C-Stick turns, since the original pad had neither. |
| `PSX Alt` | `PSX` with use and strafe the other way round on B and A. |
| `Custom` | Whatever `control_bindings.ini` says. |

While a scheme is selected it is put back in place on every launch, so the bindings file simply follows it. Change
anything and the selection becomes `Custom` by itself, so your changes are kept rather than overwritten next time. Pick
a scheme again to go back to it. `Reset To Default` puts the `Modern` layout in place as a custom one, ready to edit.

The same screen lists the actions worth rebinding on a handheld, each showing the button it is currently on. Pick one
and press it, or tap it, and the screen waits for you to press whatever you want it bound to; back out to leave it
alone. Anything not listed there is still editable in `control_bindings.ini`.

`Options > Extra Options > Controls` shows this list in game with 3DS style button glyphs. Everything is rebindable
through `sdmc:/3ds/PsyDoom/control_bindings.ini`, which documents how each physical control maps onto SDL's gamepad
input names.

The 3DS config omits desktop-only mouse, window/display, renderer-selection, launcher, and focus-loss controls. Vulkan is not built. Fixed handheld defaults constrain the zone heap, emulated VRAM, SPU RAM, and audio buffering.

## Screen size

`Options > Extra Options > Screen` chooses how the game image fills the top screen:

| Setting | Effect |
| --- | --- |
| `Screen 4 By 3` (default) | The PlayStation's own aspect ratio, leaving black pillarbox bars either side. |
| `Screen Full Width` | The image is stretched across all 400 pixels. Nothing is cropped, but it is wider than the PlayStation's aspect ratio, so the picture is stretched about 37% horizontally. |

Full width costs about 4% of the frame rate, because the blit covers 400x240 pixels instead of 292x240.

## Status bar placement

`Options > Extra Options > Hud` chooses which screen carries the status bar. It applies immediately and is saved.

| Setting | Effect |
| --- | --- |
| `Hud Top Screen` (default) | The status bar sits under the 3D view on the top screen, where the PlayStation had it. The touch screen is all automap. |
| `Hud Touch Top` | The status bar moves to a strip along the top of the touch screen. |
| `Hud Touch Below` | The status bar moves to a strip along the bottom of the touch screen. |

Either touch screen position frees the status bar's 40 rows for the 3D view, which grows from 200 rows to the full
240. This is the same trade vanilla Doom's largest screen size makes, and it is done the same way: `R_SetViewHeight`
changes the view height and regenerates `gYSlope` from it, exactly as `R_ExecuteSetViewSize` does on PC. The horizon
stays where it is, so the extra rows show more world above and below it, and more of the weapon appears from under the
bottom edge - vanilla positions psprites relative to the view centre and clips them at the view height, and so does
this.

The automap shrinks about its own centre to fit the rows the status bar leaves it, rather than being squashed or
cropped, so it keeps its shape and none of it is lost.

## Stereoscopic 3D

The 3D slider drives this directly and there is no setting for it: the slider is itself the hardware switch, so
duplicating it in a menu would only give the player a way to make it stop working.

The 3D view has to be rendered twice, once per eye, so raising the slider roughly halves the frame rate: measured in
Citra, 26.4 FPS at the bottom of the slider against 14.1 FPS at the top. The status bar is only drawn once, since it is
flat and identical for both eyes, and the second eye's render leaves those rows of the framebuffer alone. Nothing extra
is rendered while the slider is at the bottom, so a player who does not want stereo just leaves the slider down and
pays nothing for it.

The eyes are parallel rather than converged, which keeps distant geometry comfortable to look at. Only the 3D view is
stereoscopic; the status bar and all menus are flat.

## Performance and the Detail setting

Shading wall and floor pixels is by far the most expensive thing the ARM11 does, so `Options > Extra Options > Detail`
trades image detail for frame rate. It applies immediately and is saved to `saved_prefs.ini`.

| Setting | What it does | Citra frame time |
| --- | --- | ---: |
| Full | Every pixel is shaded | 50.8 ms (19.7 FPS) |
| Low | Every 2nd screen column is shaded and doubled up, like PC Doom's low detail mode | 37.9 ms (26.4 FPS) |
| Lowest | Every 3rd column, and every 2nd row of wall pixels | 33.6 ms (29.8 FPS) |

Those figures were measured with the 4:3 screen setting and the 3D slider down.

### Why it is not faster still

PsyDoom is not a Doom port; it is a PlayStation Doom port, and that difference is where the frame time goes. PC Doom's
software renderer writes 8-bit palette indices straight to the screen at roughly four instructions per pixel. PsyDoom
runs PlayStation Doom's renderer, which emits PlayStation GPU primitives, and then emulates that GPU in
`simple_gpu`. Every shaded pixel is masked to a texture window, masked to a texture page, converted to a VRAM address,
read out of a 16-bit emulated VRAM, split out of a packed byte pair, looked up in a CLUT, modulated by the primitive
colour and written back. That is five to eight times the per pixel work of a native Doom port, which is why a port like
PrBoom3DS runs considerably faster on the same hardware while looking less like the PlayStation game.

Measured split of a frame at `Low` detail (per frame, in milliseconds):

| Stage | Time |
| --- | ---: |
| Floors and ceilings | 11.4 |
| Top screen blit | 6.7 |
| Sky | 6.1 |
| Walls | 4.6 |
| Sprites | 2.5 |
| Status bar | 2.5 |
| Bottom screen automap | 1.2 |
| BSP traversal | 0.3 |

BSP traversal - the part people usually think of as "the Doom engine" - is 0.8% of the frame. Everything else is
pixel shading in the emulated GPU.

Sprites, the status bar, menus and the automap are always drawn at full detail.

The default is automatic: `Low` on a New 3DS (804 MHz with L2 cache) and `Lowest` on an Old 3DS/2DS (268 MHz).
Choosing a level in the menu makes the preference explicit and disables the automatic choice. Set `detailMode` in
`saved_prefs.ini` back to `-1` to return to automatic.

The game simulates at the PlayStation's 15 Hz tick rate regardless of how fast frames are drawn, and elapsed time is
clamped so it can never run faster than real time. A low frame rate makes the game run slower than intended, never
faster.

## Two player local wireless

Co-op and deathmatch run over 3DS local wireless (UDS) instead of the PlayStation link cable. Both consoles need the
same edition of the game.

To start a match: on the first console pick `Game Mode: Co-op` (or `Deathmatch`), the level and the difficulty, and
press START. It will advertise the game and wait. On the second console pick the same game mode and press START; it
finds the waiting console and joins, taking the level and difficulty from the host. There is no host/join prompt -
whichever console starts first hosts, exactly as one end of a link cable waited for the other.

A connection health square appears in the top right of the touch screen while searching or connected:

| Colour | Meaning |
| --- | --- |
| Flashing white | Searching for the other console |
| Green | Round trip under 40 ms; the link is keeping up with the 30 Hz tick |
| Yellow | Round trip 40-120 ms; frames are being spent waiting for the other console |
| Red | Round trip over 120 ms, or nothing received for half a second |

### How desync is avoided

PsyDoom's netgame is strict lockstep: each 30 Hz tick both consoles send the inputs they will use next tick and then
wait for the other's packet, then simulate identically. That model cannot survive a dropped or reordered packet, and
unlike a link cable, wireless drops packets.

Rollback netcode is deliberately not used: rolling back would mean snapshotting and restoring the entire Doom
simulation every tick, which this engine has no facility for. Instead the guarantee is moved into the transport, in
`PsyDoom/NetworkUds3DS.cpp`:

- Every message carries a sequence number and an acknowledgement of the highest contiguous sequence received.
- Unacknowledged messages are retransmitted every 24 ms until acknowledged.
- Out of order arrivals are held in a receive window until the gap ahead of them is filled, so the game only ever
  sees messages in the order they were sent.
- Keep-alives every 100 ms, and the link is declared lost after 5 seconds of silence.

On top of that the engine's own protections still apply: it measures how late each packet was and gradually shifts
its clock to compensate, and it compares a checksum of both players' positions every tick, showing the `NETWORK
ERROR` plaque and resynchronising if the two simulations ever do diverge.

Co-op and deathmatch rules (friendly fire, frag limit, ammo carry-over, and so on) are configurable in
`sdmc:/3ds/PsyDoom/multiplayer_cfg.ini`, which is now generated on 3DS.

## Variants

| Build argument | Source directory | SD data directory | Unique ID |
| --- | --- | --- | --- |
| `doom` | `PSX Doom ROM Files/Doom` | `sdmc:/3ds/PsyDoom/doom/disc` | `0xD0010` |
| `final_doom` | `PSX Doom ROM Files/Final Doom` | `sdmc:/3ds/PsyDoom/final_doom/disc` | `0xD0011` |
| `master_edition` | `PSX Doom ROM Files/Master Edition` | `sdmc:/3ds/PsyDoom/master_edition/disc` | `0xD0012` |

Each edition has a distinct SMDH icon, CIA banner, product code, and title ID. Its banner embeds a 2.8-second PCM sample sourced from the corresponding legally obtained PlayStation disc audio.

The scripts never modify source ROM directories. ROM archives, generated packages, and validation captures are ignored by version control.

## Build

Prerequisites:

- devkitPro with devkitARM, libctru, 3ds-tools, zlib, and minizip; set `DEVKITPRO_ROOT` when it is not installed at `C:\devkitPro`.
- `makerom.exe` and `bannertool.exe`; set `PSYDOOM_3DS_TOOLS` when they are not in the default local tools directory.
- Matching legally obtained CUE/BIN files; set `PSYDOOM_ROM_ROOT` when they are not in the default local ROM directory.
- FFmpeg on `PATH` for banner-audio generation.

Build one edition:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-3ds.ps1 -Variant doom
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-3ds.ps1 -Variant final_doom
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-3ds.ps1 -Variant master_edition
```

Build all editions:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-all-3ds.ps1
```

Outputs are written only to `dist/<variant>`. Use `-ForceRomfs` to recreate an archive and `-SkipCia` for development-only 3DSX builds. The manual private workflow in `.github/workflows/build-3ds.yml` runs the same scripts on a legally provisioned self-hosted Windows runner. See `VALIDATION.md` for measured Citra results and remaining hardware gates.

## Credits and licence

PsyDoom is by [BodbDearg](https://github.com/BodbDearg); this is a 3DS port of that work. Doom is id Software's.
See `LICENSE` and `CONTRIBUTORS.md`, both carried over from upstream unchanged.

The 3DS specific work lives in `game/Platform_3DS.*`, `game/Main_3DS.cpp`, `game/PsyDoom/Screens3DS.*`,
`game/PsyDoom/ControlSchemes3DS.*`, `game/PsyDoom/NetworkUds3DS.*`, `game/Doom/UI/controls3ds_main.*`,
`game/Doom/UI/menu3ds.*`, and behind `PSYDOOM_3DS` throughout the engine.
