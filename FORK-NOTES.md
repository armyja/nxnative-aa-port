# This fork

A fork of [nxnative](https://github.com/seedds/nxnative) (MIT) with fixes and tools developed while
bringing up **Ace Attorney 123 Trilogy** (`0100CB000A142000`), **Apollo Justice 456 Trilogy**
(`010020D01B890000`) and **Ace Attorney Investigations 1&2** (`010005501E68C000`) on Apple Silicon macOS.

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

4. **Ace Attorney Investigations 1&2** (`010005501E68C000`, Unity 2022 IL2CPP) is playable: the game boots
   through its first-run screens into Episode 1, dialogue advances, the Plus menu opens, and save/load works
   with the host's local time. What it took is listed under [Unity (Investigations)](#unity-investigations)
   below.

5. **Profiles** for `0100CB000A142000`, `010020D01B890000` and `010005501E68C000`, all working.

6. **Render resolution override** (opt-in, `DM_GAME_RES=<w>x<h>`): the game's own output-size constants are
   patched in the guest image, so the engine really renders above 1080p instead of upsampling. Native
   `DM_GAME_RES=1920x1080` stays the default (at higher sizes the game's 1080p character art shows a thin
   seam on some animation frames).

7. **Mac front end polish** (right button = A, left = touchscreen, window sizing, focus handling): the window sizes itself to the picture (1:1, pixel exact), keeps 16:9 while
   dragging, mouse clicks become touch points (`nn::hid` touch screen), keyboard + controller input, one-click
   fullscreen.

## Unity (Investigations)

Unity titles need four things the two MT Framework / RE Engine games never used:

- **Shaders live in Addressables bundles.** `dm/gfx/tools/shader_split.py` reads them with
  [UnityPy](https://github.com/K0lb3/UnityPy) when `DM_UNITYPY=1` (`pip install UnityPy`): the release player
  strips type trees and patches its own common-string table, so the repository's UnityFS reader cannot decode
  the Shader objects. A Unity 2022 Switch subprogram holds every stage of a variant as a NVN control section
  followed by its code area, both stages coming from the same subprogram.
- **The managed file API does not use `nn::fs`.** `System.IO.File.Exists` goes through IL2CPP's PAL to the SDK's
  libc `stat`, which opens an fsp-srv session the host does not implement, so Addressables reported "file not
  found" for files that were right there. `dm/host/hle_il2cpp.c` branches the PAL stat entries into the host,
  which answers from the same mount table as `nn::fs`.
- **The process heap must be sized before the title starts.** On hardware the loader does it; here
  `nn::os::AllocateMemoryBlock` failed with a zero heap and Unity's font engine crashed on a 16 MB buffer.
  The profile key `heap_default` (Investigations only) hands out 1 GB at the first svc.
- **Random numbers and the clock** come from services: `nn::crypto` / `nn::os` random bytes and
  `std::chrono::system_clock::now` are patched in the sdk module to the host's `arc4random` and wall clock,
  and `nn::time::StandardUserSystemClock::GetSystemClockContext` answers with the host's local offset, which is
  what the save slots display.

## Where the files go

Every command below uses one variable, `B`, the **build directory of one title**: the folder where the
extraction step put the game and where the host keeps its shader cache, saves and temp files. It is created
by `dm/tools/nx_extract` (or by `scripts/build_ipa_from_nsp.py`) and, in the layout used while developing
this fork, looks like this:

```
~/projects/aa123/0100CB000A142000_build/      # Ace Attorney 123  (B for the commands below)
  extract/v196608/ExeFS/main                  # the untouched guest modules
  extract/v196608/RomFS/                      # the game's RomFS
  patched_v196608/                            # rewriter output (patched copies, originals untouched)
  run/save/  run/temp/                        # save data (systemdata) and scratch space
  font/                                       # dumped shared fonts (only 123 needs this)

~/projects/aa456/010020D01B890000_build/      # Apollo Justice 456
  extract/v65536/                             # (our 456 build keeps the RomFS on an external volume)
  patched_v65536/
  work/cache/                                 # MSL/shader cache produced on the first run
  run/save/  run/temp/
```

Nothing in `patched_*` is ever written back to `extract/`; the rewriter only reads the original modules.

## Building and running (Ace Attorney 123)

Everything below runs **from the repository root** (the directory that contains `dm/` and `scripts/`);
`make -C dm/host`, `dm/tools/patch_nso.py` and `dm/host/dmhost` are all relative to it.
`python3` must have `lz4`, `capstone` and `fonttools` installed (`python3 -m pip install lz4 capstone fonttools`,
or use a virtualenv's interpreter in place of `python3`).

```bash
cd ~/projects/nxnative                         # <- repository root
B=~/projects/aa123/0100CB000A142000_build      # <- the build folder described above

make -C dm/host GAME=0100CB000A142000 dmhost

# rewrite the modules (patches only; never touches the original files)
python3 dm/tools/patch_nso.py \
  "$B/extract/v196608/ExeFS/main" "$B/patched_v196608" main

DM_WINDOW=1 DM_AUDIO=1 DM_KEEP_FOCUS=1 DM_NVN_CBDEFER=1 \
DM_MSL_DIR="$B/shader_cache" \
DM_ROOT="$B" DM_ROMFS="$B/extract/v196608/RomFS" \
DM_SAVE="$B/run/save" DM_TEMP="$B/run/temp" DM_FONT_DIR="$B/font" \
./dm/host/dmhost "$B/patched_v196608"
```

## Running Ace Attorney 456

```bash
cd ~/projects/nxnative                         # <- repository root
B=~/projects/aa456/010020D01B890000_build      # <- the build folder described above
ROMFS=/Volumes/Untitled/aa456/build/RomFS      # wherever the 456 RomFS was extracted

make -C dm/host GAME=010020D01B890000 dmhost

python3 dm/tools/patch_nso.py \
  "$B/extract/v65536/ExeFS/main" "$B/patched_v65536" main

DM_WINDOW=1 DM_AUDIO=1 \
DM_MSL_DIR="$B/work/cache" DM_ROMFS="$ROMFS" \
DM_SAVE="$B/run/save" DM_TEMP="$B/run/temp" \
dm/host/dmhost "$B/patched_v65536"
```

The guest modules only need the stock rewriter output (`patch_nso.py`); the resolution override
(`DM_GAME_RES`), the touch screen and the window handling are implemented on the host side, not in the rewriter.

What the variables mean:

| Variable | Points to |
|---|---|
| `DM_MSL_DIR` | shader cache: 123 uses `$B/shader_cache`, 456 `$B/work/cache` (created on the first run) |
| `DM_ROOT` / `DM_ROMFS` | the extracted game root and its RomFS |
| `DM_SAVE` / `DM_TEMP` | save data (`systemdata`) and scratch space, both inside the build directory |
| `DM_FONT_DIR` | dumped shared fonts (123 only; without it the dialogue text is invisible) |
| `DM_WINDOW=1 DM_AUDIO=1` | open a window and enable audio (`DM_AUDIO` off = silent sink) |
| `DM_KEEP_FOCUS=1` | keep the guest "focused" while the window is not key (automation; drop it when playing) |
| `DM_NVN_CBDEFER=1` | 123's command-buffer deferral (leave it on for 123) |

See the upstream `README.md` and `docs/` for the general workflow (survey, profiles, shader pipeline).

## Running Ace Attorney Investigations

```bash
cd ~/projects/nxnative
B=~/projects/aai_collection                  # XCI unpacked here: extract/{ExeFS,RomFS} and patched_v0
make -C dm/host GAME=010005501E68C000 dmhost
DM_WINDOW=1 DM_AUDIO=1 DM_MSL_DIR="$B/work/cache" DM_ROMFS="$B/extract/RomFS" \
DM_SAVE="$B/run/save" DM_TEMP="$B/run/temp" \
dm/host/dmhost "$B/patched_v0"
```

The shader cache under `$B/work/cache/b-<chain>` is built by `dm/gfx/tools/dmsh_build.py` from the tables
`shader_split.py` writes into the profile; the chain version is in `dm/games/010005501E68C000/dmsh_chain.h`.

## Licence

MIT, same as upstream. Third-party code keeps its own licence (see `dm/third_party/README.md`).
