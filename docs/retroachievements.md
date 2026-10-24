# RetroAchievements

GB300 and SF2000 have no Wi-Fi hardware, so achievements cannot be fetched
from the console while a game is running. UniFrog uses an offline-first design
instead:

1. The firmware computes the RetroAchievements game hash, reads cached
   achievement definitions from the SD card, and evaluates them every frame
   with [rcheevos](https://github.com/RetroAchievements/rcheevos).
2. Unlocks are appended to a durable queue on the card, and only then is the
   id recorded in `achievements/earned/<hash>.txt` so re-entering a game never
   queues the same award twice.
3. `tools/ra_sync.mjs`, run on a PC or phone with the card mounted, downloads
   definitions and uploads the queued unlocks to retroachievements.org.

The emulator never needs a network stack, and a crash or a flat battery costs
at most the progress written in the last five seconds.

## Player setup

1. Copy your **Web API Key** from retroachievements.org
   (Profile -> Settings -> Keys) into
   `unifrog_data/achievements/ra_sync.ini`:

   ```ini
   [retroachievements]
   username=YourName
   apikey=0123456789abcdef0123456789abcdef
   ```

   This file is only read by the companion tool. The device itself only needs
   the username, which it stores in `achievements/ra.ini` when achievements are
   enabled from the pause menu.

2. Start any game once. The device writes
   `achievements/queue/<hash>.req` when it has no definitions for the game.

3. With the SD card mounted on a computer:

   ```sh
   node tools/ra_sync.mjs --root /media/mmcblk0/unifrog_data
   ```

   The tool downloads one JSON file per game, uploads queued unlocks, and
   removes successful queue entries. Use `--list` to inspect the card without
   touching the network and `--dry-run` to see what would be sent.

4. Eject the card safely and put it back in the handheld. Unlocks now appear as
   banners in game and in the pause menu.

Node 18 or newer is the only requirement; the script has no npm dependencies.

If you would rather not use a terminal, `tools/ra-sync-gui` builds the same
sync as a GUI: `UniFrog-RA-Sync.exe` for Windows (32- and 64-bit) and
`UniFrog-RA-Sync.apk` for Android 5.0+, which picks `unifrog_data` through the
system folder picker and stores the username and key itself, so `ra_sync.ini`
is not needed there. See
[tools/ra-sync-gui/README.md](../tools/ra-sync-gui/README.md).

## In-game UI

The pause menu (SELECT + START) has an **Ach** tab listing the current game:
points, measured progress, and whether each achievement is earned. The
**RetroAchievements** row toggles evaluation on and off, and the header shows
how many achievements of the current game are unlocked.

When an achievement triggers, a banner appears at the top of the screen for
`toast_ms` milliseconds. Progress is saved every five seconds and on exit.

## Configuration

`unifrog_data/achievements/ra.ini` is written by the firmware:

| Key | Default | Meaning |
| --- | --- | --- |
| `enabled` | `1` | Evaluate cached achievements while playing. |
| `hardcore` | `0` | Reserved. Only softcore is enforced today. |
| `toast_ms` | `4000` | Unlock banner duration, clamped to 500..15000. |
| `username` | empty | Cached username, informational. |

Definitions are not enabled per game: if a cache exists it is used. Delete a
cache file to stop tracking that game, or turn achievements off entirely.

## Supported systems

Hashing and memory mapping are console specific. The following cores are
mapped today:

| Core id | System |
| --- | --- |
| `fceumm`, `quicknes` | NES |
| `snes9x2002`, `snes9x2005` | SNES |
| `gambatte`, `gearboy` | Game Boy / Game Boy Color |
| `gpsp` | Game Boy Advance |
| `picodrive` | Mega Drive, Master System, Game Gear, 32X, Sega CD (by extension) |
| `pce-fast` | PC Engine / TurboGrafx-16 |
| `prosystem`, `stella2014`, `a5200`, `handy`, `beetle-cygne`, `race`, `gearcoleco`, `bluemsx`, `frodo`, `qpsx` | Other known cores, when built |

An unknown core disables tracking instead of guessing a hash: a hash computed
for the wrong console is valid MD5 but never matches the server.

## Troubleshooting

- **"No achievement data - run ra_sync"**: the game was identified but no
  `cache/<hash>.json` exists. Run the tool with the card mounted.
- **"Achievement data rejected"**: the cache file is not the raw `patch`
  response. Do not edit it by hand; delete it and run `--refresh <hash>`.
- **Achievements never trigger**: check the pause menu list. If it is empty,
  the game may have no achievements, or the core may be unmapped.
- **Achievements unlock but `queue/unlocks.jsonl` is missing**: check
  `earned/<hash>.txt`. Older firmware recorded the earned id before appending
  the queue line, so one rejected SD write lost the award for good. The log
  line `event=unlock_queue_failed` confirms it; current firmware keeps the
  line in RAM and retries instead.
- **Unlocks stay in the queue**: run `--push-only` and read the error. Expired
  sessions, a wrong API key, or Cloudflare blocks are reported and the entry is
  kept for the next run.

## Developer notes

- Engine: `components/libretro/src/achievements/unifrog_achievements.c`,
  public API in `include/unifrog/achievements.h`.
- Host hooks: `components/libretro/src/unifrog_libretro_host.c` registers core
  memory accessors, calls begin after `retro_load_game`, evaluates once per
  frame, and draws the toast after presentation.
- UI: `libretro_frontend_quick_menu.c` adds the **Ach** page.
- rcheevos is pinned to `v12.5.0` in `config/options.mk` and built as
  `output/librcheevos.a`. It is never committed; `make deps-rcheevos` fetches
  the tag. The build disables Lua and pthreads because the engine is
  single-threaded and only needs the runtime, API parsers, and hash helpers.
- The companion protocol is the documented emulator API: `r=patch&m=<hash>`
  for definitions and `r=awardachievement` with the md5 signature
  `id + user + hardcore [+ id + seconds]`.
- rcheevos only serializes *unfinished* triggers, so earned ids are stored
  separately in `earned/<hash>.txt` (one id per line). The engine imports ids
  from `queue/unlocks.jsonl` when that file predates the earned list, so
  upgrading firmware does not re-queue old awards.
- The queue line is written *before* the id reaches the earned list. SD writes
  are the least reliable operation while a core runs, so a failed append keeps
  the line in RAM; it is retried every two seconds and on exit. Writing the
  earned id first would turn one card fault into a permanently lost award.

NocturnalRTOS ships a port of this engine inside Phobos
(`components/applications/apps-phobos/source/phobos/achievements/`). It uses
the same cache/progress/earned/queue layout and the same `ra_sync.mjs`, so one
SD card can move between both firmwares. See that repository's
`documentation/retroachievements.md` for its hooks and hotkeys.
