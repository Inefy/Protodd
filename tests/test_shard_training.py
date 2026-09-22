"""Training correctness, exact interrupted resume, balancing and holdout isolation."""
from collections import Counter
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np
import torch

from test_tensor_shards import fixture, write_rows
from training.model import load_model
from training.pack_dataset import pack
from training.shard_batches import BalancedRows, CudaTrainingCache, prefetched, tensor_batch
from training.shards import TensorShards
from training.train_shards import acquire_lock, normalize_config, split_indices, train


class ShardTrainingTests(unittest.TestCase):
    def prepare(self, root, learnable=False):
        manifest, games, rows = fixture(root)
        if learnable:
            info = json.loads(manifest.read_text())
            for game in info["games"]:
                if game["split"] == "test":
                    continue
                for row in rows[game["game_id"]]:
                    row["features"][1] = 1.0 if row["action"] == "train_probe" else 0.0
                write_rows(games / game["replay_sha256"], game["game_id"], rows[game["game_id"]])
        with redirect_stdout(io.StringIO()):
            pack(manifest, games, Path(root) / "packed", shard_rows=5, allow_synthetic=True)
        return Path(root) / "packed"

    def config(self, root, dataset, name):
        return dict(dataset=str(dataset), output=str(Path(root) / name), device="cpu", precision="fp32",
                    hidden=[16, 16], epochs=2, steps_per_epoch=4, warmup_steps=2, batch_size=24,
                    validation_batch_size=3, checkpoint_steps=2, allow_synthetic=True, cpu_threads=1)

    def run_quiet(self, cfg, **kwargs):
        with redirect_stdout(io.StringIO()):
            return train(cfg, **kwargs)

    def test_gather_across_shards_preserves_order_and_duplicates(self):
        with tempfile.TemporaryDirectory() as root:
            with TensorShards(self.prepare(root)) as ds:
                expected = ds.read_rows(0, ds.row_count)
                indices = np.array([10, 0, 10, 49, 3, 28, 50, 1])
                for key, values in ds.read_indices(indices).items():
                    np.testing.assert_array_equal(values, expected[key][indices])
                for indices in ([-1], [ds.row_count], [1.5], []):
                    with self.assertRaises(ValueError):
                        ds.read_indices(indices)

    def test_balanced_draws_and_prefetch_do_not_advance_consumption(self):
        with tempfile.TemporaryDirectory() as root:
            with TensorShards(self.prepare(root)) as ds:
                sampler = BalancedRows(ds, batch_size=12000, seed=42)
                indices = sampler.indices_at(0)
                rows = ds.read_indices(indices)
                totals = Counter(rows["game_indices"].tolist())
                train_games = {s["game_index"] for s in ds.sequences if s["split"] == "train"}
                self.assertEqual(set(totals), train_games)
                for count in totals.values():
                    self.assertLess(abs(count / len(indices) - 1 / 3), 0.02)
                gid = next(s["game_index"] for s in ds.sequences if s["game_id"] == "a")
                self.assertLess(abs(rows["perspectives"][rows["game_indices"] == gid].mean() - .5), .025)
                first = list(prefetched(range(3), sampler.indices_at))
                self.assertEqual(sampler.consumed, 0)
                sampler.acknowledge(0)
                restored = BalancedRows(ds, 12000, 42)
                restored.load_state_dict(sampler.state_dict())
                np.testing.assert_array_equal(restored.indices_at(restored.consumed), first[1][1])
                with self.assertRaises(ValueError):
                    sampler.acknowledge(0)
                with self.assertRaises(ValueError):
                    BalancedRows(ds, 32, 42).load_state_dict(sampler.state_dict())
                with self.assertRaises(ValueError):
                    split_indices(ds, "test")

    def test_resume_matches_uninterrupted_weights_optimizer_and_metrics(self):
        with tempfile.TemporaryDirectory() as root:
            dataset = self.prepare(root)
            direct_cfg, resume_cfg = (self.config(root, dataset, name) for name in ("direct", "resumed"))
            direct = self.run_quiet(direct_cfg)
            interrupted = self.run_quiet(resume_cfg, stop_after_steps=3)
            self.assertEqual(interrupted["step"], 3)
            resumed = self.run_quiet(resume_cfg, resume=True)
            self.assertEqual(direct["validation"], resumed["validation"])
            self.assertEqual(direct["model_sha256"], resumed["model_sha256"])
            states = [torch.load(Path(cfg["output"]) / "latest.pt", weights_only=False) for cfg in (direct_cfg, resume_cfg)]
            self.assertEqual(states[0]["sampler"], states[1]["sampler"])
            self.assertEqual(states[0]["state"], states[1]["state"])
            self.assertEqual(states[0]["schedule"], states[1]["schedule"])
            for key in states[0]["model"]:
                torch.testing.assert_close(states[0]["model"][key], states[1]["model"][key], rtol=0, atol=0)
            for key, values in states[0]["optimizer"]["state"].items():
                for name, value in values.items():
                    torch.testing.assert_close(value, states[1]["optimizer"]["state"][key][name], rtol=0, atol=0)
            with self.assertRaisesRegex(ValueError, "mismatch"):
                self.run_quiet({**resume_cfg, "learning_rate": 0.001}, resume=True)

    def test_epoch_boundary_resume_and_export_recovery(self):
        with tempfile.TemporaryDirectory() as root:
            dataset = self.prepare(root)
            cfg = self.config(root, dataset, "run")
            self.run_quiet(cfg, stop_after_steps=4)
            result = self.run_quiet(cfg, resume=True)
            self.assertEqual(result["steps_completed"], 8)
            self.assertEqual(result["epochs_completed"], 2)
            (Path(cfg["output"]) / "LearnedMacro.bin").unlink()
            subprocess.run([sys.executable, str(Path(cfg["output"]) / "source" / "resume.py")],
                           check=True, capture_output=True, text=True, timeout=60)
            recovered = json.loads((Path(cfg["output"]) / "manifest.json").read_text())
            self.assertEqual(result["model_sha256"], recovered["model_sha256"])

    @unittest.skipUnless(torch.cuda.is_available() and torch.cuda.is_bf16_supported(), "CUDA BF16 unavailable")
    def test_cuda_bf16_resume_matches_uninterrupted_updates(self):
        with tempfile.TemporaryDirectory() as root:
            dataset = self.prepare(root)
            configs = [self.config(root, dataset, name) for name in ("gpu_direct", "gpu_resumed")]
            for cfg in configs:
                cfg.update(device="cuda", precision="bf16")
            direct = self.run_quiet(configs[0])
            self.run_quiet(configs[1], stop_after_steps=3)
            resumed = self.run_quiet(configs[1], resume=True)
            self.assertEqual(direct["model_sha256"], resumed["model_sha256"])
            self.assertEqual(direct["validation"], resumed["validation"])

    @unittest.skipUnless(torch.cuda.is_available() and torch.cuda.is_bf16_supported(), "CUDA BF16 unavailable")
    def test_cuda_cache_matches_autocast_inputs_and_training_exactly(self):
        with tempfile.TemporaryDirectory() as root:
            dataset = self.prepare(root)
            with TensorShards(dataset) as ds:
                cache = CudaTrainingCache(ds, staging_rows=3)
                indices = BalancedRows(ds, 24).indices_at(0)
                source = tensor_batch(ds.read_indices(indices), 46)
                cached = cache.batch(indices)
                torch.testing.assert_close(cached[0].cpu(), source[0].to(torch.bfloat16), rtol=0, atol=0)
                for expected, actual in zip(source[1:], cached[1:]):
                    torch.testing.assert_close(actual.cpu(), expected, rtol=0, atol=0)
                with self.assertRaises(ValueError):
                    cache.batch(split_indices(ds, "validation"))
            configs = [self.config(root, dataset, name) for name in ("stream", "cached")]
            configs[0].update(device="cuda", precision="bf16", data_cache="stream")
            configs[1].update(device="cuda", precision="bf16", data_cache="cuda")
            stream = self.run_quiet(configs[0])
            cached_result = self.run_quiet(configs[1])
            self.assertEqual(stream["model_sha256"], cached_result["model_sha256"])
            self.assertEqual(stream["validation"], cached_result["validation"])

    def test_model_learns_signal_and_export_preserves_predictions(self):
        with tempfile.TemporaryDirectory() as root:
            dataset = self.prepare(root, learnable=True)
            cfg = self.config(root, dataset, "learn")
            cfg.update(learning_rate=.01, epochs=2, steps_per_epoch=40)
            result = self.run_quiet(cfg)
            self.assertGreater(result["validation"]["all"]["top1"], .99)
            self.assertLess(result["validation"]["all"]["cross_entropy"], .15)
            self.assertFalse(result["final_test_evaluated"])
            self.assertFalse(result["strength_validated"])
            self.assertEqual(result["validation"]["all"]["samples"], 8)
            model = load_model(Path(cfg["output"]) / "LearnedMacro.bin")
            self.assertEqual(model.hidden, (16, 16))

    def test_reject_bad_rows_synthetic_default_and_duplicate_run(self):
        with tempfile.TemporaryDirectory() as root:
            dataset = self.prepare(root)
            with TensorShards(dataset) as ds:
                rows = ds.read_rows(0, 3)
                rows["masks"][:] = 0
                with self.assertRaises(ValueError):
                    tensor_batch(rows, 46)
            cfg = self.config(root, dataset, "no_synthetic")
            cfg["allow_synthetic"] = False
            with self.assertRaisesRegex(ValueError, "synthetic"):
                self.run_quiet(cfg)
            folder = Path(root) / "locked"
            folder.mkdir()
            lock = acquire_lock(folder)
            try:
                with self.assertRaises(RuntimeError):
                    acquire_lock(folder)
            finally:
                lock.close()
            with self.assertRaises(ValueError):
                normalize_config({**cfg, "precision": "bf16"})


if __name__ == "__main__":
    unittest.main()
