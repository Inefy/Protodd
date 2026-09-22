import assert from 'node:assert/strict';
import test from 'node:test';
import { firstDifference } from '../training/compare_replay.mjs';

test('equal final resources do not hide a changed unit or production queue', () => {
  const native = { frame: 240, players: [{ minerals: 50, gas: 0 }],
    units: [{ index: 7, x: 120, hpRaw: 1024, queue: [64] }] };
  assert.equal(firstDifference(native, structuredClone(native)), null);
  for (const [key, value] of [['x', 121], ['hpRaw', 1023], ['queue', [65]]]) {
    const reference = structuredClone(native);
    reference.units[0][key] = value;
    assert.ok(firstDifference(native, reference)?.path.includes(key));
  }
  const reference = structuredClone(native);
  reference.units.push({ index: 8 });
  assert.equal(firstDifference(native, reference).path, '.units.length');
});

test('missing fields, frame differences and type differences fail comparison', () => {
  assert.ok(firstDifference({ frame: 240 }, { frame: 241 }));
  assert.ok(firstDifference({ frame: 240 }, { frame: '240' }));
  assert.ok(firstDifference({ frame: 240 }, {}));
  assert.ok(firstDifference({ a: 1 }, { b: 1 }));
});
