// Capability probe only. Spectator snapshots from this tool MUST NOT be used as
// player observations or training samples. Reaching EOF is not a desync audit.
import { createHash } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { pathToFileURL } from 'node:url';

const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const args = process.argv.slice(2);
const backend = args.shift();
const output = args.shift();
if (!backend || !output || !args.length) {
  throw new Error('Usage: node training/probe_replay_backend.mjs BACKEND_DIRECTORY OUTPUT.json REPLAY.rep [REPLAY.rep ...]');
}
const [nodeMajor, nodeMinor] = process.versions.node.split('.').map(Number);
if (nodeMajor < 24 || (nodeMajor === 24 && nodeMinor < 5)) throw new Error('Node 24.5+ required for Memory64');
const directory = resolve(backend);
const provenanceBytes = await readFile(join(directory, 'provenance.json'));
const provenance = JSON.parse(provenanceBytes);
const artifacts = {};
for (const key of ['originalWasm', 'patchedWasm', 'simulationAssetPack']) {
  const artifact = provenance.artifacts[key];
  const actual = sha256(await readFile(join(directory, artifact.path)));
  if (actual !== artifact.sha256) throw new Error(`Backend artifact hash mismatch: ${key}`);
  artifacts[key] = { path: artifact.path, sha256: actual };
}
const wrapperBytes = await readFile(join(directory, 'dist/bwsim.js'));
const { Bwsim } = await import(pathToFileURL(join(directory, 'dist/index.js')).href);
const records = [];
const backendOptions = {
  wasmPath: join(directory, provenance.artifacts.patchedWasm.path),
  assetPackPath: join(directory, provenance.artifacts.simulationAssetPack.path),
};
let sim;
function snapshot() {
  return { frame: sim.currentFrame(), players: sim.players(),
    units: sim.units().map(unit => ({ ...unit, instanceId: sim.unitInstanceId(unit.index),
      queue: sim.unitBuildQueue(unit.index) })).sort((a, b) => a.index - b.index) };
}
for (const arg of args) {
  const replay = resolve(arg);
  const bytes = await readFile(replay);
  const record = { replay, sha256: sha256(bytes), training_ready: false };
  const started = performance.now();
  try {
    sim = await Bwsim.create(backendOptions);
    await sim.loadReplayBytes(bytes);
    const header = sim.replayHeader();
    if (!header || sim.currentFrame() !== 0 || header.frameCount <= 0) throw new Error('Invalid initial replay state');
    const players = header.players.filter(player => player.controller === 2 && player.playerId >= 0);
    const initial = snapshot();
    const pilotFrame = Math.min(2400, header.frameCount);
    sim.stepTo(pilotFrame);
    const direct = snapshot();
    const directDigest = sha256(JSON.stringify(direct));
    // Independent instances keep unit-generation counters and other reload
    // history out of this comparison. A production worker must use the same
    // isolation until reload semantics have separately passed their own audit.
    sim = await Bwsim.create(backendOptions);
    await sim.loadReplayBytes(bytes);
    while (sim.currentFrame() < pilotFrame) {
      const target = Math.min(pilotFrame, sim.currentFrame() + 24);
      sim.step(target - sim.currentFrame());
      if (sim.currentFrame() !== target) throw new Error('Simulator failed to advance to requested frame');
    }
    const chunked = snapshot();
    const chunkDigest = sha256(JSON.stringify(chunked));
    if (directDigest !== chunkDigest) {
      record.chunk_differences = {
        direct_players: direct.players, chunked_players: chunked.players,
        units: direct.units.filter((unit, i) => JSON.stringify(unit) !== JSON.stringify(chunked.units[i]))
          .slice(0, 5).map(unit => ({ direct: unit, chunked: chunked.units.find(other => other.index === unit.index) })),
        direct_unit_count: direct.units.length, chunked_unit_count: chunked.units.length,
      };
      throw new Error('Fresh-instance snapshots differ between direct and 24-frame stepping');
    }
    const early = snapshot();
    while (sim.currentFrame() < header.frameCount) {
      const target = Math.min(header.frameCount, sim.currentFrame() + 240);
      sim.step(target - sim.currentFrame());
      if (sim.currentFrame() !== target) throw new Error('Simulator stopped before replay end');
    }
    const final = snapshot();
    const summarize = state => ({ frame: state.frame,
      players: players.map(player => ({ slot: player.slot, race: player.race,
        economy: state.players[player.slot],
        own_units: state.units.filter(unit => unit.owner === player.slot).length,
        completed_workers: state.players[player.slot]?.completedWorkers })),
      total_units: state.units.length });
    Object.assign(record, { status: 'playback_probe_passed', end_frame: header.frameCount,
      map: header.mapName, unit_id_namespace: sim.replayUnitIdNamespace(),
      chunk_consistency_frame: pilotFrame, chunk_consistency_sha256: directDigest,
      initial: summarize(initial), early: summarize(early), final: summarize(final),
      full_state_final_sha256: sha256(JSON.stringify(final)) });
  } catch (error) {
    Object.assign(record, { status: 'failed', error: String(error) });
  }
  record.elapsed_seconds = (performance.now() - started) / 1000;
  records.push(record);
  console.log(`${record.status}: ${arg} (${record.elapsed_seconds.toFixed(2)}s)${record.error ? `: ${record.error}` : ''}`);
}
const result = { probe_version: 1, timestamp: new Date().toISOString(), node: process.versions.node,
  backend_package: provenance.package, provenance_sha256: sha256(provenanceBytes),
  wrapper_sha256: sha256(wrapperBytes), artifacts, records,
  training_ready: false, playback_fidelity_validated: false,
  blockers: ['No validated per-player fog-of-war/detection observation interface',
    'Cumulative gathered resources required by macro-v1 are not exposed',
    'Research/upgrade state and pre-action acceptance labels need adapters and audits',
    'Chunk determinism and reaching EOF do not establish fidelity to the original game'],
};
await writeFile(resolve(output), `${JSON.stringify(result, null, 2)}\n`, { flag: 'wx' });
if (records.some(record => record.status === 'failed')) process.exitCode = 1;
