"""Causal, ordered multi-command targets at deployable cadence observations.

Inputs are the current cadence row and earlier cadence rows. Commands in the
following 24 frames are training targets only. The final censored window is
never emitted, and command order is retained for a future slot decoder.
"""
from __future__ import annotations

from collections import deque


def cadence_sequences(trajectory, *, history=8, window=24):
    if history < 1 or window < 1:
        raise ValueError("positive history and window required")
    recent = deque(maxlen=history)
    pending = None

    def finish():
        if pending is None or pending["event"] is None:
            return None
        if bool(pending["event"]) != bool(pending["labels"]):
            raise ValueError("cadence event and following commands disagree")
        current = pending["observation"]
        own_ids = {entity["id"] for entity in current["entities"]
                   if entity["relation"] == 0}
        visible_ids = {entity["id"] for entity in current["entities"]
                       if entity["visible"]}
        actor_available = []
        target_available = []
        for label in pending["labels"]:
            selected = set(label["actor_positive"])
            actor_available.append(bool(selected) and selected <= own_ids)
            target = label["actions"]["target_entity"]
            target_available.append(
                not label["loss_masks"]["target_entity"] or target in visible_ids)
        return dict(context=pending["context"], observation=current,
                    event=pending["event"], labels=pending["labels"],
                    actor_available=actor_available,
                    target_available=target_available)

    for row, target in trajectory:
        if target["update_memory"]:
            completed = finish()
            if completed is not None:
                yield completed
            pending = dict(context=list(recent), observation=row,
                           event=target["event"], labels=[])
            recent.append(row)
        elif (pending is not None and target["action"] is not None and
              pending["observation"]["frame"] <= row["frame"] <
              pending["observation"]["frame"] + window):
            pending["labels"].append(target["action"])
    completed = finish()
    if completed is not None:
        yield completed


def slot_targets(sequence, *, maximum_commands=6, window=24):
    """Make chronological teacher-forcing slots and an explicit STOP target.

    A full six-command window has no STOP supervision; a longer window records
    its overflow instead of silently treating the sixth command as terminal.
    """
    if maximum_commands < 1 or window < 1:
        raise ValueError("positive slot and window limits required")
    labels = sequence["labels"]
    if (len(sequence["actor_available"]) != len(labels) or
            len(sequence["target_available"]) != len(labels)):
        raise ValueError("availability masks must match command count")
    origin = sequence["observation"]["frame"]
    slots = []
    previous_frame = origin
    for index, label in enumerate(labels[:maximum_commands]):
        frame = label["frame"]
        if not previous_frame <= frame < origin + window:
            raise ValueError("noncausal or out-of-order command target")
        slots.append(dict(stop=False, delay_frames=frame - origin,
                          action=label, actor_available=sequence["actor_available"][index],
                          target_available=sequence["target_available"][index]))
        previous_frame = frame
    if len(labels) < maximum_commands:
        slots.append(dict(stop=True, delay_frames=None, action=None,
                          actor_available=False, target_available=False))
    return dict(slots=slots, command_count=len(labels),
                overflow_commands=max(0, len(labels) - maximum_commands))
