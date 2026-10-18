# Ace Attorney 456 bring-up notes

Status: the game boots to its title screen, menus, game selection and the first episode, advances
dialogue, and saves / loads game data (verified across a restart). In-scene background art and
characters are still black, and the logo / opening movies decode to black frames; both are known
graphics gaps (see "What is left"). Everything below was measured on this fork's host build; no game
content is reproduced here.

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

`dm/games/010020D01B890000/game.tsv` carries the identity, build ids, modules, `romfs_marker`,
memory partition (`mem_reserve` 4 GB, alias / stack 256 MB each, `app_pool` 0xcd500000) and the
NVN constants; the shader tables in the same folder are generated (below). The host builds, the
three modules rewrite, and the game reaches `nnMain`.

## The startup abort (fixed)

The first abort was not in the address-space allocator. `main+0x2bff730` sizes the engine's memory:

```
QueryMemoryInfo(&info)                          ; MemoryInfo[0] = TotalNonSystemMemorySize
                                                ; MemoryInfo[8] = UsedNonSystemMemorySize
SetMemoryHeapSize((total - used) & ~0x1fffff)   ; observed request 0xc9200000
AllocateMemoryBlock(&p, 0x800000)
StandardAllocator::Initialize(p, 0x800000, false)   ; p stayed 0 -> sdk+0x11fe74 aborts
```

The host's `UsedNonSystemMemorySize` counted the heap plus 64 MB but not the module images
(166 + 15 + 7 MB, 0xbe00000 after 2 MB alignment), so `0xc9200000 + 0xbe00000` exceeded the
`0xcd500000` pool: `SetHeapSize` answered 0x10801, the engine does not retry, `AllocateFromHeap`
returned 0x1003 and the pool was built on a null pointer. `hle_svc.c` now reports
`I_UsedNonSys = heap + image_bytes()` (id 7 keeps the old approximation), which makes the request
fit exactly; the 8 MB allocator then builds normally.

### The misread caller

`DM_QMLOG` used to print `svcQueryMemory`'s caller as `sdk+0xdff9e8`, which sent earlier rounds
after the host stack. That address is the **return point of a rewritten-SVC gate block**: the
original instruction is `svc #6` at `sdk+0x158318` (the `nn::svc::QueryMemory` wrapper), the gate
block in the patch area `blr x30`s from `sdk+0xdff9c8` and its return address is 0xdff9e8
(`sdk.gates.tsv`: `0xdff9e8  svc 6  0x158318`). The log now prints the original site, and
`gate_register` stores the site as an absolute address (it also fixes the `ThreadContext` PC that
`thr_svc_enter` gives the guest).

## SDK services that had to be taken over

| Entry point | Why |
|---|---|
| `nn::oe::IsCpuOverclockEnabled` | `oe::Initialize` is a host no-op, so `nn::apm`'s client is null; the SDK path dereferences it at `sdk+0x1c2bcc`. Answer false. |
| `nn::oe::GetNotificationMessageEvent` | Returns the SDK global that `oe::Initialize` would have created; `InitializeMultiWaitHolder` aborts on its type byte (+0x28) being 0. Build a real `SystemEventType` with `nn::os::CreateSystemEvent`. |
| `nn::oe::GetDefaultDisplayResolution[ChangeEvent]` | Reaches `am::GetCommonStateGetter`, which is null without am (crash at `sdk+0x39b370`). Report 1920x1080 and create a never-signalled event. |
| `nn::album::Initialize` (and Finalize / Save* / ShowAlbumFiles) | Its tail call `LibraryState::EnsureAlbumAvailable` aborts when `capsrv::InitializeForApplication` fails (`nn::sm::Initialize` at `sdk+0x1404bc`). No album service: no-op / failure. |

## Shaders: the NVN programs are in the pak's `.sdf` files

`re_chunk_000.pak` is a KPKA 4.0 archive (16-byte header, 48-byte entries `{hashLo, hashHi, offset,
packed, size, attrs, checksum}`, zstd or raw deflate by `attrs & 0xF`). The compiled programs live
in the `.sdf` shader definitions: `SDF\0` header, then per program a 0x880-byte control section
(magic 0x98761234, code length at +0x704) immediately followed by its code area. Graphics stage =
SPH word 0 bits 10..13 (1 vertex, 2 hull, 3 domain, 4 geometry, 5 pixel); compute code uses magic
0x12345679 with instructions at +0x100. `dm/tools/repak.py sdf <pak> <dir>` extracts them without a
name list, `shader_split.py` reads them (`sdf_shaders`) and the profile sets `shader_separable on`
for the vertex / pixel cover pairs.

105 files → 5,093 programs (316 vertex, 2,653 pixel, 2,097 compute, 11 geometry, 7 hull, 9 domain);
`dmsh_build.py` translated 5,012 of them. The 81 failures are 38 compute shaders using subgroup
builtins, 25 spirv-cross failures and 18 Metal compile failures (geometry shaders, which Metal has
no stage for). On this machine run `dmsh_xlate` with
`DOTNET_ROOT=/opt/homebrew/Cellar/dotnet/10.0.401_1/libexec` (the Homebrew wrapper sets it).

## What is left

1. **Sparse virtual memory is the main suspect for the black scene background.** RE Engine's 64 GB
   virtual pool maps non-contiguous physical ranges; `nvnMemoryPoolMapVirtual` rejects ten of them
   ("true sparse mapping not implemented"), and the host points the virtual pool at a single
   physical base. A proper fix reserves real address space for the virtual pool, aliases each mapped
   physical range into it with Mach `vm_remap` (16 KB host pages), and resolves virtual-pool
   addresses through the mapping table in `mtl_gpu_addr` (a 64 GB range cannot be one MTLBuffer).
   Pass dumps of the dialogue scene show all textures "real" and ~84 draws per frame into
   1920x1080 targets, so the pipeline runs and only the content is black.
2. **AVC movies**: the logo / opening movies use decoder type 1 (AVC/VP8); the host only decodes VP9
   and returns black frames. VideoToolbox could add H.264 on Mac and iOS.
3. Save timestamps show 01/01/1970 (the guest clock source is 0; cosmetic, same family as the AA123
   DateTime issue).
4. Frame rate is about 29 fps (34 ms/frame, 125 draws/frame, ~3.9 ms shader translation on the CPU).
5. `pso_manifest.tsv` does not exist yet; `nvn_tex_base` (0x168, 128 slots), `nvn_samp_base` (0x568)
   and `nvn_img_base` (0x120) are now measured, and the host also decodes the legacy 0x20 texture
   table (see 04-graphics.md).
