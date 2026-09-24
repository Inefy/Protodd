import unittest
import torch

from tests.test_whole_game_group_model import batch
from training.whole_game_group_model import GroupCommandModel
from training.whole_game_group_runtime import RollingGroupInference


class RollingInferenceTest(unittest.TestCase):
    def test_matches_training_history_across_eviction_and_gaps(self):
        torch.manual_seed(42)
        model=GroupCommandModel(width=64,maximum_slots=2).eval()
        adapter=RollingGroupInference(model,history=3)
        prior=[]
        for frame in (0,24,48,96,120,144):
            current=batch()
            current['global'][:,4]=frame/100000
            with torch.no_grad():
                memory=None
                for earlier in prior[-3:]:
                    memory=model(earlier,memory)['memory']
                direct=model.forward_slots(current,memory)
            cached=adapter.step('same-game',frame,current)
            torch.testing.assert_close(cached['backbone']['memory'],direct['backbone']['memory'],rtol=0,atol=0)
            for a,b in zip(cached['slots'],direct['slots']):
                for k in a:
                    torch.testing.assert_close(a[k],b[k],rtol=0,atol=0)
            prior.append(current)

    def test_new_game_and_backwards_frame_reset(self):
        model=GroupCommandModel(width=64,maximum_slots=1).eval()
        adapter=RollingGroupInference(model)
        adapter.step('first',120,batch())
        current=batch()
        direct=model.forward_slots(current)['backbone']['memory']
        for game,frame in (('second',120),('second',0)):
            output=adapter.step(game,frame,current)
            torch.testing.assert_close(output['backbone']['memory'],direct,rtol=0,atol=0)

    def test_duplicate_frame_does_not_advance_memory(self):
        model=GroupCommandModel(width=64,maximum_slots=1).eval()
        adapter=RollingGroupInference(model)
        first=adapter.step('game',0,batch())
        self.assertIs(first,adapter.step('game',0,batch()))
        self.assertEqual(len(adapter.history),1)

    def test_changed_weights_invalidate_cached_encoding(self):
        model=GroupCommandModel(width=64,maximum_slots=1).eval()
        adapter=RollingGroupInference(model)
        adapter.step('game',0,batch())
        with torch.no_grad():
            model.group_count.bias.add_(1)
        with self.assertRaisesRegex(ValueError,'changed'):
            adapter.step('game',24,batch())
        self.assertEqual(len(adapter.history),0)


if __name__=='__main__':
    unittest.main()
