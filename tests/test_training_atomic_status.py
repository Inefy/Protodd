import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from training.macro_commitment_probe import write
import training.macro_commitment_probe as module


class AtomicStatusTest(unittest.TestCase):
    def test_transient_windows_lock_preserves_old_status_then_publishes(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'status.json';write(path,{'stage':'training'})
            original=module.os.replace;calls=[]
            def replace(source,target):
                calls.append(1)
                if len(calls)==1:
                    self.assertEqual(json.loads(path.read_text())['stage'],'training')
                    raise PermissionError('sharing violation')
                return original(source,target)
            with patch.object(module.os,'replace',side_effect=replace),patch.object(module.time,'sleep'):
                write(path,{'stage':'complete'})
            self.assertEqual(len(calls),2)
            self.assertEqual(json.loads(path.read_text())['stage'],'complete')

    def test_permanent_denial_is_bounded_and_does_not_destroy_old_status(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'status.json';write(path,{'stage':'training'})
            with patch.object(module.os,'replace',side_effect=PermissionError()) as replace,patch.object(module.time,'sleep'):
                with self.assertRaises(PermissionError):write(path,{'stage':'complete'})
            self.assertEqual(replace.call_count,20)
            self.assertEqual(json.loads(path.read_text())['stage'],'training')


if __name__=='__main__':unittest.main()
