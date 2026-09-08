"""Regression tests for bounded rendezvous log reads."""
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import analyze_rendezvous as analyzer


class AnalyzerTailTests(unittest.TestCase):
    def test_default_is_one_megabyte(self):
        self.assertEqual(analyzer.DEFAULT_TAIL_BYTES, 1_000_000)

    def test_local_read_is_bounded_suffix(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'large.log'
            path.write_bytes(b'a' * 100 + b'LAST')
            data = analyzer.read_local_tail(path, 16)
            self.assertEqual(data, b'a' * 12 + b'LAST')
            data = analyzer.read_local_tail(path, 1000)
            self.assertEqual(data, path.read_bytes())
            self.assertEqual(analyzer.read_local_tail(path, 0), path.read_bytes())
            self.assertEqual(analyzer.read_local_tail(path, -1), path.read_bytes())

    def test_remote_zero_disables_tail(self):
        completed = Mock(stdout=b'full log')
        with patch.object(analyzer.subprocess, 'run', return_value=completed) as run:
            self.assertEqual(analyzer.read_remote('host', '~/log', 0), b'full log')
            self.assertIn('cat --', run.call_args.args[0][-1])
            self.assertNotIn('tail -c', run.call_args.args[0][-1])
        with patch.object(analyzer.subprocess, 'run', return_value=completed) as run:
            analyzer.read_remote('host', '~/log', 123)
            self.assertIn('tail -c 123', run.call_args.args[0][-1])


if __name__ == '__main__':
    unittest.main()
