# EDF6 Plugins

Source for native [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader/releases)
plugins for **EARTH DEFENSE FORCE 6** (Steam):

| Plugin | What it does |
| --- | --- |
| `EDF6SwarmPerf.dll` | Large FPS gain in big insect swarms (ants, spiders, pillbugs, bees, small dragons). No gameplay change. |
| `EDF6CorpseCleanup.dll` | Configurable corpse lifetime, ragdoll budget, generator-corpse tracking, optional corpse hitbox removal. |

`EDF6SwarmPerf` and `EDF6CorpseCleanup` are released together on Nexus Mods as
*EDF6 Swarm Performance Pack*.

> **AI disclaimer:** these plugins were made with AI. The game analysis, the plugin
> code and the release descriptions were created with an AI coding assistant (Claude).
> Each plugin is tested in-game before release.

## SwarmPerf

Every insect enemy (`InsectBase`: ants, spiders, bees, pillbugs, small dragons)
runs a body-contact query each frame: a Havok broadphase sphere query, then a
closest-point / penetration test, a line-of-sight check and a de-duplicating
insert for *every* body it found. The caller then discards every hit whose team
is not hostile to the insect.

In a dense swarm nearly every body found is another insect or an ally corpse, so
the cost grows with the square of the swarm size and most of it is thrown away.
In profiling of a heavy modded mission about **80% of the main thread** sat in
these tests while the GPU was close to idle.

SwarmPerf applies the game's own hostile-team check *before* the narrow phase
instead of after it, for this one query only. The hit list the insect acts on is
identical to vanilla. Measured in-game: ~99% of candidates skipped, GPU
utilisation up from ~5% to ~32% in a heavy fight.

The team relation is read from the game's own table
(`[[EDF+20B2978]+0x38] + selfTeam*0x38 + 0x18` → `int[otherTeam] == 2` means
hostile; team id lives at `obj+0x314`, `-1` = none). The hook stub only applies
the early test when the body doing the query is the insect currently inside the
wrapped contact query — other callers of the shared narrow-phase loop (some of
which pass a null self) are left alone.

## Corpse Cleanup

Enemy corpses are tracked by the game's `RagdollManager` singleton. Each corpse
normally lives 15–30 s (hardcoded per enemy family) or until the manager's
ragdoll budget (256/512) evicts the oldest.

The plugin redirects the game loop's single call to `RagdollManager::Purge` so
that, once per frame, any tracked corpse older than `CorpseSeconds` is removed
with the same `SceneObject` remove function the game uses for its own timeout —
so nothing pops or breaks. It can also override the budget constants, let
generator (teleport anchor) corpses into the manager, and remove corpse
hitboxes: the Havok bodies of tracked corpse ragdolls are tagged each frame and
the world collision filter's query-vs-body check rejects tagged bodies, so
raycasts and shape casts (bullets, explosions, line of sight) skip corpses while
body-vs-body physics is untouched and corpses still land on the ground.

## Game version and safety

Both plugins target the Steam build with `EDF.dll` **buildid 17055427**
(SHA-256 `0d509239…733d0916`).

Every patch site is byte-verified at load. On any mismatch — a game update, a
different build — the plugin patches nothing, writes the reason to its log and
the game runs as vanilla. A game update therefore means the offsets have to be
re-found, not that the game breaks.

## Configuration

Each plugin reads an `.ini` next to its DLL in `Mods\Plugins\`; the defaults
shipped here are the files in each plugin's folder. Changes apply on the next
game launch. `Log=1` or higher writes a `.log` next to the plugin.

### `EDF6SwarmPerf.ini`

- `SkipAllyContactTests` — `1` (default) enables the early team test. `0`
  disables the patch, which is the first thing to try if you suspect a crash.
- `Log` — `0` off, `1` startup info, `2` stats every 10 s.

### `EDF6CorpseCleanup.ini`

- `CorpseSeconds` — seconds a corpse may stay. `0` keeps the game's own timing.
- `RagdollBudget` — corpse budget; the oldest corpse is removed when it fills.
  `0` = game default (512).
- `AcceptGeneratorCorpses` — `1` also tracks corpses from teleport anchors and
  generators. Compatible with EDF6GeneratorBodyBudget.
- `DisableCorpseHitbox` — `1` makes corpses ignore bullets, explosions and line
  of sight.
- `Log` — `0` off, `1` startup info, `2` stats every 10 s, `3` every removal.

## Building

Needs Visual Studio 2019 Build Tools (`cl`) with the Windows 10 SDK; no other
dependencies, and the DLLs link the CRT statically so players don't need a VC++
redistributable.

```
_source\EDF6SwarmPerf\build.bat
_source\EDF6CorpseCleanup\build.bat
```

Each script calls `vcvars64.bat`, compiles the single `.cpp` and writes the DLL
two directories up — i.e. into `Mods\Plugins\` when this tree sits in
`Mods\Plugins\_source\`. Build output goes to `obj\` (ignored by git). A rebuild
fails while EDF6 is running, because the loaded DLL is locked.

If Build Tools are installed somewhere else, edit the `vcvars64.bat` path at the
top of each `build.bat`.

## Install (players)

Put `EDF6SwarmPerf.dll`, `EDF6CorpseCleanup.dll` and their `.ini` files in
`<game>\Mods\Plugins\` with EDFModLoader installed. Delete those files to
uninstall.

## Credits

- [EDF6 Corpse Optimization](https://www.nexusmods.com/earthdefenseforce6/mods/384)
  set the direction for this pack.
- [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader/releases) by
  BlueAmulet, which makes native plugins possible.

## License

MIT — see [LICENSE](LICENSE).
