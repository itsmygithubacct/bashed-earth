# Bashed Earth

Turn-based artillery combat in your terminal — a C port of
[Tank Wars](https://tankwars.domainreviews.org) that renders real pixels
via the **kitty graphics protocol**. No SDL, no X11, no ncurses: the whole
game is an RGBA framebuffer zlib-compressed and streamed to the terminal
as base64 APC escape sequences at 30 fps.

Built for [kilix](https://github.com/itsmygithubacct/kilix) (and any
kitty-protocol terminal: kitty, ghostty, wezterm...).

## Features

Full feature parity with the web original:

- **Falling-sand destructible terrain** — 1px cellular automaton with
  grass/sand/ice worlds, lakes, flowing water, snow, and ice that shatters
  into avalanches when explosions cut it loose
- **15 weapons** — from the Baby Missile to the Nuke: Triple, Bouncy,
  Roller (rolls downhill into dug-in tanks), Drill, Digger, Napalm
  (spreads burning fire that creeps downhill and is doused by water),
  Dirt, MIRV, plus Raft / Parachute / Shield utility items
- **Weapon store & economy** — $10k per match, leftover cash and unused
  ammo carry across matches, winner bonus, multi-match scoreboard
- **5 AI personalities** — Aggressive, Defensive, Tactical, Balanced,
  Trickster; each with its own shopping list, weapon priorities, aim
  accuracy, taunts, and name pool
- **Weather** — wind that pushes shells, rain that fills craters, snow
  that buries ice worlds
- **Hazards** — fall damage (parachutes save you), buried-alive damage,
  self-hits, and a stalemate rule so dug-in wars end

## Build

```sh
make            # needs gcc/clang, zlib, libm (Linux)
./bashed-earth
```

## Controls

| Key | Action |
|-----|--------|
| Left / Right | aim barrel |
| Up / Down | power |
| Space / Enter | fire |
| 1-0 | weapon hotkeys |
| D / R | Drill / Roller |
| Tab | cycle weapons |
| Q | quit |

Menus: arrows to navigate, Left/Right to change values, Enter to confirm.
Options persist to `~/.config/bashed-earth.conf`.

## Development

```sh
make test                          # headless AI-vs-AI selftest matches
./bashed-earth --selftest 42 3     # specific seed, 3 matches
./bashed-earth --render-test 7     # dump render_*.ppm screenshots
BE_DEBUG=1 ./bashed-earth --selftest 1 1   # tick-by-tick state trace
```

The selftest plays full 4-AI matches headlessly (store, combat, economy,
carry-over) and checks invariants — no terminal needed, so it runs in CI.

## Architecture

| File | Role |
|------|------|
| `src/term.c` | raw mode, key decoding, kitty graphics frames (zlib + base64) |
| `src/terrain.c` | falling-sand automaton with per-row active-span tracking |
| `src/game.c` | tanks, projectiles, flames, explosions, AI, store, turn flow |
| `src/render.c` | software rasteriser: scene, HUD, menus (embedded PSF font) |
| `src/config.c` | weapon/AI/color data tables |
| `src/main.c` | 30 fps loop (60 Hz logic), selftest, render-test |
