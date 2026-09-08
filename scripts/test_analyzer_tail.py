"""Regression tests for bounded rendezvous log reads."""
import pathlib
import sys
import tempfile
import unittest

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


if __name__ == '__main__':
    unittest.main()
