# lr_weapons (Plugify)

[LR] ExStats Weapons: per-weapon kill statistics of the Levels Ranks core (the `<lr_table>_weapons` table: steam, classname, kills), the per-weapon experience coefficient of exstats_weapons.ini and the per-kill / on-disconnect saves. Ported from the Pisex Levels Ranks module LR_ExStatsWeapons (production plugin lr_weapons). Owns its plgdb connection with the levels_ranks database settings. Requires the levels_ranks core; no exported methods.

Plugify port (fork) of **LR_ExStatsWeapons ([LR] ExStats Weapons)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-lvl_ranks_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, levels_ranks, hudmenu (optional)

## Installation
Download `lr_weapons-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/lr_weapons`.

`plugins/levels_ranks` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

