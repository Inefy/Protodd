import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.validate_parallel import checked_record, execute_tasks, pending_tasks, pinned_options
from training.schema import sha256


class ParallelValidationTests(unittest.TestCase):
    def test_resume_preserves_all_prior_results_and_rejects_changed_corpus(self):
        with sqlite3.connect(":memory:") as db:
            db.execute("CREATE TABLE results(path TEXT PRIMARY KEY, sha256 TEXT)")
            db.execute("INSERT INTO results VALUES('done','digest')")
            self.assertEqual(pending_tasks(db, [("done", "digest"), ("new", "new-digest")]), [("new", "new-digest")])
            self.assertEqual(pending_tasks(db, [("done", "digest")]), [])
            with self.assertRaisesRegex(ValueError, "identity changed"):
                pending_tasks(db, [("done", "other")])
            with self.assertRaisesRegex(ValueError, "outside"):
                pending_tasks(db, [])

    def test_parallel_workers_keep_full_results_and_drain_timeout_before_retry(self):
        barrier = threading.Barrier(4)
        lock = threading.Lock()
        calls = []
        completed = []
        options = SimpleNamespace(root=Path("root"), interval=240, timeout=300)

        def checker(path, args):
            with lock:
                calls.append(path.name)
                attempt = calls.count(path.name)
            self.assertEqual(args.interval, 240)
            self.assertEqual(args.timeout, 300)
            if attempt == 1:
                barrier.wait(timeout=5)
            if path.name == "0" and attempt == 1:
                return {"status": "quarantined", "error": "Command timed out after 300 seconds"}
            return {"status": "quarantined" if path.name == "1" else "checkpoints_matched",
                    "sha256": path.name, "native_checkpoints_sha256": "unchanged:" + path.name}

        execute_tasks([(str(i), str(i)) for i in range(4)], options, 4,
                      lambda p, h, r: completed.append((p, h, r)), lambda: None, checker)
        self.assertEqual(len(completed), 4)
        self.assertEqual(calls.count("0"), 2)
        self.assertEqual(completed[-1][0], "0")
        self.assertIn("parallel_timeout_retry", completed[-1][2])
        for path, digest, record in completed:
            self.assertEqual(record["native_checkpoints_sha256"], "unchanged:" + digest)
            if path == "1":
                self.assertEqual(record["status"], "quarantined")

    def test_changed_replay_never_passes(self):
        result = checked_record({"sha256": "changed", "status": "checkpoints_matched"}, "original")
        self.assertEqual(result["status"], "quarantined")
        with self.assertRaises(ValueError):
            checked_record({"sha256": "same", "status": "unexpected"}, "same")

    def test_resume_requires_original_config_and_checker_hash(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            config = {name: str(root) for name in
                      ("node", "backend", "native", "decoder", "mpq", "assets", "output", "audit", "root")}
            config.update(workers=4, interval=240, timeout=300)
            config_path = root / "config.json"
            config_path.write_text(json.dumps(config))
            checker = root / "checker.py"
            checker.write_text("original")
            identity = {"config": config, "terrain": {}, "hashes": {str(checker): sha256(checker)}}
            (root / "identity.json").write_text(json.dumps(identity))
            with patch("tools.validate_parallel.verify", return_value={}):
                options, _ = pinned_options(config_path)
                self.assertEqual(options.interval, 240)
                checker.write_text("changed")
                with self.assertRaisesRegex(ValueError, "input changed"):
                    pinned_options(config_path)
                config["interval"] = 2400
                config_path.write_text(json.dumps(config))
                with self.assertRaisesRegex(ValueError, "config changed"):
                    pinned_options(config_path)


if __name__ == "__main__":
    unittest.main()
