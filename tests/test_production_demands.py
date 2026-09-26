import unittest
import numpy as np
from training.production_demands import demand_rows, ACTIONS
from training.schema import load_schema


def rows(frames):
    n=len(frames)
    return dict(frames=np.array(frames), labels=np.zeros(n,dtype=np.int64),
        action_frames=np.array(frames), features=np.zeros((n,2),dtype=np.float32))


def event(frame,action='train_probe',owner=0,accepted=True,repeated=False):
    return dict(frame=frame,action=action,owner=owner,accepted=accepted,repeated=repeated)


class DemandTargetsTest(unittest.TestCase):
    def test_all_concurrent_commands_and_strict_history(self):
        r=rows([0,24,48])
        events=[event(0),event(0,'build_pylon'),event(4),event(24,'build_gateway')]
        x,y,past,valid=demand_rows(r,events,0,96,horizon=48)
        self.assertEqual(y[0,:3].tolist(),[2,1,1])
        self.assertEqual(past[0,:3].tolist(),[0,0,0])
        self.assertEqual(past[1,:3].tolist(),[2,1,0])
        self.assertEqual(y[1,:3].tolist(),[0,0,1])
        self.assertTrue(valid.all())
        self.assertEqual(x[0,2:2+len(ACTIONS)].sum(),0)

    def test_future_changes_only_targets_and_end_is_censored(self):
        r=rows([0,24,48])
        x,y,_,v=demand_rows(r,[],0,80,48)
        xx,yy,_,_=demand_rows(r,[event(50)],0,80,48)
        np.testing.assert_array_equal(x,xx)
        self.assertNotEqual(y[1,0],yy[1,0])
        self.assertEqual(v.tolist(),[True,True,False])

    def test_rejected_repeated_and_other_owner_never_become_demand(self):
        _,y,_,_=demand_rows(rows([0]),[event(4,accepted=False),
            event(5,repeated=True),event(6,owner=1)],0,100,48)
        self.assertEqual(int(y.sum()),0)

    def test_gap_does_not_hide_full_log_events(self):
        _,y,_,v=demand_rows(rows([0,240]),[event(100,'build_pylon')],0,500,240)
        self.assertTrue(v.all()); self.assertEqual(y[0,1],1)

    def test_mismatched_first_action_rejected(self):
        r=rows([0]);r['labels'][0]=load_schema()['actions'].index('train_probe')
        with self.assertRaisesRegex(ValueError,'does not match'):
            demand_rows(r,[],0,500)


if __name__=='__main__': unittest.main()
