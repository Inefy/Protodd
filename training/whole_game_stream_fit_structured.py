"""Run the frozen-cohort GPU fitter with experimental structured action loss.

This entry point keeps the ongoing baseline run's source hashes and checkpoints
unchanged. Its own manifest records this file and every objective dependency.
"""
from __future__ import annotations

from . import whole_game_stream_fit as stream
from .whole_game_structured_batch import structured_minibatch_loss


stream.SCHEMA = "protodd-whole-game-stream-fit-structured-v1"
stream.SOURCE_FILES = (*stream.SOURCE_FILES,
                       "whole_game_action_schema.py", "whole_game_structured_loss.py",
                       "whole_game_structured_batch.py", "whole_game_stream_fit_structured.py")
stream.minibatch_loss = structured_minibatch_loss


if __name__ == "__main__":
    stream.main()
