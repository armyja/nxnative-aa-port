# Ace Attorney 456 bring-up notes (research, not yet running)

Status: the code modules load and the game runs its own initialisation, then aborts while setting up
its memory pools. Everything below was measured on this fork's host build; no game data is needed to
follow the reasoning, and no game content is reproduced here.

## Title

| | |
|---|---|
| Title ID | `010020D01B890000` |
| Name | Apollo Justice: Ace Attorney Trilogy |
| Display version | 1.0.0 |
| Engine | RE Engine (RomFS is `re_chunk_000.pak` + `natives/NSW/runtime.args`) |
| `main` build id | `dbe49182fbca27691c65b23c9e105d1f` |
| `sdk` build id | `8618895246b420329b77746b960a66ab` |
| `subsdk0` build id | `368f01d5cb42f06cd125dc739d036719` |

## Extraction

`nx_extract` reads game-card images (`.xci`) directly: the `secure` partition holds the base game,
the `normal` partition holds the control data (NACP: title id, name, icon) and the update NCAs.

```bash
nx_extract <prod.keys> <game>.xci - <out dir> --info      # list NCAs per partition
nx_extract <prod.keys> <game>.xci - <out dir> --no-exefs  # RomFS only
```

On a network share, read performance is dominated by random access (~3 MB/s); copy the image to a
local disk first, then extract (14 GB image → ~90 s).

## Profile and build

`dm/games/010020D01B890000/game.tsv` is provisional: identity, build ids, modules, `romfs_marker`,
and memory/NVN constants copied from another profile. The host builds, the three modules rewrite,
and the game reaches its own initialisation.

## Where it stops

```
nn::diag::Abort (host HLE, Result 0)
  <- sdk+0x11ff34      abort wrapper
  <- main+0x57fdd80    return address of the failing call
  <- main+0x2bff788    RE Engine init: computes a size, then creates a pool
```

The initialisation at `main+0x2bff730` computes `(a - b) & ~0x1fffff` (a 2 MB aligned size, 8 MB in
practice), stores it in a global, then calls `0x57fdd40(pointer, size)`. That call funnels through an
import table (`main+0xa27a8f8 -> sdk+0x11fe74`), which validates the request:

```
sdk+0x11fe74   ldrb w9,[x0]; cbnz w9, abort          state byte must be 0
               start == 0 -> sdk+0x11fed4            "map anywhere"
               bl 0x5c38e0  (-> sdk+0x12cb10)        precondition, bit 0 must be set
               bl 0x5c4020  (-> sdk+0x11e5e4)        map, Result must be 0
sdk+0x11e5e4   size != 0, page aligned, [state+0x28] == 0
sdk+0x11e6a8   bl 0x5c3f50  (-> sdk+0x111b00)        VAMM address-range allocator
```

Measured arguments: `start = 0`, `size = 0x800000`.

## The failing interface

With `DM_QMLOG=1` the host prints every `svcQueryMemory` with its guest caller:

```
QueryMemory 0x16f...  caller sdk+0xdff9e8
QueryMemory 0x0       caller sdk+0xdff9e8
```

The address looks like a **host stack pointer**: guest code runs on the host stack on this port, so
an address-space manager that probes the stack to decide what is free sees a region that does not
exist in the emulated guest address space. The host answers "unknown", the allocator concludes there
is no room, and RE Engine aborts.

## Ruled out

- **Pool size**: raising `mem_reserve` / `app_pool` from 4 GB / 3.2 GB to 6 GB / 5 GB changes nothing.
- **Unimplemented NVN calls**: no red-stub hits are logged before the abort.
- **Reporting the host stack as a Stack region**: `hle_svc.c` now recognises the host stack range in
  `QueryMemory` (state 3), but the abort is unchanged, so this alone is not sufficient.

## Next steps

1. Disassemble `sdk+0xdff9e8` and determine which field of the `QueryMemory` result it consumes, and
   why it probes a stack address at all (it may be testing "does this pointer belong to the heap").
2. Then either make `QueryMemory` answer usefully for such addresses, or give the guest its own stack
   in the guest address space instead of reusing the host stack.
3. After the pool exists, the next unknown is graphics: RE Engine keeps its shaders inside the pak,
   so the shader extraction/translation path will need work of its own.
