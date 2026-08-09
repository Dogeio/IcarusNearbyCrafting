# NearbyCrafting

NearbyCrafting lets you craft and repair inventory items with resources stored in nearby
containers and crafting stations. It also adds a configurable shortcut that deposits
matching backpack items into nearby inventories.

## Features

- `Nearby crafting` uses ingredients from nearby storage containers and,
  optionally, nearby crafting stations (enabled by default).
- `Nearby repairs` repair inventory items with resources stored nearby.
- `Storage-first sourcing` utilizes storage containers before crafting-station inventories.
- `Quick Deposit` uses a configurable shortcut to move backpack items into nearby
  inventories that already contain that item type (enabled by default, behind `Shift+E`).
  Also allows for configuration to skip certain items (e.g. `Composite Arrow`).
- `Configurable range and sources` let you control the scan radius, inventory limit,
  crafting-station inclusion, repairs, and deposit behavior.

## Full Installation

Use these steps if you downloaded the full package, including the experimental version
of UE4SS. If you already have **a recent UE4SS version** and other mods installed,
use the mod-only steps below instead.
**If you're unsure of the version, please install the UE4SS version included in this mod** using
the instructions below, and then look at the [UE4SS Configuration](#ue4ss-configuration) section for adding
support for the rest of your installed mods using a simple configuration change.

1. Close Icarus.
2. In Steam, right-click `Icarus`, choose `Manage`, then `Browse local files`.
3. Open `Icarus\Binaries\Win64` inside the game folder.
4. Copy both `dwmapi.dll` and the `ue4ss` folder from the downloaded archive into that `Win64` folder.
5. Allow Windows to merge folders if prompted, then start the game.

The finished layout should include:
```text
Icarus\Binaries\Win64\dwmapi.dll
Icarus\Binaries\Win64\ue4ss\UE4SS.dll
Icarus\Binaries\Win64\Icarus-Win64-Shipping.exe
```

*The first launch after installing UE4SS may take a little longer than usual.*

## Installing only NearbyCrafting

Use these steps if experimental UE4SS is already installed, or if another mod manager
provides it. Download the standalone archive (`NearbyCrafting-v<version>-Standalone.zip`):

1. Close Icarus.
2. Open the downloaded archive and copy the `NearbyCrafting` folder.
3. Paste it into the game's `Icarus\Binaries\Win64\ue4ss\Mods` folder.

Note: If you DON'T have a `ue4ss` folder, then the installed UE4SS version is likely
older than this mod supports, please use the instructions above to install the full mod
and then follow the instructions in the next section to configure UE4SS to load both mod folders.
That being said, having the `ue4ss` folder does NOT 100% mean the installed version is recent enough.

This method does not replace your existing `dwmapi.dll`, `UE4SS.dll`, or UE4SS config.
Back up your current `NearbyCrafting.ini` before replacing an older version of NearbyCrafting.

## UE4SS Configuration

The UE4SS configuration file is stored at `Win64/ue4ss/UE4SS-settings.ini`.
Some common options you might want to change are:

- Adding support for loading the old-UE4SS style of Mods folder under `Win64/Mods`:
  Add the following `+ModsFolderPaths` line under the Overrides section like so:
```ini
[Overrides]
; Path to the 'Mods' folder
; Default: <dll_directory>/Mods
ModsFolderPath =
+ModsFolderPaths = ../Mods
```
- Enabling the debug console:
  Replace `ConsoleEnabled = 0` With `ConsoleEnabled = 1` (`CTRL+F` is useful for finding this.)

## Configuration

The settings file is located at `Icarus\Binaries\Win64\ue4ss\Mods\NearbyCrafting\config\NearbyCrafting.ini`

Most settings can be edited while Icarus is running: save the file, then press the
configured reload key (`F5` by default). Settings tied to startup or key registration
still require a restart, as listed below.

- `Enabled` turns the mod on or off.
- `ReloadConfigKey` sets the key used to reload live-safe settings. The default is `F5`.
  Set it to `NONE` or leave it empty to disable live configuration reload.
- `ScanRadiusMeters` controls how far crafting, inventory repairs, and deposit can reach.
- `MaxNearbyInventories` limits how many nearby inventories can be checked. Storage
  containers fill this limit first; crafting-station inventories are added afterward.
- `IncludeBenchInventories` controls whether nearby crafting stations can supply items.
- `RepairsEnabled` enables nearby inventory repairs. Set it to `false` and restart Icarus
  to skip native repair-hook installation when troubleshooting compatibility problems.
- `DepositExcludedItems` is a comma-separated list of full in-game item names that
  must remain in the backpack, such as `Composite Arrow, 12-Gauge Slug`.
  Matching is case-insensitive but exact; partial names such as Slug and internal item identifiers do not match.Names use the language currently selected in Icarus.
- `DepositEnabled` enables or disables the nearby-deposit shortcut.
- `DepositKey` sets its key. Common examples are `O`, `F8`, `HOME`, `PAGE_UP`, and
  `NUM_ZERO`.
- `DepositModifier` accepts `NONE`, `SHIFT`, `CTRL`, `ALT`, or combinations such as
  `CTRL+SHIFT`. Modifiers are matched exactly.
- `DepositIncludeBenchInventories` independently controls whether the shortcut can
  deposit into crafting stations. Set it to `false` for chests and storage containers only.
- `BenchCacheRefreshMilliseconds` controls the backup refresh interval for stationary
  crafting stations. The default is recommended.
- `PlayerCacheRefreshMilliseconds` controls how quickly nearby items update while your
  character moves. The default is recommended.
- The two `Exclude...Inventories` settings protect special-purpose game inventories and
  should normally stay enabled.

The deposit shortcut passes only the player's `BackpackInventory`; it never passes the
hotbar inventory, and it does not introduce an item type into a destination that does
not already contain that type. It keeps using Icarus's normal server-authoritative
`TransferLike` action when no exclusions are configured, when the backpack contains no
excluded type, or when a particular destination does not already contain an excluded
type from the backpack. Only an affected destination uses server-authoritative
per-item-type transfers so the prohibited type can be skipped.

### Live configuration reload

When `ReloadConfigKey` is configured, save `NearbyCrafting.ini` and press it to reload
these settings. Set `ReloadConfigKey` to `NONE` or leave it empty to disable live
configuration reload:
- `ScanRadiusMeters`
- `MaxNearbyInventories`
- `IncludeBenchInventories`
- `BenchCacheRefreshMilliseconds`
- `PlayerCacheRefreshMilliseconds`
- `DepositIncludeBenchInventories`
- `DepositExcludedItems`
- `ExcludeClientOnlyInventories`
- `ExcludeRemoveOnlyInventories`

The proximity/source caches are rebuilt as part of the reload, so the new radius,
inventory filters, and bench choices take effect on the next operation. If the file is
missing or contains an invalid or unknown setting, the reload is rejected and the live
settings remain unchanged; details are written to `UE4SS.log`.

`Enabled`, `ReloadConfigKey`, `RepairsEnabled`, `DepositEnabled`, `DepositKey`, and
`DepositModifier` are not reloaded because they control startup behavior or registered
input callbacks. Restart Icarus after changing one of them. The currently registered
reload key remains usable until that restart.

## Game updates and fail-safe behavior

NearbyCrafting validates the Icarus function signatures, parameter types, object
references, and native call patterns it uses before enabling each feature. If a future
game update changes the required server queue contract, the mod stays inert and writes
the reason to `UE4SS.log`. Optional nearby repairs or Quick Deposit disable only their
own incompatible path, so the remaining verified safe features can continue.

This prevents the mod from guessing at changed native layouts. It cannot promise that
an unknown future game build will preserve every feature, but an unrecognized build is
handled by refusing the unsafe operation rather than using a stale address or ABI.

## Multiplayer

For multiplayer, use the same NearbyCrafting version on the host or dedicated server
and on every player who wants to utilize this feature.

**Client-only use on an unmodded dedicated server will cause the Nearby Crafting to not work.**
**Quick Deposit WILL work regardless of the server-side mod status.**

## Uninstalling

Close the game and remove the `Icarus\Binaries\Win64\ue4ss\Mods\NearbyCrafting` folder.
Only remove UE4SS itself if no other installed mods need it.

If the mod does not work, check `Icarus\Binaries\Win64\ue4ss\UE4SS.log`
for error/warnings containing `NearbyCrafting`.

## Included UE4SS version

The full package includes experimental UE4SS built from source at commit
[662df91503379fc383bc745f7ade795d7b2ca215](https://github.com/UE4SS-RE/RE-UE4SS/commit/662df91503379fc383bc745f7ade795d7b2ca215).

The absolute earliest RE-UE4SS source revision containing every lifecycle hook used by
NearbyCrafting is [953b6dd747a87bfba1a5c582002101741822b31d](https://github.com/UE4SS-RE/RE-UE4SS/commit/953b6dd747a87bfba1a5c582002101741822b31d).
Older builds do not provide the removable `BeginPlay` and `EndPlay` callbacks this mod
requires. Because native mods must match UE4SS closely, using the version included in
the full package is **strongly recommended**.

## Inspiration and thanks

Special thanks to the developer of [IcarusCraftNet](https://www.nexusmods.com/icarus/mods/277), the original mod that
inspired me to work on this one. NearbyCrafting was rewritten from scratch after
I encountered crashes and other issues with the earlier mod.
**`It does not contain code or files from IcarusCraftNet`**.

## Open source repository

NearbyCrafting is open source. Its source code, issue tracker, and development
history are available in the [IcarusNearbyCrafting GitHub repository](https://github.com/Dogeio/IcarusNearbyCrafting).
Bug reports and contributions are welcome.

## License

NearbyCrafting is licensed under the [MIT License](LICENSE).

UE4SS is a separate project distributed under its own MIT License. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for its copyright and license
notice.

Icarus and its associated assets are not included in this repository. This
project is not affiliated with or endorsed by the game's developers.
