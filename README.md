# EDF6 Plugins

Source for native [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader/releases)
plugins for **EARTH DEFENSE FORCE 6** (Steam).

| Plugin | What it does |
| --- | --- |
| [`EDF6SwarmPerf`](EDF6SwarmPerf/README.md) | Large FPS gain in big insect swarms (ants, spiders, pillbugs, bees, small dragons). No gameplay change. |
| [`EDF6CorpseCleanup`](EDF6CorpseCleanup/README.md) | Configurable corpse lifetime, ragdoll budget, generator-corpse tracking, optional corpse hitbox removal. |

Each plugin folder holds its source, its default `.ini`, its build script and a
README covering what that plugin does, how the patch works and what every
setting means. This file covers what they have in common.

`EDF6SwarmPerf` and `EDF6CorpseCleanup` are released together on Nexus Mods as
*EDF6 Swarm Performance Pack*.

> **AI disclaimer:** these plugins were made with AI. The game analysis, the plugin
> code and the release descriptions were created with an AI coding assistant (Claude).
> Each plugin is tested in-game before release.

## Game version and safety

These plugins target the Steam build with `EDF.dll` **buildid 17055427**
(SHA-256 `0d509239…733d0916`).

Every patch site is byte-verified at load. On any mismatch — a game update, a
different build — the plugin patches nothing, writes the reason to its log and
the game runs as vanilla. A game update therefore means the offsets have to be
re-found, not that the game breaks.

## Configuration

Each plugin reads an `.ini` next to its DLL in `Mods\Plugins\`; the defaults
shipped here are the files in each plugin's folder, and each plugin's README
documents its own settings. Changes apply on the next game launch. `Log=1` or
higher writes a `.log` next to the plugin.

## Building

Needs Visual Studio 2019 Build Tools (`cl`) with the Windows 10 SDK; no other
dependencies, and the DLLs link the CRT statically so players don't need a VC++
redistributable.

Every plugin folder has a `build.bat`:

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

Put the plugin `.dll` files and their `.ini` files in `<game>\Mods\Plugins\` with
EDFModLoader installed. Delete those files to uninstall.

## Credits

[EDFModLoader](https://github.com/BlueAmulet/EDFModLoader/releases) by
BlueAmulet, which makes native plugins possible. Per-plugin credits are in each
plugin's README.

## License

MIT — see [LICENSE](LICENSE).
