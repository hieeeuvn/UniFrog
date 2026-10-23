#!/usr/bin/env node
/*
 * RetroAchievements companion for UniFrog / NocturnalRTOS.
 *
 * The handhelds have no network hardware, so the emulator only talks to the
 * SD card. This tool is run on a PC or phone that has the card mounted and
 * exchanges three things with retroachievements.org:
 *
 *   achievements/queue/*.req        game hashes the device could not identify
 *   achievements/cache/<hash>.json  raw "patch" responses for the device
 *   achievements/queue/unlocks.jsonl unlocks waiting to be awarded
 *
 * Usage:
 *   node tools/ra_sync.mjs --root /media/mmcblk0/unifrog_data   # UniFrog
 *   node tools/ra_sync.mjs --root /media/mmcblk0p2/system       # NocturnalRTOS
 *   node tools/ra_sync.mjs --root . --pull-only
 *   node tools/ra_sync.mjs --root . --push-only
 *   node tools/ra_sync.mjs --root . --list
 *
 * Credentials come from (highest priority first):
 *   --user/--key, RA_USERNAME/RA_API_KEY, achievements/ra_sync.ini, ra.ini
 *
 * RetroAchievements calls this an API key: Profile -> Settings -> Keys ->
 * Web API Key. It is not the account password.
 */

import { createHash } from 'node:crypto';
import { existsSync } from 'node:fs';
import { mkdir, readFile, readdir, rename, rm, unlink, writeFile }
  from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const DOREQUEST = process.env.RA_HOST || 'https://retroachievements.org/dorequest.php';
const USER_AGENT = 'unifrog-ra-sync/1.0';
const REQUEST_TIMEOUT_MS = 20000;
const HASH_RE = /^[0-9a-fA-F]{32}$/;

function usage() {
  process.stdout.write(`\
RetroAchievements sync for UniFrog and NocturnalRTOS

  node tools/ra_sync.mjs [options]

Options:
  --root <dir>      SD root, unifrog_data, or NocturnalRTOS system dir
  --user <name>     RetroAchievements username
  --key <key>       RetroAchievements web API key
  --pull            download definitions for queued game hashes
  --push            upload queued unlocks
  --pull-only       same as --pull, do not upload
  --push-only       same as --push, do not download
  --refresh <hash>  re-download one cached game even if it exists
  --refresh-all     re-download every cached game
  --list            show cached games, pending requests and unlocks
  --dry-run         report what would happen without network or file changes
  --quiet           only print errors and the final summary
  -h, --help        this text

Default action is --pull and --push. The tool needs Node 18 or newer and has
no npm dependencies.
`);
}

function parseArgs(argv) {
  const options = {
    root: '',
    user: '',
    key: '',
    pull: false,
    push: false,
    list: false,
    refresh: new Set(),
    refreshAll: false,
    dryRun: false,
    quiet: false,
    help: false,
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length)
        throw new Error(`${arg} needs a value`);
      return argv[++i];
    };

    switch (arg) {
      case '--root': options.root = next(); break;
      case '--user': options.user = next(); break;
      case '--key': options.key = next(); break;
      case '--pull': options.pull = true; break;
      case '--push': options.push = true; break;
      case '--pull-only': options.pull = true; break;
      case '--push-only': options.push = true; break;
      case '--refresh': options.refresh.add(next().toLowerCase()); break;
      case '--refresh-all': options.refreshAll = true; break;
      case '--list': options.list = true; break;
      case '--dry-run': options.dryRun = true; break;
      case '--quiet': options.quiet = true; break;
      case '-h':
      case '--help': options.help = true; break;
      default:
        if (arg.startsWith('-'))
          throw new Error(`unknown option ${arg}`);
        if (!options.root)
          options.root = arg;
        else
          throw new Error(`unexpected argument ${arg}`);
    }
  }

  if (!options.pull && !options.push && !options.list &&
      options.refresh.size === 0 && !options.refreshAll)
    options.pull = options.push = true;
  if (options.refresh.size > 0 || options.refreshAll)
    options.pull = true;
  return options;
}

function parseIni(text) {
  const values = {};
  let section = '';
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#') || line.startsWith(';'))
      continue;
    if (line.startsWith('[') && line.endsWith(']')) {
      section = line.slice(1, -1).trim().toLowerCase();
      continue;
    }
    const equals = line.indexOf('=');
    if (equals < 0)
      continue;
    const key = line.slice(0, equals).trim().toLowerCase();
    const value = line.slice(equals + 1).trim();
    values[`${section}.${key}`] = value;
    if (!(key in values))
      values[key] = value;
  }
  return values;
}

async function readIni(file) {
  try {
    return parseIni(await readFile(file, 'utf8'));
  } catch {
    return {};
  }
}

async function resolveRoot(explicit) {
  const candidates = [];
  if (explicit)
    candidates.push(explicit);
  if (process.env.UNIFROG_DATA_ROOT)
    candidates.push(process.env.UNIFROG_DATA_ROOT);
  candidates.push(
    '/media/mmcblk0/unifrog_data',
    '/sdcard/unifrog_data',
    '/mnt/sdcard/unifrog_data',
    '/media/mmcblk0p2/system',
    path.join(process.cwd(), 'unifrog_data'),
    process.cwd(),
  );

  for (const candidate of candidates) {
    const resolved = path.resolve(candidate);
    if (path.basename(resolved) === 'achievements')
      return path.dirname(resolved);
    if (existsSync(path.join(resolved, 'achievements')) ||
        existsSync(path.join(resolved, 'unifrog.ini')))
      return resolved;
  }
  if (explicit)
    return path.resolve(explicit);
  throw new Error('could not find an achievements directory; pass --root <dir>');
}

function signature(achievementId, username, hardcore, seconds) {
  let input = `${achievementId}${username}${hardcore ? 1 : 0}`;
  if (seconds)
    input += `${achievementId}${seconds}`;
  return createHash('md5').update(input).digest('hex');
}

function logLine(file, message) {
  const line = `${new Date().toISOString()} ${message}\n`;
  return writeFile(file, line, { flag: 'a' }).catch(() => {});
}

async function dorequest(params) {
  const body = new URLSearchParams(params);
  let lastError;

  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const response = await fetch(DOREQUEST, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/x-www-form-urlencoded',
          'User-Agent': USER_AGENT,
        },
        body,
        signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
      });
      const text = await response.text();
      if (!response.ok)
        throw new Error(`HTTP ${response.status}`);
      let json;
      try {
        json = JSON.parse(text);
      } catch {
        throw new Error('server did not return JSON (Cloudflare block?)');
      }
      return { json, text };
    } catch (error) {
      lastError = error;
      if (attempt === 0)
        await new Promise((resolve) => setTimeout(resolve, 1500));
    }
  }
  throw lastError;
}

function serverMessage(json) {
  if (!json)
    return 'no response';
  const error = json.Error || json.error || json.Message || json.message;
  if (error)
    return String(error);
  if (json.Code)
    return `code ${json.Code}`;
  return 'request rejected';
}

async function writeAtomic(file, text) {
  const temporary = `${file}.tmp`;
  await writeFile(temporary, text);
  await rename(temporary, file);
}

function unlockKey(entry) {
  /* "h" is the game hash, "m" is the hardcore flag (see the device engine). */
  return `${entry.a}:${Number(entry.m) ? 1 : 0}:${entry.h || ''}`;
}

function parseUnlocks(text) {
  const entries = [];
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line)
      continue;
    try {
      const entry = JSON.parse(line);
      if (!entry || !Number.isInteger(entry.a) || entry.a <= 0)
        throw new Error('missing achievement id');
      entries.push({ entry, raw: line });
    } catch {
      entries.push({ entry: null, raw: line });
    }
  }
  return entries;
}

class Sync {
  constructor(root, options) {
    this.root = root;
    this.options = options;
    this.achievements = path.join(root, 'achievements');
    this.cacheDir = path.join(this.achievements, 'cache');
    this.queueDir = path.join(this.achievements, 'queue');
    this.unlockFile = path.join(this.queueDir, 'unlocks.jsonl');
    this.logFile = path.join(this.achievements, 'sync.log');
    this.user = '';
    this.key = '';
    this.stats = {
      requests: 0,
      cached: 0,
      unknown: 0,
      unlocks: 0,
      awarded: 0,
      failed: 0,
    };
  }

  say(message) {
    if (!this.options.quiet)
      process.stdout.write(`${message}\n`);
  }

  async init() {
    const config = await readIni(path.join(this.achievements, 'ra_sync.ini'));
    const device = await readIni(path.join(this.achievements, 'ra.ini'));

    this.user = this.options.user ||
      process.env.RA_USERNAME || process.env.RA_USER ||
      config['retroachievements.username'] || config.username ||
      device['retroachievements.username'] || device.username || '';
    this.key = this.options.key ||
      process.env.RA_API_KEY || process.env.RA_KEY ||
      config['retroachievements.apikey'] ||
      config['retroachievements.api_key'] ||
      config.apikey || config.api_key || '';

    if (!this.options.dryRun) {
      await mkdir(this.cacheDir, { recursive: true });
      await mkdir(this.queueDir, { recursive: true });
    }
  }

  async login() {
    if (this.options.dryRun)
      return true;
    if (!this.user || !this.key)
      throw new Error('missing credentials: pass --user/--key or write achievements/ra_sync.ini');
    const { json } = await dorequest({ r: 'login2', u: this.user, t: this.key });
    if (!json.Success) {
      const message = json.Error ? `: ${json.Error}` :
        ' (use the Web API Key, not the account password)';
      throw new Error(`login failed${message}`);
    }
    this.say(`Logged in as ${this.user}`);
    return true;
  }

  async list() {
    const caches = await readdir(this.cacheDir).catch(() => []);
    const requests = (await readdir(this.queueDir).catch(() => []))
      .filter((name) => name.endsWith('.req'));
    const unlocks = parseUnlocks(
      await readFile(this.unlockFile, 'utf8').catch(() => ''));

    this.say(`SD root: ${this.root}`);
    this.say(`Cached games: ${caches.filter((name) => name.endsWith('.json')).length}`);
    for (const name of caches.filter((entry) => entry.endsWith('.json')).sort())
      this.say(`  ${name.replace(/\.json$/, '')}`);
    this.say(`Pending requests: ${requests.length}`);
    for (const name of requests.sort()) {
      const meta = await readFile(path.join(this.queueDir, name), 'utf8')
        .then((text) => JSON.parse(text)).catch(() => ({}));
      this.say(`  ${name.replace(/\.req$/, '')} ` +
        `console=${meta.console ?? '?'} ${meta.content || ''}`);
    }
    this.say(`Queued unlocks: ${unlocks.filter((item) => item.entry).length}`);
    for (const { entry } of unlocks) {
      if (entry)
        this.say(`  achievement ${entry.a} ${entry.h || ''} ` +
          `hardcore=${Number(entry.m) ? 1 : 0} points=${entry.p || 0}`);
    }
  }

  async pullOne(hash, { force = false } = {}) {
    if (!HASH_RE.test(hash)) {
      this.say(`Skipping invalid hash ${hash}`);
      return false;
    }
    const cacheFile = path.join(this.cacheDir, `${hash}.json`);
    if (!force && existsSync(cacheFile)) {
      this.say(`${hash}: already cached`);
      return true;
    }
    if (this.options.dryRun) {
      this.say(`${hash}: would download definitions`);
      return true;
    }

    this.stats.requests++;
    const { json, text } = await dorequest({
      r: 'patch',
      u: this.user,
      t: this.key,
      m: hash,
    });
    if (!json.Success) {
      this.stats.unknown++;
      const message = `${hash}: ${serverMessage(json)}`;
      this.say(message);
      await logLine(this.logFile, message);
      return false;
    }
    await writeAtomic(cacheFile, text);
    this.stats.cached++;
    const title = json.PatchData?.Title || 'unknown game';
    this.say(`${hash}: cached "${title}" ` +
      `(${json.PatchData?.Achievements?.length ?? 0} achievements)`);
    return true;
  }

  async pull() {
    const requests = (await readdir(this.queueDir).catch(() => []))
      .filter((name) => name.endsWith('.req'));

    for (const name of requests) {
      const file = path.join(this.queueDir, name);
      const hash = name.replace(/\.req$/, '').toLowerCase();
      const meta = await readFile(file, 'utf8')
        .then((text) => JSON.parse(text)).catch(() => ({}));
      if (meta.content)
        this.say(`${hash}: requested by ${meta.content}`);

      if (await this.pullOne(hash)) {
        if (!this.options.dryRun)
          await unlink(file).catch(() => {});
      }
    }

    const refreshes = [...this.options.refresh];
    if (this.options.refreshAll) {
      const cached = await readdir(this.cacheDir).catch(() => []);
      for (const name of cached) {
        if (name.endsWith('.json'))
          refreshes.push(name.replace(/\.json$/, '').toLowerCase());
      }
    }
    for (const hash of refreshes)
      await this.pullOne(hash, { force: true });
  }

  async push() {
    let text;
    try {
      text = await readFile(this.unlockFile, 'utf8');
    } catch {
      return;
    }
    const entries = parseUnlocks(text);
    if (entries.length === 0)
      return;

    const seen = new Set();
    const remaining = [];
    const now = Math.floor(Date.now() / 1000);

    for (const item of entries) {
      if (!item.entry) {
        this.say(`Dropping malformed unlock: ${item.raw}`);
        continue;
      }
      const entry = item.entry;
      const key = unlockKey(entry);
      if (seen.has(key)) {
        this.say(`Dropping duplicate unlock ${key}`);
        continue;
      }
      seen.add(key);

      const hardcore = Number(entry.m) ? 1 : 0;
      const seconds = Math.max(0, now - (Number(entry.t) || now));
      const params = {
        r: 'awardachievement',
        u: this.user,
        t: this.key,
        a: String(entry.a),
        h: String(hardcore),
        v: signature(entry.a, this.user, hardcore, seconds),
      };
      if (entry.h && HASH_RE.test(entry.h))
        params.m = String(entry.h);
      if (seconds > 0)
        params.o = String(seconds);

      if (this.options.dryRun) {
        this.say(`achievement ${entry.a}: would upload (${seconds}s late)`);
        remaining.push(item.raw);
        continue;
      }

      let response;
      try {
        response = await dorequest(params);
      } catch (error) {
        this.stats.failed++;
        this.say(`achievement ${entry.a}: ${error.message}`);
        remaining.push(item.raw);
        continue;
      }

      const json = response.json;
      const already = !json.Success &&
        /already|previously|duplicate/i.test(serverMessage(json));
      if (json.Success || already) {
        this.stats.awarded++;
        this.stats.unlocks++;
        this.say(`achievement ${entry.a}: awarded` +
          (already ? ' (already unlocked)' : ''));
      } else {
        this.stats.failed++;
        const message = `achievement ${entry.a} (${entry.m || 'no hash'}): ` +
          serverMessage(json);
        this.say(message);
        await logLine(this.logFile, message);
        remaining.push(item.raw);
      }
    }

    const rewritten = remaining.length ? `${remaining.join('\n')}\n` : '';
    if (!this.options.dryRun) {
      if (rewritten)
        await writeAtomic(this.unlockFile, rewritten);
      else
        await rm(this.unlockFile, { force: true });
    }
  }
}

async function main() {
  let options;
  try {
    options = parseArgs(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`${error.message}\n\n`);
    usage();
    process.exit(2);
  }
  if (options.help) {
    usage();
    return;
  }

  const root = await resolveRoot(options.root);
  const sync = new Sync(root, options);
  await sync.init();

  if (options.list) {
    await sync.list();
    return;
  }

  await sync.login();
  if (options.pull)
    await sync.pull();
  if (options.push)
    await sync.push();

  if (!options.quiet) {
    process.stdout.write(`\
Summary: requests=${sync.stats.requests} cached=${sync.stats.cached} \
unknown=${sync.stats.unknown} awarded=${sync.stats.awarded} \
failed=${sync.stats.failed}
`);
    process.stdout.write(
      'Eject the card safely before removing it from the reader.\n');
  }
}

main().catch((error) => {
  process.stderr.write(`ra_sync: ${error.message}\n`);
  process.exit(1);
});
