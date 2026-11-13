// Box art coverage for UniFrog app paks (standalone JS2300 script).
//
// Counts the games in every ROM system folder and how many of them have a box
// image in /unifrog_data/artwork/<system>/box/, then writes a readable report
// and shows a toast. Use it after copying an artwork pack to the card, or after
// running a scraper on a phone, to see what is still missing.
//
// The report is plain text, so it can be opened on the device from
// Apps -> File Browser -> unifrog_data/logs/reports/artwork-coverage.txt.

var DATA_ROOT = "/media/mmcblk0/unifrog_data";
var REPORT_PATH = DATA_ROOT + "/logs/reports/artwork-coverage.txt";
var DEFAULT_ROM_ROOT = "/media/mmcblk0/ROMS";
var MAX_REPORT_LINES = 400;

function trim(text) {
  var value = "" + text;
  var start = 0;
  var end = value.length;

  while (start < end && isSpace(value.charAt(start)))
    start++;
  while (end > start && isSpace(value.charAt(end - 1)))
    end--;
  return value.substring(start, end);
}

function isSpace(ch) {
  return ch === " " || ch === "\t" || ch === "\r" || ch === "\n";
}

function endsWithCI(text, suffix) {
  var offset = text.length - suffix.length;

  if (offset < 0)
    return false;
  return text.substring(offset).toLowerCase() === suffix;
}

function readRomRoot() {
  var text;
  var lines;
  var index;
  var line;

  try {
    text = JS2300.fs.readText(DATA_ROOT + "/unifrog.ini");
  } catch (readErr) {
    text = null;
  }
  if (!text)
    return DEFAULT_ROM_ROOT;
  lines = text.split("\n");
  for (index = 0; index < lines.length; index++) {
    line = trim(lines[index]);
    if (line.indexOf("rom_root=") === 0) {
      line = trim(line.substring("rom_root=".length));
      if (line.length > 0)
        return line;
    }
  }
  return DEFAULT_ROM_ROOT;
}

function isDirEntry(entry) {
  return entry.type === "dir" || entry.type === 2;
}

// JS2300.fs.list returns { name, type } entries, and an unreadable folder must
// not abort the whole report.
function listDir(path) {
  var out = [];
  var list;
  var index;
  var item;

  try {
    list = JS2300.fs.list(path);
  } catch (listErr) {
    return out;
  }
  if (!list)
    return out;
  for (index = 0; index < list.length; index++) {
    item = list[index];
    if (typeof item === "string")
      out.push({ name: item, type: "file" });
    else
      out.push({ name: item.name, type: item.type });
  }
  return out;
}

function stripExtension(name) {
  var dot = name.lastIndexOf(".");

  return dot > 0 ? name.substring(0, dot) : name;
}

function isImageName(name) {
  return endsWithCI(name, ".png") || endsWithCI(name, ".jpg") ||
    endsWithCI(name, ".jpeg") || endsWithCI(name, ".bmp");
}

function artworkIndex(system) {
  var index = {};
  var list = listDir(DATA_ROOT + "/artwork/" + system + "/box");
  var entry;
  var position;

  for (position = 0; position < list.length; position++) {
    if (isDirEntry(list[position]) || !isImageName(list[position].name))
      continue;
    entry = stripExtension(list[position].name).toLowerCase();
    if (entry.length > 0)
      index[entry] = true;
  }
  return index;
}

function run() {
  var romRoot = readRomRoot();
  var systems = listDir(romRoot);
  var lines = [];
  var systemCount = 0;
  var totalGames = 0;
  var covered = 0;
  var started = JS2300.now();
  var s;
  var g;
  var m;
  var system;
  var games;
  var art;
  var missing;
  var gameCount;
  var systemCovered;
  var gameName;

  for (s = 0; s < systems.length; s++) {
    if (!isDirEntry(systems[s]))
      continue;
    system = systems[s].name;
    games = listDir(romRoot + "/" + system);
    art = artworkIndex(system);
    missing = [];
    gameCount = 0;
    systemCovered = 0;
    for (g = 0; g < games.length; g++) {
      if (isDirEntry(games[g]))
        continue;
      gameName = stripExtension(games[g].name).toLowerCase();
      if (gameName.length === 0)
        continue;
      gameCount++;
      if (art[gameName])
        systemCovered++;
      else
        missing.push(games[g].name);
    }
    if (gameCount === 0)
      continue;
    systemCount++;
    totalGames += gameCount;
    covered += systemCovered;
    lines.push(system + ": " + systemCovered + "/" + gameCount);
    for (m = 0; m < missing.length; m++) {
      if (lines.length >= MAX_REPORT_LINES) {
        lines.push("... list truncated");
        break;
      }
      lines.push("  missing " + missing[m]);
    }
  }

  lines.push("");
  lines.push("root=" + romRoot);
  lines.push("systems=" + systemCount + " games=" + totalGames +
    " covered=" + covered + " missing=" + (totalGames - covered));
  lines.push("ms=" + (JS2300.now() - started));

  try {
    JS2300.fs.writeText(REPORT_PATH,
      "Box art coverage\n" + lines.join("\n") + "\n");
  } catch (writeErr) {
    JS2300.log("artwork-coverage write failed");
  }
  try {
    JS2300.system.action("toast:Box art " + covered + "/" + totalGames);
  } catch (toastErr) {}
  JS2300.log("artwork-coverage games=" + totalGames + " covered=" + covered +
    " systems=" + systemCount);
  return 0;
}

run();
