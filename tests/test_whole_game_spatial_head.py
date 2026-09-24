import unittest

import torch

from training.whole_game_spatial_head import SpatialPositionHead, entity_raster
from training.whole_game_spatial_probe import _batch


class SpatialPositionHeadTests(unittest.TestCase):
    def test_location_changes_logits_and_receives_supervised_gradient(self):
        torch.manual_seed(7)
        head = SpatialPositionHead(state_width=16, grid=4, key_width=16)
        spatial = torch.zeros((2, 3, 16, 16), requires_grad=True)
        with torch.no_grad():
            spatial[0, 0, 2:4, 2:4] = 1
            spatial[1, 0, 12:14, 12:14] = 1
        state = torch.zeros((2, 16))
        actors = torch.full((2, 2), 0.5)
        kind = torch.ones(2, dtype=torch.long)
        output = head(state, spatial, actors, kind)
        self.assertEqual(output["logits"].shape, (2, 16))
        self.assertFalse(torch.allclose(output["logits"][0], output["logits"][1]))
        loss = head.loss(output, torch.tensor([[0.15, 0.15], [0.85, 0.85]]))
        loss.backward()
        self.assertGreater(spatial.grad.abs().sum().item(), 0)
        self.assertGreater(head.cells[0].weight.grad.abs().sum().item(), 0)
        self.assertGreater(head.offset[-1].weight.grad.abs().sum().item(), 0)

    def test_cell_decoder_returns_normalized_coordinates(self):
        head = SpatialPositionHead(state_width=8, grid=4, key_width=8)
        with torch.no_grad():
            head.offset[-1].weight.zero_()
            head.offset[-1].bias.zero_()
        output = head(torch.zeros((1, 8)), torch.zeros((1, 3, 8, 8)),
                      torch.zeros((1, 2)), torch.zeros(1, dtype=torch.long))
        output["logits"][:] = -10
        output["logits"][0, 13] = 10  # x=1, y=3 in a 4x4 grid.
        self.assertTrue(torch.allclose(head.decode(output), torch.tensor([[0.375, 0.875]])))

    def test_entity_raster_preserves_player_visible_geometry(self):
        numeric = torch.zeros((1, 5, 16))
        numeric[0, :, :2] = torch.tensor([[0.1, 0.1], [0.8, 0.8], [0.2, 0.8],
                                           [0.7, 0.2], [0.9, 0.1]])
        numeric[0, 1, 4] = 1
        batch = dict(entity_numeric=numeric,
                     relation=torch.tensor([[0, 1, 1, 2, 0]]),
                     entity_mask=torch.tensor([[1, 1, 1, 1, 0]], dtype=torch.bool))
        raster = entity_raster(batch, 4)
        self.assertEqual(raster.shape, (1, 4, 4, 4))
        for channel, y, x in ((0, 0, 0), (1, 3, 3), (2, 3, 0), (3, 0, 2)):
            self.assertAlmostEqual(raster[0, channel, y, x].item(), torch.log(torch.tensor(2.)).item())
        self.assertEqual(torch.count_nonzero(raster).item(), 4)
        head = SpatialPositionHead(state_width=8, grid=4, key_width=8, spatial_channels=7)
        extended = torch.cat((torch.zeros((1, 3, 4, 4)), raster), dim=1)
        self.assertEqual(head(torch.zeros((1, 8)), extended, torch.zeros((1, 2)),
                              torch.zeros(1, dtype=torch.long))["logits"].shape, (1, 16))

    def test_predicted_context_is_separate_from_oracle(self):
        example = dict(state=torch.zeros(8), spatial=torch.zeros((7, 4, 4)),
                       actor_xy=torch.tensor([0.1, 0.2]), kind=torch.tensor(1),
                       predicted_actor_xy=torch.tensor([0.8, 0.9]),
                       predicted_kind=torch.tensor(2), target=torch.tensor([0.3, 0.4]))
        oracle = _batch([example], torch.device("cpu"))
        predicted = _batch([example], torch.device("cpu"), predicted_context=True)
        self.assertTrue(torch.equal(oracle[2], torch.tensor([[0.1, 0.2]])))
        self.assertTrue(torch.equal(predicted[2], torch.tensor([[0.8, 0.9]])))
        self.assertEqual(oracle[3].item(), 1)
        self.assertEqual(predicted[3].item(), 2)


if __name__ == "__main__":
    unittest.main()
