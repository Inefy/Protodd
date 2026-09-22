"""Bounded-memory streaming of prepared samples, with training-only shuffling."""
import json
from contextlib import closing
from pathlib import Path
import random
import sqlite3

import numpy as np
import torch

from .schema import load_schema


def connect(path):
    return sqlite3.connect(Path(path).resolve().as_uri() + "?mode=ro", uri=True)


def metadata(path):
    with closing(connect(path)) as db:
        result = {key: json.loads(value) for key, value in db.execute("SELECT key,value FROM metadata")}
    if result.get("format_version") != 1 or result.get("schema") != load_schema():
        raise ValueError("prepared dataset schema mismatch")
    return result


def samples(path, split, shuffle_seed=None, buffer_size=8192):
    if split not in ("train", "validation", "test"):
        raise ValueError("unknown data split")
    if shuffle_seed is not None and split != "train":
        raise ValueError("only training data can be shuffled")
    rng = random.Random(shuffle_seed)
    buffer = []
    db = connect(path)
    try:
        rows = db.execute("""SELECT s.features,s.mask,s.action,s.weight / g.sample_count,g.matchup,s.game_id
            FROM samples s JOIN games g USING(game_id) WHERE g.split=?
            ORDER BY s.game_id,s.perspective,s.frame""", (split,))
        for row in rows:
            if shuffle_seed is None:
                yield row
            else:
                buffer.append(row)
                if len(buffer) >= buffer_size:
                    yield buffer.pop(rng.randrange(len(buffer)))
        rng.shuffle(buffer)
        yield from buffer
    finally:
        db.close()


def batches(path, split, batch_size, device="cpu", shuffle_seed=None):
    if batch_size < 1:
        raise ValueError("batch_size must be positive")
    schema = load_schema()
    action_count = len(schema["actions"])
    def pack(rows):
        features = np.stack([np.frombuffer(r[0], dtype="<f4") for r in rows])
        masks = [[bool(r[1] & (1 << i)) for i in range(action_count)] for r in rows]
        return (torch.tensor(features, dtype=torch.float32, device=device),
                torch.tensor(masks, dtype=torch.bool, device=device),
                torch.tensor([r[2] for r in rows], dtype=torch.long, device=device),
                torch.tensor([r[3] for r in rows], dtype=torch.float32, device=device),
                [r[4] for r in rows], [r[5] for r in rows])
    batch = []
    for row in samples(path, split, shuffle_seed):
        batch.append(row)
        if len(batch) == batch_size:
            yield pack(batch)
            batch.clear()
    if batch:
        yield pack(batch)
