// Achievements viewer for UniFrog app paks (standalone JS2300 script).
//
// The handheld has no network, so RetroAchievements definitions arrive on the
// SD card from tools/ra_sync.mjs. This pak reads what is already there:
//
//   achievements/cache/<hash>.json   raw "patch" response with the definitions
//   achievements/earned/<hash>.txt   unlocked ids, one per line
//
// It lists every cached game with its progress and opens the definition list,
// which is otherwise only reachable while a game is running.

var DATA_ROOT = "/media/mmcblk0/unifrog_data";
var ACH_ROOT = DATA_ROOT + "/achievements";
var CACHE_DIR = ACH_ROOT + "/cache";
var EARNED_DIR = ACH_ROOT + "/earned";
var CONFIG_PATH = DATA_ROOT + "/unifrog.ini";

// unifrog_button order from include/unifrog/input.h.
var BTN_L = 8;
var BTN_A = 16;
var BTN_B = 32;
var BTN_START = 128;
var BTN_UP = 256;
var BTN_DOWN = 512;
var BTN_LEFT = 1024;
var BTN_RIGHT = 2048;

var COLOR_BG = 0x0000;
var COLOR_TEXT = 0xffff;
var COLOR_DIM = 0xbdf7;
var COLOR_FAINT = 0x8410;
var COLOR_EARNED = 0x07e0;
var COLOR_POINTS = 0xffe0;

var LIST_ROWS = 9;
var DETAIL_ROWS = 11;
var LIST_ROW_H = 18;
var DETAIL_ROW_H = 16;
var LIST_TOP = 34;
var DETAIL_TOP = 30;
var CHAR_W = 6;
var SCREEN_W = 320;

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

function isDirEntry(entry) {
  return entry.type === "dir" || entry.type === 2;
}

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

function readTextOrNull(path) {
  try {
    return JS2300.fs.readText(path);
  } catch (readErr) {
    return null;
  }
}

// Cut to the pixel width the row can show; the font is fixed width.
function cut(text, columns) {
  var value = "" + text;

  if (value.length <= columns)
    return value;
  if (columns <= 1)
    return "~";
  return value.substring(0, columns - 1) + "~";
}

function rightText(y, text, color) {
  var value = "" + text;
  var x = SCREEN_W - 8 - value.length * CHAR_W;

  if (x < 0)
    x = 0;
  JS2300.video.text(x, y, value, color);
}

function loadEarned(hash) {
  var ids = {};
  var text = readTextOrNull(EARNED_DIR + "/" + hash + ".txt");
  var lines;
  var index;
  var line;

  if (!text)
    return ids;
  lines = text.split("\n");
  for (index = 0; index < lines.length; index++) {
    line = trim(lines[index]);
    if (line.length > 0)
      ids[line] = true;
  }
  return ids;
}

function gameProgress(game) {
  var total = game.achievements.length;
  var unlocked = 0;
  var points = 0;
  var index;
  var entry;

  for (index = 0; index < total; index++) {
    entry = game.achievements[index];
    if (game.earned[String(entry.ID)]) {
      unlocked++;
      points += entry.Points ? entry.Points : 0;
    }
  }
  game.total = total;
  game.unlocked = unlocked;
  game.points = points;
}

function loadGames() {
  var games = [];
  var list = listDir(CACHE_DIR);
  var index;
  var name;
  var hash;
  var text;
  var patch;
  var game;

  for (index = 0; index < list.length; index++) {
    name = list[index].name || "";
    if (isDirEntry(list[index]) || !endsWithCI(name, ".json"))
      continue;
    hash = name.substring(0, name.length - 5);
    text = readTextOrNull(CACHE_DIR + "/" + name);
    if (!text)
      continue;
    try {
      patch = JSON.parse(text);
    } catch (parseErr) {
      JS2300.log("achievements cache unreadable hash=" + hash);
      continue;
    }
    if (!patch || !patch.PatchData)
      continue;
    game = {
      hash: hash,
      title: String(patch.PatchData.Title || hash),
      achievements: patch.PatchData.Achievements || [],
      earned: loadEarned(hash),
    };
    gameProgress(game);
    games.push(game);
  }
  sortGames(games);
  return games;
}

function sortGames(games) {
  var index;
  var position;
  var current;
  var key;

  for (index = 1; index < games.length; index++) {
    current = games[index];
    key = current.title.toLowerCase();
    position = index - 1;
    while (position >= 0 && games[position].title.toLowerCase() > key) {
      games[position + 1] = games[position];
      position--;
    }
    games[position + 1] = current;
  }
}

function readUsername() {
  var text = readTextOrNull(ACH_ROOT + "/ra.ini");
  var lines;
  var index;
  var line;

  if (!text)
    return "";
  lines = text.split("\n");
  for (index = 0; index < lines.length; index++) {
    line = trim(lines[index]);
    if (line.indexOf("username=") === 0)
      return trim(line.substring("username=".length));
  }
  return "";
}

function offsetFor(selected, rows, count) {
  var offset;

  if (count <= rows || selected < rows)
    return 0;
  offset = selected - rows + 1;
  if (offset > count - rows)
    offset = count - rows;
  return offset < 0 ? 0 : offset;
}

function drawList(state) {
  var games = state.games;
  var offset = offsetFor(state.selected, LIST_ROWS, games.length);
  var row;
  var game;
  var y;
  var summary;

  JS2300.video.clear(COLOR_BG);
  JS2300.video.text(8, 6, "Achievements", COLOR_TEXT);
  if (state.username.length > 0)
    rightText(6, state.username, COLOR_DIM);
  else
    rightText(6, games.length + " games", COLOR_DIM);

  if (games.length === 0) {
    JS2300.video.text(8, 60, "No cached definitions.", COLOR_TEXT);
    JS2300.video.text(8, 80, "Run ra_sync on a phone", COLOR_DIM);
    JS2300.video.text(8, 96, "and open this pak again.", COLOR_DIM);
    JS2300.video.text(8, 222, "B back", COLOR_FAINT);
    JS2300.video.present();
    return;
  }

  for (row = 0; row < LIST_ROWS; row++) {
    if (offset + row >= games.length)
      break;
    game = games[offset + row];
    y = LIST_TOP + row * LIST_ROW_H;
    if (offset + row === state.selected) {
      JS2300.video.text(2, y, ">", COLOR_POINTS);
      JS2300.video.text(10, y, cut(game.title, 34), COLOR_TEXT);
    } else {
      JS2300.video.text(10, y, cut(game.title, 34), COLOR_DIM);
    }
    summary = game.unlocked + "/" + game.total;
    rightText(y, summary, game.unlocked === game.total && game.total > 0 ?
      COLOR_EARNED : COLOR_DIM);
  }
  JS2300.video.text(8, 222,
    games.length > LIST_ROWS ? "A open  B back  UP/DOWN scroll" :
    "A open  B back", COLOR_FAINT);
  JS2300.video.present();
}

function drawDetail(state) {
  var game = state.games[state.selected];
  var rows = game.achievements.length;
  var offset = offsetFor(state.detail, DETAIL_ROWS, rows);
  var row;
  var entry;
  var y;
  var earned;
  var title;

  JS2300.video.clear(COLOR_BG);
  JS2300.video.text(8, 4, cut(game.title, 44), COLOR_TEXT);
  rightText(18, game.unlocked + "/" + game.total + "  " + game.points + "pt",
    game.unlocked === game.total && game.total > 0 ? COLOR_EARNED : COLOR_DIM);

  if (rows === 0) {
    JS2300.video.text(8, 60, "This game has no achievements.", COLOR_DIM);
  }
  for (row = 0; row < DETAIL_ROWS; row++) {
    if (offset + row >= rows)
      break;
    entry = game.achievements[offset + row];
    y = DETAIL_TOP + row * DETAIL_ROW_H;
    earned = game.earned[String(entry.ID)] ? true : false;
    title = cut(entry.Title ? entry.Title : ("#" + entry.ID), 39);
    JS2300.video.text(8, y, earned ? "[x]" : "[ ]",
      earned ? COLOR_EARNED : COLOR_FAINT);
    JS2300.video.text(34, y, title, earned ? COLOR_TEXT : COLOR_DIM);
    rightText(y, (entry.Points ? entry.Points : 0) + "", COLOR_POINTS);
  }
  JS2300.video.text(8, 222,
    rows > DETAIL_ROWS ? "B back  UP/DOWN scroll  L/R page" : "B back",
    COLOR_FAINT);
  JS2300.video.present();
}

function run() {
  var state = {
    games: loadGames(),
    username: readUsername(),
    selected: 0,
    detail: 0,
    mode: "list",
  };
  var previous = 0;
  var mask;
  var pressed;
  var game;
  var rows;
  var page;

  JS2300.log("achievements pak games=" + state.games.length);
  drawList(state);
  while (true) {
    mask = JS2300.input.poll() || 0;
    pressed = mask & ~previous;
    previous = mask;
    if (!pressed) {
      JS2300.sleep(40);
      continue;
    }
    if (state.mode === "list") {
      if (pressed & BTN_UP) {
        if (state.selected > 0)
          state.selected--;
        drawList(state);
      } else if (pressed & BTN_DOWN) {
        if (state.selected + 1 < state.games.length)
          state.selected++;
        drawList(state);
      } else if (pressed & BTN_A) {
        if (state.games.length > 0) {
          state.mode = "detail";
          state.detail = 0;
          drawDetail(state);
        }
      } else if ((pressed & BTN_B) || (pressed & BTN_START)) {
        return 0;
      }
    } else {
      game = state.games[state.selected];
      rows = game.achievements.length;
      page = DETAIL_ROWS;
      if (pressed & BTN_UP) {
        if (state.detail > 0)
          state.detail--;
        drawDetail(state);
      } else if (pressed & BTN_DOWN) {
        if (state.detail + 1 < rows)
          state.detail++;
        drawDetail(state);
      } else if (pressed & BTN_LEFT) {
        state.detail -= page;
        if (state.detail < 0)
          state.detail = 0;
        drawDetail(state);
      } else if (pressed & BTN_RIGHT) {
        state.detail += page;
        if (state.detail > rows - 1)
          state.detail = rows > 0 ? rows - 1 : 0;
        drawDetail(state);
      } else if ((pressed & BTN_B) || (pressed & BTN_L)) {
        state.mode = "list";
        drawList(state);
      }
    }
    JS2300.sleep(40);
  }
}

run();
