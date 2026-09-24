"""Offline rolling-history inference adapter; grants no BWAPI control.

Caches only the model's observation encoding under fixed weights. Rebuilds GRU
memory from zero over the same eight preceding cadence rows used by the trainer.
No labels or teacher command tokens are accepted by this interface.
"""
from collections import deque

import torch


class RollingGroupInference:
    def __init__(self, model, history=8):
        if history < 1 or model.training:
            raise ValueError('positive history and evaluation-mode model required')
        self.model, self.history = model, deque(maxlen=history)
        self.game_id, self.frame, self.last_output = None, None, None
        self.versions = tuple(p._version for p in model.parameters())

    def reset(self):
        self.history.clear()
        self.game_id, self.frame, self.last_output = None, None, None

    @torch.no_grad()
    def step(self, game_id, frame, batch):
        if not isinstance(game_id,str) or not game_id or not isinstance(frame,int) or frame < 0:
            raise ValueError('invalid game/frame identity')
        if self.model.training or tuple(p._version for p in self.model.parameters()) != self.versions:
            self.reset()
            raise ValueError('model changed while encoded history was cached')
        if game_id != self.game_id or (self.frame is not None and frame < self.frame):
            self.reset()
        if frame == self.frame:
            return self.last_output
        observation, entities = self.model.encode_step(batch)
        if observation.shape[0] != 1:
            raise ValueError('one live perspective per inference adapter')
        memory = torch.zeros_like(observation)
        for previous in self.history:
            memory = self.model.memory(previous,memory)
        memory = self.model.memory(observation,memory)
        output = self.model.forward_slots(batch,encoded_base=dict(memory=memory,entities=entities))
        self.history.append(observation.detach())
        self.game_id, self.frame, self.last_output = game_id,frame,output
        return output
