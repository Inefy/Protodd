"""GPU cadence-action focal ablation on the conditional whole-game teacher."""
from __future__ import annotations

from . import whole_game_stream_fit_conditional  # Installs cadence and natural schedules.
from . import whole_game_stream_fit as stream
from . import whole_game_structured_batch as structured_batch
from .whole_game_focal_loss import focal_structured_action_loss


stream.SCHEMA = "protodd-whole-game-stream-fit-conditional-focal-v1"
stream.SOURCE_FILES = (*stream.SOURCE_FILES, "whole_game_focal_loss.py",
                       "whole_game_stream_fit_focal.py")
structured_batch.structured_action_loss = focal_structured_action_loss


if __name__ == "__main__":
    stream.main()
