import re
import unittest
from pathlib import Path

from training.whole_game_action_schema import KIND_TARGET_MODES
from training.whole_game_model import DOMAINS, KINDS, TARGET_MODES


class WholeGameIntentSchemaTests(unittest.TestCase):
    def test_compiled_action_order_matches_exported_heads(self):
        source = (Path(__file__).resolve().parents[1] / "src/cpu/WholeGameIntent.hpp").read_text()
        for symbol, expected in (("kindNames", KINDS), ("domainNames", DOMAINS),
                                 ("targetModeNames", TARGET_MODES)):
            body = re.search(rf"{symbol} = std::to_array<std::string_view>\(\{{(.*?)\}}\);",
                             source, re.DOTALL)
            self.assertIsNotNone(body)
            self.assertEqual(tuple(re.findall(r'"([^"]+)"', body.group(1))), expected)
        masks = re.search(r"kindTargetModeMask = std::to_array<std::uint8_t>\(\{(.*?)\}\);",
                          source, re.DOTALL)
        self.assertIsNotNone(masks)
        actual = tuple(map(int, re.findall(r"\b\d+\b", masks.group(1))))
        expected = tuple(sum(1 << index for index, mode in enumerate(TARGET_MODES)
                             if mode in KIND_TARGET_MODES.get(kind, ())) for kind in KINDS)
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
