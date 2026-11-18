# Port overrides

Generated functions a port build replaces with a hand-written definition, without
editing `Source/`. Mechanism (`cmake/port.cmake`, `HS_PORT_OVERRIDES`): after the
module's library is built, `objcopy --weaken-symbol` weakens the generated definition
in the archive, so the strong one in `Redivivus/port/overrides/` wins at link
(same-file callers too: GCC calls go through symbol relocations). A file-static
the override needs is renamed and globalized the same way.

An override is a last resort and carries its retire condition. Every one here is
also something to report upstream.

| Function | Module | Why | Retire when |
|-|-|-|-|
| `D2CMP::Pallete::PALETTE_LoadFromFile` | D2CMP | Generator **frame artefact**: the end of the path buffer is computed as `&szInlineNameBuf + 3`, which lands on `nStrEndOffset1 - 1` only with VC8's frame layout. GCC puts the scalar 0x104 bytes away, `.dat` goes astray, and entering a game halts (Fog halt line 57). | Upstream generator fix (issue to file: generator bug, function address from the Ghidra DB). |
| `D2CMP::SRC::LRUCache::LRUCACHE_Unlink` | D2CMP | Blizzard's null write unlinking a list's only node. Upstream fixes it at runtime by rewriting x86 `CALL rel32`s (`Redivivus/d2_patch.cpp`), which can't work on MIPS. | Upstream moves the fix to a link-level or generator-level form. |

## Wraps

Interposed rather than replaced (`HS_PORT_WRAPS`): before the module's archive is built, the
definition in one object file is renamed to `hs_orig_<symbol>`; every caller outside that object
reaches the hand-written wrapper, which calls the original.

| Function | Module | Why | Retire when |
|-|-|-|-|
| `Storm::Source::SMem::SMemCalculateAllocationLayout` (static) | Storm | **Generator defect**: emits `(n ? 7 : 0) + n` for the original `(n + 7) & ~7`, so every Storm block is 15 bytes over its request and none is 8-aligned. Reached via its section, so on MIPS its entry is patched at first `SMemAlloc` to jump to a corrected copy (`overrides/storm_align.cpp`); the static and Storm's two memory flags are globalized for it. | Upstream generator fix (issue to draft). |
| `Storm::Source::SMem::SMemAlloc` | Storm | Doesn't round request sizes, so later blocks start at odd addresses (Storm's archive records at 0x…2F3); on MIPS their 32-bit fields are unaligned loads and lookups in every archive but the first failed. The wrapper rounds every request up to 8 bytes. | Never on MIPS (it's Blizzard's allocator behaviour), unless upstream emits an aligned SMem. |
| `D2CMP::SRC::SpriteCache::GFX_InitCelDataCache` | D2CMP | **PS2 memory budget**: the game sizes the sprite cache from RAM (24-64 MB); on a target reporting 32 MB or less the wrapper caps it at 2 MB (it held 5.2 MB in town). Not a defect: a port policy. | Never on a 32 MB target, unless the cache moves to a budget set elsewhere. |
| `D2CMP::SRC::SpriteCache::GFX_InitLRUCache` | D2CMP | **PS2 memory budget**: the game gives the tile LRU a flat 6 MB; it grows as the player explores. On a target reporting 32 MB or less the wrapper (in `sprite_cache_cap.cpp`) caps it at 2 MB. Costs speed in busy areas (15 FPS in Harrogath's lower town). Not a defect: a port policy. | Never on a 32 MB target. |
| `Fog::Memory::Alloc`, `AllocClientMemory`, `AllocServerMemory`, `ReAllocServerMemory`, `FreeClientMemory`, `FreeServerMemory`, `FreeMemoryPool` | Fog | **PS2 memory budget**: on a target reporting 32 MB or less, requests over 256 bytes skip Fog's power-of-two pools and get their exact size from malloc, tracked per pool manager so `FreeMemoryPool` still frees them (`overrides/fog_exact_alloc.cpp`). Town peak 36.8 -> 27.5 MB. Not a defect: a port policy. | Never on a 32 MB target. |
| `Storm::Source::SFile::SFILE_SearchHashTable` (static) | Storm | **PS2 memory budget**: MPQ hash tables are mostly empty slots (d2data.mpq: 10,815 of 65,536). On MIPS its entry is patched to a search over a compacted table built at an archive's first lookup (`overrides/mpq_compact_index.cpp`); the static and `SFILE_Hash` are globalized for it. 2.5 MB freed. Not a defect: a port policy. | When the port reads its own disc archive format. |
| `Storm::Source::SMem::SMemFree`, `SMemReAlloc` (and `SMemAlloc`, through `storm_align.cpp`) | Storm | **PS2 memory budget**: Storm keeps a 64 KiB heap window per allocating source line (2.2 MB reserved for 0.3 MB used in town). On a target reporting 32 MB or less, requests below 0xfe88 bytes get their exact size from malloc behind an 8-byte header (`overrides/storm_exact_alloc.cpp`), honouring flags 8 (zero) and 0x10 (realloc in place only). Not a defect: a port policy. | Never on a 32 MB target. |
| `D2Win::Src::D2WinArchive::IMAGE_LoadDC6`, `D2Win::D2WinAnimImage` (constructor) | D2Win | **PS2 memory budget**: the class-select screen loads all seven classes' animations (12.3 MB). On a target reporting 32 MB or less, the walk and selected rows are skipped at load and loaded per class when its control leaves idle, freed when it returns (`overrides/charcreate_lazy.cpp`). Not a defect: a port policy. | Never on a 32 MB target. |
