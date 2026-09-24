"""Bounded CPU feature cache for repeated causal replay observations.

Rows and static terrain tensors are immutable for a fit. Cache keys retain the
actual objects, so Python cannot recycle an identity while its entry is live.
The byte budget counts encoded tensors and provides deterministic LRU eviction.
"""
from __future__ import annotations

from collections import OrderedDict

from .whole_game_features import encode_observation


class ObservationCache:
    def __init__(self, max_bytes=512 * 1024 * 1024):
        if max_bytes < 1:
            raise ValueError("cache budget must be positive")
        self.max_bytes = max_bytes
        self.bytes = 0
        self.hits = 0
        self.misses = 0
        self.evictions = 0
        self._entries = OrderedDict()

    def get(self, row, terrain):
        key = (id(row), id(terrain))
        cached = self._entries.get(key)
        if cached is not None and cached[0] is row and cached[1] is terrain:
            self._entries.move_to_end(key)
            self.hits += 1
            return cached[2]
        self.misses += 1
        encoded = encode_observation(row, terrain)
        tensors, ids, _ = encoded
        size = sum(value.numel() * value.element_size() for value in tensors.values()) + len(ids) * 16
        if size <= self.max_bytes:
            while self.bytes + size > self.max_bytes:
                _, old = self._entries.popitem(last=False)
                self.bytes -= old[3]
                self.evictions += 1
            self._entries[key] = (row, terrain, encoded, size)
            self.bytes += size
        return encoded

    def stats(self):
        return dict(hits=self.hits, misses=self.misses, evictions=self.evictions,
                    entries=len(self._entries), bytes=self.bytes,
                    max_bytes=self.max_bytes)
