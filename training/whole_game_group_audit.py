"""Versioned observed-field audit for group commands; no legacy metric changes."""
from collections import Counter


def observed_fields(sequence, decoded, ids, legacy_row):
    result = Counter()
    for index, label in enumerate(sequence["labels"][:len(decoded)]):
        result["commands"] += 1
        active = index < legacy_row["predicted_commands"]
        output = decoded[index]
        positive = set(label["actor_positive"])
        negative = set(label["actor_negative"])
        group = {token for token, at in ids.items() if bool(output["chosen_actor_set"][0, at])} if active else set()
        result["positive_actor_memberships"] += len(positive)
        result["covered_positive_memberships"] += len(group & positive)
        result["known_wrong_memberships"] += len(group & negative)
        result["unknown_memberships"] += len(group - positive - negative)
        exact_group = active and sequence["actor_available"][index] and group == positive
        result["exact_confirmed_actor_sets"] += exact_group
        arguments_correct = active
        for name in ("queued", "order", "unit_type", "technology", "upgrade", "queue_slot"):
            if label["loss_masks"].get(name, False):
                result[name + "_known"] += 1
                correct = active and int(output[name][0].argmax()) == int(label["actions"][name])
                result[name + "_correct"] += correct
                arguments_correct = arguments_correct and correct
        result["all_observed_fields_correct"] += bool(exact_group and arguments_correct and
            legacy_row["command_slots"][index]["full_signature_correct"])
    return result
