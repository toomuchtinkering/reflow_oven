import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildAligned, fmtElapsed } from '../../src/web/align.js';

/* buildAligned turns N series with independent, jittered time bases into the
 * single shared x array uPlot requires (spec §1). The invariant under test
 * throughout: a series' column carries its own value at its own timestamps and
 * null everywhere else — nothing is interpolated, resampled or invented. */

const pts = (...pairs) => pairs.map(([t_s, value]) => ({ t_s, value }));

test('single series round-trips unchanged', () => {
    const { data, keys } = buildAligned([{ key: 'a', points: pts([0, 10], [1, 20], [2, 30]) }]);
    assert.deepEqual(keys, ['a']);
    assert.deepEqual(data, [[0, 1, 2], [10, 20, 30]]);
});

test('disjoint jittered timestamps union into one sorted x axis', () => {
    // The real case: two runs polled at ~1 Hz never share a timestamp.
    const { data } = buildAligned([
        { key: 'a', points: pts([0.98, 10], [2.01, 11]) },
        { key: 'b', points: pts([1.02, 20], [2.05, 21]) },
    ]);
    assert.deepEqual(data[0], [0.98, 1.02, 2.01, 2.05]);
    assert.deepEqual(data[1], [10, null, 11, null]);
    assert.deepEqual(data[2], [null, 20, null, 21]);
});

test('x axis is sorted even when series arrive out of order', () => {
    const { data } = buildAligned([
        { key: 'late',  points: pts([5, 1], [6, 2]) },
        { key: 'early', points: pts([0, 3], [1, 4]) },
    ]);
    assert.deepEqual(data[0], [0, 1, 5, 6]);
});

test('duplicate timestamps across series collapse to one x entry', () => {
    const { data } = buildAligned([
        { key: 'a', points: pts([0, 1], [1, 2]) },
        { key: 'b', points: pts([0, 3], [1, 4]) },
    ]);
    assert.deepEqual(data[0], [0, 1], 'shared timestamps must not duplicate the x axis');
    assert.deepEqual(data[1], [1, 2]);
    assert.deepEqual(data[2], [3, 4]);
});

test('a shorter series gets trailing nulls, never fabricated values', () => {
    // Trailing nulls are what stop spanGaps drawing past the end of a run.
    const { data } = buildAligned([
        { key: 'long',  points: pts([0, 1], [1, 2], [2, 3], [3, 4]) },
        { key: 'short', points: pts([0, 9], [1, 9]) },
    ]);
    assert.deepEqual(data[2], [9, 9, null, null]);
});

test('a series starting late gets leading nulls', () => {
    const { data } = buildAligned([
        { key: 'a', points: pts([0, 1], [1, 2], [2, 3]) },
        { key: 'b', points: pts([2, 7]) },
    ]);
    assert.deepEqual(data[2], [null, null, 7]);
});

test('empty input yields an empty x axis and no series columns', () => {
    const { data, keys } = buildAligned([]);
    assert.deepEqual(keys, []);
    assert.deepEqual(data, [[]]);
});

test('a series with no points still occupies its column, all null', () => {
    // Ticking on a run whose samples failed to load must not shift the columns
    // of the series after it, or every curve would take the wrong colour.
    const { data, keys } = buildAligned([
        { key: 'a',     points: pts([0, 1], [1, 2]) },
        { key: 'empty', points: [] },
        { key: 'c',     points: pts([0, 5], [1, 6]) },
    ]);
    assert.deepEqual(keys, ['a', 'empty', 'c']);
    assert.deepEqual(data[2], [null, null]);
    assert.deepEqual(data[3], [5, 6]);
});

test('non-finite values are dropped rather than plotted', () => {
    // A dropped sample or a JSON null must leave a gap, not a spike to zero.
    const { data } = buildAligned([
        { key: 'a', points: [{ t_s: 0, value: 1 }, { t_s: 1, value: null },
                             { t_s: 2, value: NaN }, { t_s: 3, value: 4 }] },
    ]);
    assert.deepEqual(data[0], [0, 1, 2, 3]);
    assert.deepEqual(data[1], [1, null, null, 4]);
});

test('samples with a non-finite timestamp are skipped entirely', () => {
    const { data } = buildAligned([
        { key: 'a', points: [{ t_s: 0, value: 1 }, { t_s: NaN, value: 2 }, { t_s: 1, value: 3 }] },
    ]);
    assert.deepEqual(data[0], [0, 1]);
    assert.deepEqual(data[1], [1, 3]);
});

/* The x-axis formatter. The defect this replaces: niceTimeStep capped at
 * 1200 s, so a 12 h dry drew 37 labels reading "720:00" (spec §2.2). */

test('fmtElapsed uses seconds below 90 s', () => {
    assert.equal(fmtElapsed(0, 60), '0s');
    assert.equal(fmtElapsed(45, 60), '45s');
});

test('fmtElapsed switches to m:ss at the 90 s boundary', () => {
    assert.equal(fmtElapsed(89, 89), '89s');
    assert.equal(fmtElapsed(90, 90), '1:30');
    assert.equal(fmtElapsed(252, 300), '4:12');
});

test('fmtElapsed switches to h:mm at the 2 h boundary', () => {
    const twoH = 7200;
    assert.equal(fmtElapsed(3600, twoH - 1), '60:00');
    assert.equal(fmtElapsed(3600, twoH), '1:00');
    assert.equal(fmtElapsed(27600, 43200), '7:40');
});

test('fmtElapsed keeps a 12 h dry readable', () => {
    // The whole point: 12 h reads as 12:00 hours, not 720:00 minutes.
    assert.equal(fmtElapsed(43200, 43200), '12:00');
});

test('fmtElapsed pads minutes and seconds to two digits', () => {
    assert.equal(fmtElapsed(125, 300), '2:05');
    assert.equal(fmtElapsed(3660, 7200), '1:01');
});
