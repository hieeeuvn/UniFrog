# App Paks

An app pak is a folder on the SD card that adds a tool to the **Apps** menu.
Adding a tool means copying one folder; it does not need a firmware rebuild, and
it survives firmware updates because it lives under `unifrog_data`.

App paks are UniFrog's own format. MustardOS `.muxapp` / Onion `.pak` bundles are
folders with a shell script entry point, and the SF2000 firmware has no shell, so
those cannot run here. A UniFrog pak runs a [JS2300](js2300-scripting.md) script
instead.

## Layout

```text
SD card root
└── unifrog_data
    └── apps
        └── artwork-coverage        <- one folder per pak, the folder is the id
            ├── app.ini             <- manifest, required
            ├── main.js             <- entry script, default name
            └── icon.png            <- optional, shown in the Apps list
```

`app.ini` keys:

| Key     | Required | Meaning |
| ------- | -------- | ------- |
| `title` | no       | Label in the Apps list. Defaults to the folder name. |
| `info`  | no       | One-line hint under the label. Defaults to `app pak`. |
| `icon`  | no       | Image inside the pak folder. Larger than 320x240 costs load time. |
| `entry` | no       | Script to run, relative to the pak folder. Defaults to `main.js`. |

The entry must be a `.js` or `.mjs` file inside the pak folder. `..`, `\`, `:`
and absolute paths are rejected, so a pak cannot point the launcher at another
file on the card. A folder without `app.ini` is ignored, which is why the
template ships as `app.ini.example`.

## Installing a pak

1. Unzip or copy the pak folder into `/unifrog_data/apps/`.
2. Open **Apps**. The pak appears after the built-in entries, with an `App paks`
   hint row showing where tools are loaded from.
3. Press **A** to run it. The launcher closes, the script runs, and the launcher
   comes back with `script returned <n>` in the status bar.

On a phone, any file manager that can extract a ZIP works: extract the archive
so that `app.ini` ends up directly inside `/unifrog_data/apps/<name>/`.

Up to 24 paks get an icon slot; more than that still run, they just fall back to
the theme's generic glyph. Removing a pak is deleting its folder.

## Writing a tool

Start from `paks/_template` in the repository, or copy the bundled
`artwork-coverage` pak and edit it. Useful pieces:

- `JS2300.fs.list(path)` - `{ name, type }` entries (`type` is `"dir"`/`"file"`
  or `2`/`1`), at most 1024 entries.
- `JS2300.fs.readText(path)` / `writeText(path, text)` - 2 MiB per call.
- `JS2300.fs.readBytesInto(path, uint8Array)` - binary reads up to 4 MiB.
- `JS2300.log(message)` - goes to the device log, which is what you read when a
  pak does not behave.
- `JS2300.system.action(...)` - `toast:<text>`, `script:<path>`, `storage:recover`
  and the `developer:*` diagnostics. Unsupported actions return failure.
- `JS2300.video.*` and `JS2300.input.poll()` for a drawn UI; see
  [js2300-scripting.md](js2300-scripting.md).

There is no `setTimeout`, no network, and no promises. Do short batches of work
and write partial results to a file so a slow card cannot lose everything.

### Example: Achievements

`paks/achievements` reads the definitions `ra_sync.mjs` already put on the card
(`achievements/cache/<hash>.json`) and the unlocked ids
(`achievements/earned/<hash>.txt`), then lists every cached game with its
progress. Press **A** on a game to see each achievement, marked `[x]` when it is
unlocked, with its points. Up/Down scrolls, L/R pages, **B** goes back, and
**B** in the list leaves the tool. The in-game pause menu shows the same data
for the game that is running; this pak is for browsing everything else.

### Example: Box Art Check

`paks/artwork-coverage` reads `rom_root` from `unifrog.ini`, walks every system
folder, compares the games against
`unifrog_data/artwork/<system>/box/<name>.png`, and writes
`unifrog_data/logs/reports/artwork-coverage.txt` with per-system counts and the
list of games that still have no box art. Open that file with
**Apps -> File Browser** to see it on the device.

## Overlay paks

An overlay pak draws over a game while a core runs: a bezel such as a GBA or
Game Boy frame, and optionally a small stats panel. It cannot use JS2300: the
script runtime is either standalone, the launcher, or the core itself, so a
third-party script cannot own a frame during somebody else's game. An overlay
pak therefore only *describes* what to draw, and the firmware draws it natively
into the presenter buffer every core frame.

```text
unifrog_data/apps/overlay-gba
├── overlay.ini        <- no app.ini, so it is not listed as a tool
└── frame.png          <- optional bezel
```

| Key        | Meaning |
| ---------- | ------- |
| `title`    | Name shown in the selector. Defaults to the folder name. |
| `image`    | PNG inside the pak, scaled to the whole screen. Pixels with alpha below 128 are skipped, so the transparent middle stays game. |
| `position` | Panel corner: `top-left`, `top-right` (default), `bottom-left`, `bottom-right`. |
| `line1`..`line4` | Panel lines, up to three tokens each, comma separated. |

`image` and the `lineN` panel can be used together; the panel is drawn on top
of the bezel. A pak with neither, or with a missing or unreadable image, is
skipped and logged.

The bezel is not blended per frame. At load the PNG is scaled once into runs of
opaque pixels, and each frame only those runs are copied and written back. The
shipped GBA frame covers 9,176 pixels, so a frame touches about 18 KiB instead
of the 150 KiB a full framebuffer flush costs. A thin bezel with a large
picture window is cheaper than a thick one; draw with hard edges, because
semi-transparent pixels are treated as visible or skipped, never mixed with the
game.

Tokens:

| Token          | Shows |
| -------------- | ----- |
| `clock`        | Device time as `HH:MM`. |
| `battery`      | Charge percent, or `--%` when the battery is unreadable. |
| `achievements` | Unlocked/total for the running game, `no ACH` when it is off. |
| `points`       | Achievement points earned in this game. |
| `game`         | The achievement title of the running game. |

Examples:

```ini
# paks/overlay-gba/overlay.ini - a 320x240 frame with 13px bands, the letterbox
# a 240x160 GBA image gets on this screen
title=GBA Frame
image=frame.png
```

```ini
# paks/overlay-stats/overlay.ini - clock, battery and progress in a corner
title=Stats
position=top-right
line1=clock
line2=battery
line3=achievements
```

Turn it on with **Config -> Custom -> Game Overlay**, which cycles through
`off` and every overlay pak on the card; the choice is saved as `overlay_pak` in
`unifrog.ini`. Folders with both `app.ini` and `overlay.ini` stay ordinary apps,
overlay folders are skipped by the Apps list, and a selected pak that is deleted
simply draws nothing.

The overlay is repainted every frame because the presenter rotates framebuffers,
but only its own rows are flushed to memory, so the cost is a few kilobytes
instead of the whole 150 KiB frame. System messages are drawn after the overlay:
the achievement unlock banner (`ACHIEVEMENT UNLOCKED`, 38px tall at the top of
the screen for `toast_ms`, 4s by default) and the low battery warning stay on
top of a bezel instead of being cut by it.

## Security

A pak runs with the same rights as any script in `/unifrog_data/scripts`: it can
read and write the SD card and call the system actions above. There is no
signing or sandbox, so only install paks you trust. The launcher never
auto-installs from an archive; every pak is a folder the user placed or
extracted themselves.

## Limits

- Listing scans at most 1024 entries per folder, so a pak that indexes a huge
  ROM tree should page through folders like the launcher does.
- Scripts are limited to a 512 KiB source and an 8 MiB heap.
- The Apps list reads every manifest each time it opens, which is cheap for a
  few dozen paks; keep artwork inside a pak small.

## Planned

- Installing a pak from a `.zip` in `unifrog_data/archive` without a computer.
- A bundled artwork pak that unpacks an artwork pack ZIP into the catalogue
  layout.
