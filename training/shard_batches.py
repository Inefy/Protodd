"""Bounded, deterministic batches for GPU imitation; final test is inaccessible."""
from collections import defaultdict, deque
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import torch

from .shards import positive_integer


class BalancedRows:
    """Uniform matchup -> game -> perspective -> row, sampled with replacement.

    A batch is a pure function of (seed, batch index). Acknowledgment happens only
    after an optimizer update, so speculative prefetch cannot change resume order.
    Raw shard weights already contain confidence/quality; do not weight by game
    length again after using this sampler.
    """

    def __init__(self, dataset, batch_size, seed=42):
        self.dataset = dataset
        self.config = dict(dataset_sha256=dataset.identity,
                           batch_size=positive_integer(batch_size, "batch size"),
                           seed=positive_integer(seed, "seed", 0))
        self.consumed = 0
        groups = defaultdict(lambda: defaultdict(list))
        for sequence in dataset.sequences:
            if sequence["split"] == "train":
                groups[sequence["matchup"]][sequence["game_index"]].append(sequence)
        self.groups = []
        for matchup, games in sorted(groups.items()):
            starts = np.zeros((len(games), 2), dtype=np.int64)
            counts = np.ones((len(games), 2), dtype=np.int64)
            perspectives = np.empty(len(games), dtype=np.int64)
            for i, (_, sequences) in enumerate(sorted(games.items())):
                perspectives[i] = len(sequences)
                for j, sequence in enumerate(sorted(sequences, key=lambda s: s["perspective"])):
                    starts[i, j], counts[i, j] = sequence["start"], sequence["count"]
            self.groups.append((matchup, starts, counts, perspectives))
        if not self.groups:
            raise ValueError("no training rows")

    def indices_at(self, index):
        positive_integer(index, "batch index", 0)
        rng = np.random.default_rng(np.random.SeedSequence([self.config["seed"], index]))
        matchups = rng.integers(len(self.groups), size=self.config["batch_size"])
        result = np.empty(self.config["batch_size"], dtype=np.int64)
        for m, (_, starts, counts, perspectives) in enumerate(self.groups):
            positions = np.flatnonzero(matchups == m)
            games = rng.integers(len(starts), size=len(positions))
            players = rng.integers(perspectives[games])
            result[positions] = starts[games, players] + rng.integers(counts[games, players])
        return result

    def acknowledge(self, index):
        if type(index) is not int or index != self.consumed:
            raise ValueError("batches must be acknowledged in consumed order")
        self.consumed += 1

    def state_dict(self):
        return dict(format="balanced-row-batches-v1", **self.config, consumed=self.consumed)

    def load_state_dict(self, state):
        if {k: v for k, v in state.items() if k != "consumed"} != {
                "format": "balanced-row-batches-v1", **self.config}:
            raise ValueError("sampler configuration or dataset mismatch")
        self.consumed = positive_integer(state.get("consumed"), "consumed batches", 0)


def tensor_batch(rows, action_count, pin=False):
    masks = (rows["masks"][:, None] >> np.arange(action_count, dtype=np.uint64)) & 1
    labels = rows["labels"].astype(np.int64)
    weights = rows["weights"].astype(np.float32)
    if (not np.isfinite(rows["features"]).all() or not np.isfinite(weights).all() or
            np.any(weights <= 0) or np.any(labels >= action_count) or
            not masks[np.arange(len(labels)), labels].all()):
        raise ValueError("nonfinite features/weights or illegal training label")
    arrays = (rows["features"], masks.astype(bool), labels, weights,
              rows["game_indices"].astype(np.int64))
    tensors = tuple(torch.from_numpy(np.ascontiguousarray(a)) for a in arrays)
    return tuple(t.pin_memory() for t in tensors) if pin else tensors


class CudaTrainingCache:
    """Keep training inputs on the GPU at the existing autocast input precision.

    BF16 autocast already converts first-layer inputs to BF16. Storing that same
    conversion once preserves the model's inputs exactly, while validation still
    reads original FP32 tensors. Labels, confidence weights and masks are lossless.
    The CPU holds only an index map and bounded staging batches, not a full copy.
    """

    @staticmethod
    def required_bytes(dataset):
        count = sum(s["count"] for s in dataset.sequences if s["split"] == "train")
        return count * (2 * len(dataset.manifest["schema"]["features"]) + 8 + 8 + 4 + 8)

    def __init__(self, dataset, progress=None, staging_rows=32768):
        indices = np.concatenate([np.arange(s["start"], s["start"] + s["count"], dtype=np.int64)
                                  for s in dataset.sequences if s["split"] == "train"])
        self.row_map = np.full(dataset.row_count, -1, dtype=np.int64)
        self.row_map[indices] = np.arange(len(indices), dtype=np.int64)
        width = len(dataset.manifest["schema"]["features"])
        self.action_count = len(dataset.manifest["schema"]["actions"])
        self.features = torch.empty((len(indices), width), dtype=torch.bfloat16, device="cuda")
        self.packed_masks = torch.empty(len(indices), dtype=torch.int64, device="cuda")
        self.labels = torch.empty(len(indices), dtype=torch.int64, device="cuda")
        self.weights = torch.empty(len(indices), dtype=torch.float32, device="cuda")
        self.games = torch.empty(len(indices), dtype=torch.int64, device="cuda")
        self.bits = torch.arange(self.action_count, device="cuda")
        for first in range(0, len(indices), staging_rows):
            end = min(first + staging_rows, len(indices))
            rows = dataset.read_indices(indices[first:end])
            batch = tensor_batch(rows, self.action_count, pin=True)
            self.features[first:end].copy_(batch[0], non_blocking=True)
            self.packed_masks[first:end].copy_(torch.from_numpy(rows["masks"].view(np.int64)).pin_memory(), non_blocking=True)
            self.labels[first:end].copy_(batch[2], non_blocking=True)
            self.weights[first:end].copy_(batch[3], non_blocking=True)
            self.games[first:end].copy_(batch[4], non_blocking=True)
            # Bound staging lifetime and memory even if disk outruns CUDA.
            torch.cuda.synchronize()
            if progress:
                progress(end, len(indices))

    def batch(self, indices):
        local = self.row_map[indices]
        if (local < 0).any():
            raise ValueError("GPU cache permits only training rows")
        selected = torch.from_numpy(local).to("cuda")
        masks = ((self.packed_masks[selected, None] >> self.bits) & 1).bool()
        return self.features[selected], masks, self.labels[selected], self.weights[selected], self.games[selected]


def prefetched(indices, read, depth=2):
    """A single bounded reader overlaps disk/CPU preparation with CUDA work."""
    positive_integer(depth, "prefetch depth")
    iterator = iter(indices)
    with ThreadPoolExecutor(max_workers=1, thread_name_prefix="shard-prefetch") as pool:
        pending = deque()
        for _ in range(depth):
            item = next(iterator, None)
            if item is not None:
                pending.append((item, pool.submit(read, item)))
        try:
            while pending:
                item, future = pending.popleft()
                value = future.result()
                following = next(iterator, None)
                if following is not None:
                    pending.append((following, pool.submit(read, following)))
                yield item, value
        finally:
            for _, future in pending:
                future.cancel()
