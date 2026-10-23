# EDF6CorpseCleanup

Configurable corpse lifetime, ragdoll budget, generator-corpse tracking and
optional corpse hitbox removal.

Part of [EDF6 Plugins](../README.md); see the root README for install, build and
game-version notes that apply to every plugin here.

## What it does

Enemy corpses are tracked by the game's `RagdollManager` singleton. Each corpse
normally lives 15–30 s (hardcoded per enemy family) or until the manager's
ragdoll budget (256/512) evicts the oldest.

The plugin redirects the game loop's single call to `RagdollManager::Purge` so
that, once per frame, any tracked corpse older than `CorpseSeconds` is removed
with the same `SceneObject` remove function the game uses for its own timeout —
so nothing pops or breaks.

It can also:

- override the budget constants,
- let generator (teleport anchor) corpses into the manager,
- remove corpse hitboxes.

For the hitboxes, the Havok bodies of tracked corpse ragdolls are tagged each
frame and the world collision filter's query-vs-body check rejects tagged bodies.
Raycasts and shape casts — bullets, explosions, line of sight — skip corpses,
while body-vs-body physics is untouched, so corpses still land on the ground.

## `EDF6CorpseCleanup.ini`

- `CorpseSeconds` — seconds a corpse may stay. `0` keeps the game's own timing.
- `RagdollBudget` — corpse budget; the oldest corpse is removed when it fills.
  `0` = game default (512).
- `AcceptGeneratorCorpses` — `1` also tracks corpses from teleport anchors and
  generators. Compatible with EDF6GeneratorBodyBudget.
- `DisableCorpseHitbox` — `1` makes corpses ignore bullets, explosions and line
  of sight.
- `Log` — `0` off, `1` startup info, `2` stats every 10 s, `3` every removal.

## Source

`CorpseCleanup.cpp` is the whole plugin; `build.bat` compiles it. The shipped
`EDF6CorpseCleanup.ini` in this folder is the default config.

## Credits

[EDF6 Corpse Optimization](https://www.nexusmods.com/earthdefenseforce6/mods/384)
set the direction for this plugin.
