import unittest

from tests.test_whole_game_batch import row
from training.whole_game_encoding_cache import ObservationCache
from training.whole_game_features import static_grid


class ObservationCacheTests(unittest.TestCase):
    def test_identity_and_bounded_eviction(self):
        terrain = static_grid(dict(schema="protodd-terrain-v2", width_walktiles=8,
                                   height_walktiles=8, walkability="1" * 64))
        first, second = row("cadence", 1), row("cadence", 2)
        cache = ObservationCache(max_bytes=1200)
        initial = cache.get(first, terrain)
        self.assertIs(initial, cache.get(first, terrain))
        self.assertEqual(cache.hits, 1)
        cache.get(second, terrain)
        self.assertLessEqual(cache.bytes, cache.max_bytes)
        self.assertGreaterEqual(cache.evictions, 1)
        self.assertIsNot(initial, cache.get(first, terrain))


if __name__ == "__main__":
    unittest.main()
