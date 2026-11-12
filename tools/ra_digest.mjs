#!/usr/bin/env node
/*
 * RetroAchievements session digest for UniFrog.
 *
 * The card already holds everything needed to describe a play session: game
 * definitions under achievements/cache/, earned ids under achievements/earned/
 * and unlocks still waiting for upload in achievements/queue/unlocks.jsonl.
 * This tool reads those files and asks a model for a short, readable digest of
 * what the player unlocked, instead of leaving them with raw ids.
 *
 * The model call goes through the Respan hosted gateway, so the only
 * credential is a server-side RESPAN_API_KEY (no provider keys):
 *
 *   node tools/ra_digest.mjs --root /media/mmcblk0/unifrog_data
 *   node --env-file=.env tools/ra_digest.mjs --root .
 *   node tools/ra_digest.mjs --root . --dry-run     # print the request only
 *
 * Requests show up as traces at https://platform.respan.ai/platform/requests.
 * Node 18 or newer and no npm dependencies, like tools/ra_sync.mjs.
 */

import { existsSync } from 'node:fs';
import { mkdir, readFile, readdir, rename, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const GATEWAY = (process.env.RESPAN_BASE_URL || 'https://api.respan.ai/api')
  .replace(/\/$/, '');
const DEFAULT_MODEL = process.env.RESPAN_MODEL || 'gpt-5.5';
const MODEL_RE = /^[A-Za-z0-9._:/-]+$/;
const HASH_RE = /^[0-9a-f]{32}$/;
const REQUEST_TIMEOUT_MS = 60000;
const GAMES_MAX = 12;
const EARNED_MAX = 12;

const SYSTEM_PROMPT = `\
You write short session digests for a retro handheld. You get JSON describing \
the games on the SD card, the achievements the player has earned and the \
unlocks still waiting to be uploaded. Write 3-6 sentences of plain text, no \
markdown headings and no lists: what was played, what was unlocked (name the \
achievements), how much of each game is complete, and whether uploads are \
pending. Use only the JSON. If it is empty, say there is nothing to report. \
Never invent achievement names, games or scores.`;

function usage() {
  process.stdout.write(`\
RetroAchievements session digest for UniFrog

  node tools/ra_digest.mjs [options]

Options:
  --root <dir>      SD root, unifrog_data, or NocturnalRTOS system dir
  --model <name>    gateway model (default ${DEFAULT_MODEL}, or RESPAN_MODEL)
  --out <file>      write the digest to a file instead of stdout
  --dry-run         print the gateway request without sending it
  --quiet           only print the digest
  -h, --help        this text

The model call uses the Respan hosted gateway and needs a server-side
RESPAN_API_KEY; nothing else, and no npm dependencies. --dry-run needs no key.

  export RESPAN_API_KEY=...
  node tools/ra_digest.mjs --root /media/mmcblk0/unifrog_data
`);
}

function parseArgs(argv) {
  const options = {
    root: '',
    model: DEFAULT_MODEL,
    out: '',
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
      case '--model': options.model = next(); break;
      case '--out': options.out = next(); break;
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

  if (!MODEL_RE.test(options.model))
    throw new Error(`invalid model name ${options.model}`);
  return options;
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

function parseUnlocks(text) {
  const entries = [];
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line)
      continue;
    try {
      const entry = JSON.parse(line);
      if (entry && Number.isInteger(entry.a) && entry.a > 0)
        entries.push(entry);
    } catch {
      /* A malformed queue line is ra_sync's business, not the digest's. */
    }
  }
  return entries;
}

async function readJson(file) {
  try {
    return JSON.parse(await readFile(file, 'utf8'));
  } catch {
    return null;
  }
}

function achievementIndex(patch) {
  const index = new Map();
  const list = patch?.PatchData?.Achievements;
  if (!Array.isArray(list))
    return index;
  for (const entry of list) {
    const id = Number(entry?.ID);
    if (!Number.isInteger(id))
      continue;
    index.set(id, {
      id,
      title: String(entry.Title || `achievement ${id}`),
      description: String(entry.Description || ''),
      points: Number(entry.Points) || 0,
    });
  }
  return index;
}

async function collect(root, options) {
  const achievementsDir = path.join(root, 'achievements');
  const cacheDir = path.join(achievementsDir, 'cache');
  const earnedDir = path.join(achievementsDir, 'earned');

  const caches = (await readdir(cacheDir).catch(() => []))
    .filter((name) => name.endsWith('.json'))
    .sort();
  const earnedFiles = (await readdir(earnedDir).catch(() => []))
    .filter((name) => name.endsWith('.txt'))
    .sort();

  const patches = new Map();
  for (const name of caches) {
    const hash = name.replace(/\.json$/, '').toLowerCase();
    patches.set(hash, await readJson(path.join(cacheDir, name)));
  }

  const games = [];
  const unknown = [];
  const seen = new Set();

  for (const name of earnedFiles) {
    const hash = name.replace(/\.txt$/, '').toLowerCase();
    if (!HASH_RE.test(hash))
      continue;
    const ids = (await readFile(path.join(earnedDir, name), 'utf8').catch(() => ''))
      .split(/\r?\n/)
      .map((line) => Number(line.trim()))
      .filter((id) => Number.isInteger(id) && id > 0)
      .filter((id) => !seen.has(`${hash}:${id}`) && seen.add(`${hash}:${id}`));
    if (ids.length === 0)
      continue;

    const patch = patches.get(hash) || null;
    const index = achievementIndex(patch);
    const earned = ids
      .filter((id) => index.has(id))
      .slice(0, EARNED_MAX)
      .map((id) => index.get(id));
    for (const id of ids) {
      if (!index.has(id))
        unknown.push({ hash, id });
    }

    games.push({
      hash,
      title: String(patch?.PatchData?.Title || 'uncached game'),
      console: String(patch?.PatchData?.ConsoleName || ''),
      earned,
      earnedTotal: ids.length,
      achievementTotal: index.size,
      points: earned.reduce((total, entry) => total + entry.points, 0),
    });
  }

  const unlocks = parseUnlocks(
    await readFile(path.join(achievementsDir, 'queue', 'unlocks.jsonl'), 'utf8')
      .catch(() => ''));
  const pending = unlocks.slice(0, GAMES_MAX).map((entry) => {
    const hash = String(entry.h || '').toLowerCase();
    const index = achievementIndex(patches.get(hash));
    const known = index.get(entry.a);
    return {
      achievement: known ? known.title : `achievement ${entry.a}`,
      game: String(patches.get(hash)?.PatchData?.Title || hash || 'unknown'),
      hardcore: Number(entry.m) ? true : false,
      points: known ? known.points : Number(entry.p) || 0,
    };
  });

  if (!options.quiet) {
    process.stderr.write(
      `games=${games.length} earned=${games.reduce((n, g) => n + g.earnedTotal, 0)} ` +
      `pending=${unlocks.length} uncached_earned=${unknown.length}\n`);
  }

  return {
    games: games.slice(0, GAMES_MAX),
    pendingCount: unlocks.length,
    pending,
    uncachedEarnedCount: unknown.length,
  };
}

function buildRequest(options, summary) {
  return {
    model: options.model,
    temperature: 0.2,
    max_tokens: 400,
    metadata: {
      tool: 'unifrog-ra-digest',
      games: summary.games.length,
      pending: summary.pendingCount,
    },
    messages: [
      { role: 'system', content: SYSTEM_PROMPT },
      {
        role: 'user',
        content: 'Session data:\n' + JSON.stringify(summary, null, 2),
      },
    ],
  };
}

async function callGateway(request) {
  const key = process.env.RESPAN_API_KEY;
  if (!key)
    throw new Error(
      'RESPAN_API_KEY is not set: export it, or run with ' +
      '--env-file=.env after copying .env.example');

  let response;
  try {
    response = await fetch(`${GATEWAY}/chat/completions`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${key}`,
      },
      body: JSON.stringify(request),
      signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
    });
  } catch (error) {
    throw new Error(`gateway request failed: ${error.message}`);
  }

  const text = await response.text();
  if (!response.ok)
    throw new Error(`gateway HTTP ${response.status}: ${text.slice(0, 300)}`);

  let json;
  try {
    json = JSON.parse(text);
  } catch {
    throw new Error('gateway did not return JSON');
  }
  const digest = json?.choices?.[0]?.message?.content;
  if (typeof digest !== 'string' || !digest.trim())
    throw new Error('gateway returned no message content');
  return { digest: digest.trim(), json };
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
  const summary = await collect(root, options);
  const request = buildRequest(options, summary);

  if (options.dryRun) {
    if (!options.quiet)
      process.stderr.write(`POST ${GATEWAY}/chat/completions\n`);
    process.stdout.write(`${JSON.stringify(request, null, 2)}\n`);
    return;
  }

  const { digest, json } = await callGateway(request);

  if (options.out) {
    const file = path.resolve(options.out);
    await mkdir(path.dirname(file), { recursive: true });
    const temporary = `${file}.tmp`;
    await writeFile(temporary, `${digest}\n`);
    await rename(temporary, file);
    process.stdout.write(`${file}\n`);
  } else {
    process.stdout.write(`${digest}\n`);
  }

  const tokens = json?.usage;
  if (!options.quiet && tokens)
    process.stderr.write(`model=${json.model || options.model} ` +
      `tokens=${tokens.total_tokens ?? '?'}\n`);
}

main().catch((error) => {
  process.stderr.write(`ra_digest: ${error.message}\n`);
  process.exit(1);
});
