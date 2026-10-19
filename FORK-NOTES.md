# This fork

A fork of [nxnative](https://github.com/seedds/nxnative) (MIT) with fixes and tools developed while
bringing up **Ace Attorney 123 Trilogy** (`0100CB000A142000`) and **Apollo Justice 456 Trilogy**
(`010020D01B890000`) on Apple Silicon macOS.

Nothing from any game is in this repository: no ROMs, no keys, no extracted data, no patched binaries.
You need your own legally obtained copy of the game and your own `prod.keys`; this project does not
provide or explain how to obtain either. Not affiliated with Nintendo or any game publisher.

## What this fork adds

1. **Save/load crash fix for Ace Attorney 123** (`dm/tools/patch_nso.py`, `CUTS` table)
   - The game's save coroutine dies with `NullReferenceException` inside `DateTime.get_Now`
     (`main+0x960f80`). On this port the guest's "current time" comes back as 0, the cached local
     time difference is 0 too, so the fast path reuses a null boxed `TimeSpan` and the unbox throws.
   - Fix: one instruction — `0x961018` `b.gt 0x96103c` → `b 0x96103c`, forcing the recompute path.
   - Optional: `OBS` entry 68 injects the host's local time into the coroutine's `DateTime` result so
     save slots show a real date instead of `0001/01/01`.

2. **XCI (game card image) support in `nx_extract`** (`dm/tools/nx_extract/Program.cs`)
   - Opens `secure` / `normal` partitions, reads the NACP from the control NCA for title id, name and icon.
   - `--info` lists the NCAs, `--no-exefs` extracts only the RomFS.

3. **Apollo Justice 456 Trilogy** (`010020D01B890000`, RE Engine) is playable: the host implements the
   sparse virtual memory pools, the NVN command stream and the shaders the engine needs; court scenes,
   cross-examination, the Court Record with its 3D evidence viewer, the pause menu and save/load all work at
   30 fps. Bring-up notes: [docs/aa456-bringup.md](docs/aa456-bringup.md).

4. **Profiles** for `0100CB000A142000` and `010020D01B890000`, both working.

5. **Render resolution override** (opt-in, `DM_GAME_RES=<w>x<h>`): the game's own output-size constants are
   patched in the guest image, so the engine really renders above 1080p instead of upsampling. Native
   `DM_GAME_RES=1920x1080` stays the default (at higher sizes the game's 1080p character art shows a thin
   seam on some animation frames).

6. **Mac front end polish**: the window sizes itself to the picture (1:1, pixel exact), keeps 16:9 while
   dragging, mouse clicks become touch points (`nn::hid` touch screen), keyboard + controller input, one-click
   fullscreen.

## Building and running (Ace Attorney 123)

```bash
# host
make -C dm/host GAME=0100CB000A142000 dmhost

# rewrite the modules (patches only; never touches the original files)
~/projects/nxnative-venv/bin/python dm/tools/patch_nso.py \
  <build>/extract/v196608/ExeFS/main <build>/patched_v196608 main

# run
DM_WINDOW=1 DM_AUDIO=1 DM_KEEP_FOCUS=1 DM_NVN_CBDEFER=1 \
DM_MSL_DIR=<shader cache> \
DM_ROOT=<build> DM_ROMFS=<build>/extract/v196608/RomFS \
DM_SAVE=<build>/run/save DM_TEMP=<build>/run/temp DM_FONT_DIR=<build>/font \
./dm/host/dmhost <build>/patched_v196608
```

## Running Ace Attorney 456

```bash
make -C dm/host GAME=010020D01B890000 dmhost

DM_WINDOW=1 DM_AUDIO=1 \
DM_MSL_DIR=<build>/work/cache DM_ROMFS=<RomFS> \
DM_SAVE=<build>/run/save DM_TEMP=<build>/run/temp \
dm/host/dmhost <build>/patched_v65536
```

The guest modules only need the stock rewriter output (`patch_nso.py ... <build>/patched_v65536 main`); the
resolution override and the touch screen are implemented on the host side, not in the rewriter.

See the upstream `README.md` and `docs/` for the general workflow (survey, profiles, shader pipeline).

## Licence

MIT, same as upstream. Third-party code keeps its own licence (see `dm/third_party/README.md`).
