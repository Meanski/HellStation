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
