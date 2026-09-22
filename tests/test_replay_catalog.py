"""Engine and BWAPI mappings must refer to the same unit and tech names."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CatalogParity(unittest.TestCase):
    def test_unit_mapping_matches_live_bridge(self):
        source = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text()
        source = source[source.index("UnitKind BwapiBridge::toKind("):source.index("BWAPI::UnitType BwapiBridge::toBwapi(")]
        adapter = (ROOT / "tools/replay_native/catalog.hpp").read_text()
        adapter = adapter[:adapter.index("bwgame::TechTypes replayTech(")]
        pattern = r"if \(type == .*?return UnitKind::\w+;"
        normalize = lambda text: re.sub(r"\s+", " ", text)
        self.assertEqual([normalize(x) for x in re.findall(pattern, source, re.S)],
                         [normalize(x) for x in re.findall(pattern, adapter, re.S)])
    def test_technology_mapping_matches_live_bridge(self):
        source = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text()
        source = source[source.index("BWAPI::TechType BwapiBridge::toBwapiTech("):]
        adapter = (ROOT / "tools/replay_native/catalog.hpp").read_text()
        pattern = r"case TechnologyKind::(\w+): return (\w+);"
        self.assertEqual(re.findall(pattern, source), re.findall(pattern, adapter))


if __name__ == "__main__":
    unittest.main()
