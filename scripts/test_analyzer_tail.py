"""Regression tests for bounded rendezvous log reads."""
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace
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

    def test_reset_recovery_groups_reset_wave_and_pairs_consensus(self):
        log_a = b'2026-09-09T08:00:00+00:00 host_mono_ns=1 board=a port=p session=s | TEST RESET EXECUTED\n'
        log_b = b'2026-09-09T08:00:20+00:00 host_mono_ns=2 board=b port=p session=s | TEST RESET EXECUTED\n'
        cycles = [SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:00:30+00:00'), home='aaaa'),
                  SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:00:31+00:00'), home='bbbb')]
        with patch.object(analyzer, 'global_home_convergences', return_value=[
                {'host_time': analyzer.evidence.timestamp('2026-09-09T08:01:00+00:00'),
                 'bssid': 'deadbeef'}]), \
             patch.object(analyzer.evidence, 'parse_evidence', side_effect=[(cycles[:1], 0), (cycles[1:], 0)]):
            rows = analyzer.reset_recovery_events(
                [('a', log_a), ('b', log_b)], required=2)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['boards'], ['a', 'b'])
        self.assertEqual(rows[0]['bssid'], 'deadbeef')
        self.assertEqual(rows[0]['latency'], 60.0)


if __name__ == '__main__':
    unittest.main()
