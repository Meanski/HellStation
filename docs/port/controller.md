# Controller support

Legacy D2 (1.14d) never had a controller. D2R does, and that's the UX bar. We build it as hand-written
code in `Redivivus/port/input/`, platform-neutral, fed by thin adapters: SDL2 `SDL_GameController`
(Phase 1, so it can be designed and tested on Linux) and ps2sdk `padman` for the DualShock 2 (Phase 2).
USB mouse and keyboard stay fully supported alongside it.

## Prior art (research 2026-11-17)

| Project | What it does | Licence | Use |
|-|-|-|-|
| D2R (Blizzard) | Native controller UI: direct stick movement, skill slots on face buttons with a modifier set, radial menus, panel navigation | proprietary | UX reference only; no assets |
| [d2decomped](https://github.com/psi29a/d2decomped) | 1.14d reimplementation with full SDL3 gamepad support: stick walking by direction, soft targeting, d-pad panel hopping, store trading, skill picker, weapon swap, rebindable actions, 7+7 skill slots with an LT layer and a radial assign wheel (PRs #50, #53, #63, #229, #347; `docs/design/controller-plan.md`) | GPL-3.0 | Design reference: read docs and behaviour, **never copy code** |
| [D2Vita](https://github.com/Franckrst/D2Vita) | Original 1.14d via box86 on Vita. Left stick walks, right stick is a free cursor, L is aim-assist (walk-to and attack the best target), R right-click, d-pad belt 1–4, Select radial menu, remappable `controls.txt` | GPL-3.0 | Scheme reference only |
| [DevilutionX](https://github.com/diasurgical/devilutionX) | Diablo 1 port with a mature controller scheme and dead-zone handling | Sustainable Use License | Scheme reference only |
| Xpadder / AutoHotkey / Steam Input layouts | Map the pad onto mouse and keyboard from outside the game | n/a | Proves layer 0 is playable; nothing more |

No standalone controller mod for legacy 1.14d turned up. The working implementations are all inside
ports or reimplementations, which matches our approach: we own the message pump, so the pad lives in
the engine rather than outside it.

## Layers

Each layer is shippable by itself and falls back to the one below.

**Layer 0: virtual mouse.** Pad → synthesized `WM_MOUSEMOVE`/`WM_[LR]BUTTON*`/`WM_KEY*` through the
shim's message queue. Right stick drives the cursor with acceleration; triggers are left and right
click; buttons map to D2's own hotkeys. It needs no knowledge of game state and works on every screen,
menus included. This is the first milestone on both backends.

**Layer 1: in-world play.** It reads game state from hand-written code (calls into generated functions,
reads of generated globals, resolved by **address** through the Ghidra DB, never by guessing):
- Left stick walks by direction: a move target at a fixed radius from the player in screen space, held
  while deflected. Run follows the game's run toggle.
- Soft targeting: the nearest hostile, object or item inside a cone around the stick or facing
  direction, highlighted through the game's own hover unit. Primary attack and interact go to it.
- Skill slots: face buttons and the right trigger hold skills; a held shoulder gives a second set.
  Assignments are stored per character in a sidecar file next to the save. **`.d2s` is never touched.**
- The d-pad drives belt slots 1–4; a held modifier gives belt 5–8 in LoD when a belt is worn.

**Layer 2: UI.** Inventory, stash, cube, vendor and skill tree hop the cursor cell by cell or button by
button from a hit-test table per panel. A radial wheel handles skill assignment and quick panels. The
overlay is drawn into the 8-bit frame by the backend before present, with the game's palette and
font, so it's identical on SDL and GS.

## Action model

```
adapter (SDL / padman)  ->  PadState {buttons, sticks, triggers, pressure}
                         ->  Bindings (rebindable, ini)   ->  Action stream
Action stream           ->  Layer 2 (if a panel is open) / Layer 1 (in world) / Layer 0 (fallback)
                         ->  WM_* messages or game calls on the game thread
```

- Adapters hold no game logic and game code never sees SDL or ps2sdk types.
- Everything is delivered on the game thread at message-pump time, so it's deterministic and
  replayable: the smoke test's input file can carry pad events.
- Dead zones are radial, with separate inner and outer values per stick. DualShock 2 analogue mode is
  forced on through padman; pressure-sensitive face buttons are unused at first.

## Default layout (DualShock 2 labels)

| Input | In world | In panels |
|-|-|-|
| Left stick | Walk | Move cursor (coarse) |
| Right stick | Free cursor (layer 0) / aim override | Move cursor (fine) |
| D-pad | Belt 1–4 (hold L1: 5–8) | Hop cell or button |
| ✕ | Interact / pick up / primary on target | Click |
| ○ | Skill slot | Back / close |
| □ | Skill slot | Move item to stash/inventory |
| △ | Skill slot | Item details |
| R1 | Skill slot | Next tab |
| L1 (hold) | Second skill set | Previous tab |
| R2 / L2 | Right / left click (layer 0) | — |
| L3 | Toggle run | — |
| R3 | Show items (hold) | — |
| Start | Game menu (Esc) | — |
| Select | Radial: inventory, character, skills, quests, map, merc | — |

These are starting values to be tuned on hardware. Every entry is rebindable.

## Open items

- Find by address the stable entry points for: the player unit, screen↔world projection, the hover
  unit, move/attack/skill commands, belt use and the panel-open state. Mac pairs first.
- Check whether the D2Gdi path exposes the font and palette we need for the overlay without a second
  renderer.
- Decide whether PS2 USB pads (via SDL2-PS2 or a USB HID IRX) are worth supporting beyond DualShock 2.
