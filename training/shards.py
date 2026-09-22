"""Read immutable macro tensors and sample causal sequences by consumed index.

No PyTorch dependency. Sampling is random access: prefetching sample_at(i) never
changes resume state. Call acknowledge(i) only after that sample was consumed.
"""
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import random

import numpy as np

from .schema import load_schema, sha256

FORMAT = "protodd-tensor-shards-v1"
METADATA_DTYPE = np.dtype([
    ("masks", "<u8"), ("labels", "u1"), ("weights", "<f4"),
    ("frames", "<i4"), ("action_frames", "<i4"),
    ("perspectives", "u1"), ("game_indices", "<u4"),
    ("delta_frames", "<i4"),
])


def positive_integer(value, label, minimum=1):
    if type(value) is not int or value < minimum:
        raise ValueError(f"invalid {label}")
    return value


class TensorShards:
    """Validate and memory-map a completed train/validation tensor release."""

    def __init__(self, directory, verify_hashes=True, progress=None):
        self.directory = Path(directory)
        if self.directory.name.endswith(".partial"):
            raise ValueError("unfinished tensor release")
        path = self.directory / "manifest.json"
        self.manifest = json.loads(path.read_text(encoding="utf-8"))
        self.identity = sha256(path)
        info = self.manifest
        if (info.get("format") != FORMAT or info.get("complete") is not True or
                info.get("schema") != load_schema()):
            raise ValueError("incomplete or incompatible tensor release")
        if info.get("packed_splits") != ["train", "validation"]:
            raise ValueError("training shards may contain only train and validation")
        self.games = info["games"]
        self.sequences = info["sequences"]
        self.row_count = positive_integer(info["rows"], "row count")
        self._arrays = []
        self._starts = []
        position = 0
        try:
            for shard in info["shards"]:
                count = positive_integer(shard["rows"], "shard rows")
                if shard["start"] != position:
                    raise ValueError("noncontiguous shard ranges")
                arrays = {}
                # Register immediately so a later file/checksum error closes
                # already-open mappings, including on Windows.
                self._arrays.append(arrays)
                for name in ("features", "metadata"):
                    record = shard[name]
                    filename = record["file"]
                    if (not isinstance(filename, str) or Path(filename).name != filename or
                            "/" in filename or "\\" in filename or ":" in filename):
                        raise ValueError("invalid shard filename")
                    file = self.directory / filename
                    if verify_hashes and sha256(file) != record["sha256"]:
                        raise ValueError(f"shard checksum mismatch: {filename}")
                    arrays[name] = np.load(file, mmap_mode="r", allow_pickle=False)
                if (arrays["features"].dtype != np.dtype("<f4") or
                        arrays["features"].shape != (count, len(info["schema"]["features"])) or
                        arrays["metadata"].dtype != METADATA_DTYPE or
                        arrays["metadata"].shape != (count,)):
                    raise ValueError("shard dtype or shape mismatch")
                self._starts.append(position)
                position += count
                if progress is not None:
                    progress(position, self.row_count)
            if position != self.row_count:
                raise ValueError("shard row total mismatch")
            position = 0
            seen = set()
            for sequence in self.sequences:
                count = positive_integer(sequence["count"], "sequence count")
                game_index = positive_integer(sequence["game_index"], "game index", 0)
                if game_index >= len(self.games):
                    raise ValueError("unknown sequence game")
                game = self.games[game_index]
                perspective = sequence["perspective"]
                key = (game_index, perspective)
                if (sequence["start"] != position or perspective not in (0, 1) or
                        key in seen or sequence["game_id"] != game["game_id"] or
                        sequence["split"] != game["split"] or
                        sequence["split"] not in ("train", "validation") or
                        sequence["matchup"] != game["matchup"]):
                    raise ValueError("invalid sequence boundaries or identity")
                seen.add(key)
                position += count
            if position != self.row_count or not self.sequences:
                raise ValueError("sequence row total mismatch")
            if not all(any(s["split"] == split for s in self.sequences)
                       for split in ("train", "validation")):
                raise ValueError("train and validation sequences required")
        except Exception:
            self.close()
            raise

    def close(self):
        for arrays in self._arrays:
            for array in arrays.values():
                mapping = getattr(array, "_mmap", None)
                if mapping is not None:
                    mapping.close()
        self._arrays = []

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def read_rows(self, start, count):
        """Return owned contiguous arrays; requested rows may span shard files."""
        import bisect
        positive_integer(start, "row start", 0)
        positive_integer(count, "row count")
        if start + count > self.row_count or not self._arrays:
            raise ValueError("row range outside open dataset")
        features = np.empty((count, len(self.manifest["schema"]["features"])), dtype="<f4")
        metadata = np.empty(count, dtype=METADATA_DTYPE)
        done = 0
        while done < count:
            index = bisect.bisect_right(self._starts, start + done) - 1
            offset = start + done - self._starts[index]
            arrays = self._arrays[index]
            take = min(count - done, len(arrays["metadata"]) - offset)
            features[done:done + take] = arrays["features"][offset:offset + take]
            metadata[done:done + take] = arrays["metadata"][offset:offset + take]
            done += take
        return {"features": features, **{name: metadata[name].copy() for name in METADATA_DTYPE.names}}

    def metadata_blocks(self):
        """Read-only views for statistics, without faulting feature pages into RAM."""
        for start, arrays in zip(self._starts, self._arrays):
            yield start, arrays["metadata"]

    def read_indices(self, indices):
        """Gather a bounded batch in request order, grouping disk reads by shard."""
        indices = np.asarray(indices)
        if (indices.ndim != 1 or not len(indices) or indices.dtype.kind not in "iu" or
                indices.min() < 0 or indices.max() >= self.row_count or not self._arrays):
            raise ValueError("invalid row indices")
        order = np.argsort(indices, kind="stable")
        sorted_indices = indices[order]
        shards = np.searchsorted(self._starts, sorted_indices, side="right") - 1
        boundaries = np.r_[0, np.flatnonzero(np.diff(shards)) + 1, len(indices)]
        features = np.empty((len(indices), len(self.manifest["schema"]["features"])), dtype="<f4")
        metadata = np.empty(len(indices), dtype=METADATA_DTYPE)
        for first, end in zip(boundaries[:-1], boundaries[1:]):
            shard = int(shards[first])
            offsets = sorted_indices[first:end] - self._starts[shard]
            destinations = order[first:end]
            features[destinations] = self._arrays[shard]["features"][offsets]
            metadata[destinations] = self._arrays[shard]["metadata"][offsets]
        return {"features": features, **{name: metadata[name].copy() for name in METADATA_DTYPE.names}}

    def sequence(self, index, start, length, burn_in=0):
        """Read one causal window, with past-only burn-in and explicit reset/gaps.

        start is the first supervised offset inside the game/perspective. The
        returned window can be shorter at either boundary; no padding is invented.
        reset_mask starts a fresh sampled recurrent state; is_game_start records
        whether this is also the actual start of that player's recorded history.
        """
        positive_integer(index, "sequence index", 0)
        positive_integer(start, "sequence start", 0)
        positive_integer(length, "sequence length")
        positive_integer(burn_in, "burn in", 0)
        if index >= len(self.sequences):
            raise ValueError("unknown sequence")
        info = self.sequences[index]
        if start >= info["count"]:
            raise ValueError("sequence start outside perspective")
        first = max(0, start - burn_in)
        end = min(info["count"], start + length)
        result = self.read_rows(info["start"] + first, end - first)
        if (not np.all(result["game_indices"] == info["game_index"]) or
                not np.all(result["perspectives"] == info["perspective"]) or
                (len(result["frames"]) > 1 and not np.all(np.diff(result["frames"]) > 0))):
            raise ValueError("corrupt sequence identity or chronology")
        result["loss_mask"] = np.arange(end - first) >= start - first
        result["reset_mask"] = np.arange(end - first) == 0
        result["gap_mask"] = result["delta_frames"] > 24
        result.update(sequence_index=index, game_id=info["game_id"],
                      perspective=info["perspective"], split=info["split"],
                      matchup=info["matchup"], window_start=first, target_start=start,
                      burn_in=start - first, is_game_start=first == 0)
        return result


class SequenceSampler:
    """Uniform matchup (optional), game, perspective, then target-window start.

    Weights in the shard remain raw confidence * quality; do not apply inverse
    game length again to this game-balanced sampling distribution.
    """

    def __init__(self, dataset, seed=42, sequence_length=32, burn_in=0,
                 balance_matchups=True):
        self.dataset = dataset
        positive_integer(seed, "seed", 0)
        positive_integer(sequence_length, "sequence length")
        positive_integer(burn_in, "burn in", 0)
        if type(balance_matchups) is not bool:
            raise ValueError("balance_matchups must be boolean")
        self.config = dict(dataset_sha256=dataset.identity, seed=seed,
                           sequence_length=sequence_length, burn_in=burn_in,
                           balance_matchups=balance_matchups)
        self.consumed = 0
        groups = defaultdict(lambda: defaultdict(list))
        for index, sequence in enumerate(dataset.sequences):
            if sequence["split"] == "train":
                groups[sequence["matchup"]][sequence["game_index"]].append(index)
        self.groups = {m: dict(games) for m, games in sorted(groups.items())}
        self.matchups = list(self.groups)
        self.game_sequences = {g: seqs for group in self.groups.values() for g, seqs in group.items()}
        if not self.game_sequences:
            raise ValueError("no training sequences")

    def sample_at(self, index):
        positive_integer(index, "sample index", 0)
        entropy = hashlib.sha256(f"{self.config['seed']}:{index}".encode("ascii")).digest()
        rng = random.Random(int.from_bytes(entropy, "little"))
        if self.config["balance_matchups"]:
            group = self.groups[rng.choice(self.matchups)]
        else:
            group = self.game_sequences
        game = rng.choice(sorted(group))
        sequence_index = rng.choice(group[game])
        start = rng.randrange(self.dataset.sequences[sequence_index]["count"])
        result = self.dataset.sequence(sequence_index, start, self.config["sequence_length"],
                                       self.config["burn_in"])
        result["sample_index"] = index
        return result

    def acknowledge(self, index):
        """Commit consumption in order, never at speculative/prefetch time."""
        if type(index) is not int or index != self.consumed:
            raise ValueError("samples must be acknowledged exactly once in consumed order")
        self.consumed += 1

    def state_dict(self):
        return {"format": "consumed-sequence-sampler-v1", **self.config,
                "consumed_samples": self.consumed}

    def load_state_dict(self, state):
        expected = {"format": "consumed-sequence-sampler-v1", **self.config}
        if {k: v for k, v in state.items() if k != "consumed_samples"} != expected:
            raise ValueError("sampler checkpoint configuration or dataset mismatch")
        self.consumed = positive_integer(state.get("consumed_samples"), "consumed samples", 0)
