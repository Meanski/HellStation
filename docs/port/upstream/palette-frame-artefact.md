# Draft upstream issue: generator frame artefact in PALETTE_LoadFromFile

*For the owner to file at https://github.com/jaenster/redivivus/issues (template: generator bug).
Add the function's address from the Ghidra DB before filing.*

**Function:** `D2CMP::Pallete::PALETTE_LoadFromFile` (`Source/D2CMP/Pallete.cpp`), as generated at
`dd289669` (Ghidra v2109), unchanged on current `main`.

**Emitted code:**

```cpp
char* szInlineNameBuf;
char nStrEndOffset1[260];
...
strcat(nStrEndOffset1, szPaletteName);
pSrcWord = (char*)((intptr_t)&szInlineNameBuf + 3);   // meant: nStrEndOffset1 - 1
do { pFullPathEnd = pSrcWord; pSrcWord = pFullPathEnd + 1; } while (pFullPathEnd[1] != '\0');
memcpy(pFullPathEnd + 1, ".dat", 5);
```

The inlined `strlen` scan starts at the byte before the path buffer, but it's expressed as an offset
from a different local. That only holds under VC8's frame, where `szInlineNameBuf` sits directly below
`nStrEndOffset1`.

**Effect with GCC:** GCC places the scalar 0x104 bytes from the array (`-0x20(%ebp)` against
`-0x124(%ebp)` at -O0), so the scan runs over unrelated stack, `.dat` is written somewhere else, and
`CheckIfFileExists("Data\Global\Items\Palette\grey")` fails. Entering any game halts:
`ERROR_UnrecoverableInternalError_Halt(…, 0x39)` from `PALETTE_InitItemPalettes` ←
`GAME_InitClientSide`. The mingw `d2.exe` should hit the same halt on entering a game. We saw it on an
i686 Linux build of the same tree.

**Expected:** the scan starts at `nStrEndOffset1 - 1` (or the generator emits `strcat(…, ".dat")`).

**Workaround in HellStation:** a link-time override (`docs/port/overrides.md`), to be retired when this
is fixed in the generator.
