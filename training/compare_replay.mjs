// One fresh process/engine per replay; spectator diagnostics, never training data.
import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { createInterface } from 'node:readline';
import { resolve, join } from 'node:path';
import { pathToFileURL } from 'node:url';

export function firstDifference(a, b, path = '') {
  if (a === b) return null;
  if (a === null || b === null || typeof a !== typeof b || typeof a !== 'object')
    return { path, native: a, reference: b };
  const ak = Object.keys(a), bk = Object.keys(b);
  if (ak.length !== bk.length) return { path: `${path}.length`, native: ak.length, reference: bk.length };
  for (const key of ak) {
    if (!Object.hasOwn(b, key)) return { path: `${path}.${key}`, native: a[key], reference: null };
    const diff = firstDifference(a[key], b[key], `${path}.${key}`);
    if (diff) return diff;
  }
  return null;
}

const unitFields = ['index', 'type', 'owner', 'completed', 'hidden', 'x', 'y',
  'hpRaw', 'shieldsRaw', 'energyRaw', 'remainingBuildTime'];
const playerFields = ['owner', 'minerals', 'gas', 'usedSupplyRaw', 'maxSupplyRaw'];
const pick = (obj, fields) => Object.fromEntries(fields.map(key => [key, obj[key]]));
const hash = bytes => createHash('sha256').update(bytes).digest('hex');

async function main() {
  const [backendArg, replayArg, checkpointsArg, intervalArg] = process.argv.slice(2);
  if (!intervalArg) throw new Error('Usage: node compare_replay.mjs BACKEND REPLAY NATIVE_JSONL INTERVAL');
  const [major, minor] = process.versions.node.split('.').map(Number);
  if (major < 24 || (major === 24 && minor < 5)) throw new Error('Node 24.5+ required for Memory64');
  const interval = Number(intervalArg);
  if (!Number.isInteger(interval) || interval < 1 || interval > 2400) throw new Error('Invalid interval');
  const backend = resolve(backendArg);
  const provenance = JSON.parse(await readFile(join(backend, 'provenance.json')));
  for (const key of ['originalWasm', 'patchedWasm', 'simulationAssetPack']) {
    const item = provenance.artifacts[key];
    if (hash(await readFile(join(backend, item.path))) !== item.sha256) throw new Error(`Artifact changed: ${key}`);
  }
  const { Bwsim } = await import(pathToFileURL(join(backend, 'dist/index.js')).href);
  const sim = await Bwsim.create({
    wasmPath: join(backend, provenance.artifacts.patchedWasm.path),
    assetPackPath: join(backend, provenance.artifacts.simulationAssetPack.path),
  });
  await sim.loadReplay(replayArg);
  const header = sim.replayHeader();
  if (!header || header.frameCount < 1) throw new Error('Invalid reference header');
  const result = { status: 'checkpoints_matched', end_frame: header.frameCount,
    map: header.mapName, unit_id_namespace: sim.replayUnitIdNamespace(),
    checkpoint_interval: interval, checkpoints: 0, mismatching_checkpoints: 0,
    first_difference: null, compared_unit_fields: [...unitFields, 'queue'],
    compared_player_fields: playerFields, training_ready: false,
    excluded_unit_state: ['turret subunits', 'Scanner Sweep'],
    authoritative_game_validated: false };
  let previousFrame = -interval;
  const nativeDigest = createHash('sha256'), referenceDigest = createHash('sha256');
  for await (const line of createInterface({ input: createReadStream(checkpointsArg), crlfDelay: Infinity })) {
    const native = JSON.parse(line);
    const expectedFrame = previousFrame < 0 ? 0 : Math.min(header.frameCount, previousFrame + interval);
    if (previousFrame === header.frameCount || native.frame !== expectedFrame)
      throw new Error(`Missing, duplicate or out-of-order checkpoint at ${native.frame}; expected ${expectedFrame}`);
    sim.stepTo(native.frame);
    if (sim.currentFrame() !== native.frame) throw new Error('Reference engine failed to advance');
    const reference = { frame: sim.currentFrame(), players: sim.players().map(p => pick(p, playerFields)),
      units: sim.units().map(u => ({ ...pick(u, unitFields), queue: sim.unitBuildQueue(u.index) }))
        .sort((a, b) => a.index - b.index) };
    const diff = firstDifference(native, reference);
    if (diff) {
      result.mismatching_checkpoints++;
      if (!result.first_difference) result.first_difference = { frame: native.frame, ...diff };
    }
    nativeDigest.update(JSON.stringify(native)); referenceDigest.update(JSON.stringify(reference));
    result.checkpoints++;
    previousFrame = native.frame;
  }
  if (previousFrame !== header.frameCount) throw new Error('Missing final replay checkpoint');
  if (result.mismatching_checkpoints) result.status = 'quarantined';
  result.native_checkpoints_sha256 = nativeDigest.digest('hex');
  result.reference_checkpoints_sha256 = referenceDigest.digest('hex');
  console.log(JSON.stringify(result));
  if (result.status !== 'checkpoints_matched') process.exitCode = 1;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { console.error(String(error)); process.exitCode = 1; });
}
