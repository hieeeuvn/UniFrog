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
