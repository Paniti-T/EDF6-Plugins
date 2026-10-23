# EDF6SwarmPerf

Large FPS gain in big insect swarms — ants, spiders, bees, pillbugs and small
dragons. No gameplay change.

Part of [EDF6 Plugins](../README.md); see the root README for install, build and
game-version notes that apply to every plugin here.

## What it does

Every insect enemy (`InsectBase`) runs a body-contact query each frame: a Havok
broadphase sphere query, then a closest-point / penetration test, a line-of-sight
check and a de-duplicating insert for *every* body it found. The caller then
discards every hit whose team is not hostile to the insect.

In a dense swarm nearly every body found is another insect or an ally corpse, so
the cost grows with the square of the swarm size and most of it is thrown away.
Profiling a heavy modded mission put about **80% of the main thread** in these
tests while the GPU was close to idle.

SwarmPerf applies the game's own hostile-team check *before* the narrow phase
instead of after it, for this one query only. The hit list the insect acts on is
identical to vanilla. Measured in-game: ~99% of candidates skipped, GPU
utilisation up from ~5% to ~32% in a heavy fight.

## How the patch works

The team relation is read from the game's own table — `[[EDF+20B2978]+0x38] +
selfTeam*0x38 + 0x18` → `int[otherTeam] == 2` means hostile; the team id lives at
`obj+0x314`, with `-1` meaning none.

The shared narrow-phase loop has five callers and some of them pass a null self,
so the hook stub only applies the early test when the body doing the query is the
insect currently inside the wrapped contact query. Every other caller is left
alone — an early build that skipped this guard crashed on the null self once a
mission loaded.

## `EDF6SwarmPerf.ini`

- `SkipAllyContactTests` — `1` (default) enables the early team test. `0`
  disables the patch, which is the first thing to try if you suspect a crash.
- `Log` — `0` off, `1` startup info, `2` stats every 10 s.

## Source

`SwarmPerf.cpp` is the whole plugin; `build.bat` compiles it. The shipped
`EDF6SwarmPerf.ini` in this folder is the default config.
