import unittest
import numpy as np
from training.macro_commitment_probe import commitment_targets


def rows(frames, labels, action_frames, masks=None):
    return dict(frames=np.array(frames),labels=np.array(labels),action_frames=np.array(action_frames),
                masks=np.array(masks or [7]*len(frames),dtype=np.uint64))


class CommitmentTargetTest(unittest.TestCase):
    def test_future_event_only_changes_target(self):
        r=rows([0,24,48,72,96,120,144],[0,0,2,0,0,0,0],[-1,-1,52,-1,-1,-1,-1])
        before={k:v.copy() for k,v in r.items()}
        targets,valid=commitment_targets(r,48)
        self.assertEqual(targets.tolist(),[0,2,2,0,0,0,0])
        self.assertEqual(valid.tolist(),[True,True,True,True,False,False,False])
        for k in r:
            np.testing.assert_array_equal(r[k],before[k])

    def test_future_masked_intent_is_not_wait(self):
        targets,valid=commitment_targets(rows([0,24,48,72],[0,2,0,0],[-1,25,-1,-1],[3,7,7,7]),48)
        self.assertFalse(valid[0])
        self.assertTrue(valid[1])
        self.assertEqual(targets[1],2)

    def test_gap_hides_unknown_events(self):
        _,valid=commitment_targets(rows([0,24,240,264,288],[0,0,2,0,0],[-1,-1,242,-1,-1]),120)
        self.assertFalse(valid[0]);self.assertFalse(valid[1]);self.assertTrue(valid[2])


if __name__=='__main__':
    unittest.main()
