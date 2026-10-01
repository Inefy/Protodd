import json
from pathlib import Path
import tempfile
import unittest

from tools.allin_campaign_review import opening_observations


class AllinCampaignReviewTests(unittest.TestCase):
    def test_completed_counts_and_forward_motion_are_not_command_claims(self):
        def entity(kind, complete, position, relation=0):
            return dict(type=kind, completed=complete, position=position, relation=relation)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'observations.jsonl'
            rows = [dict(frame=7, entities=[entity(154, 1, [100, 100])]),
                    dict(frame=31, entities=[entity(65, 0, [1600, 100])] * 4),
                    dict(frame=55, entities=[entity(65, 1, [1600, 100], 1)] * 4),
                    dict(frame=79, entities=[entity(65, 1, [300, 100])] * 4),
                    dict(frame=103, entities=[entity(65, 1, [1600, 100])] * 4)]
            path.write_text(''.join(json.dumps(row) + '\n' for row in rows))
            result = opening_observations(path)
            self.assertEqual(result['first_completed']['65_4'], 79)
            self.assertEqual(result['max_completed'][65], 4)
            self.assertEqual(result['first_four_fighters_1200_from_home'], 103)


if __name__ == '__main__':
    unittest.main()
