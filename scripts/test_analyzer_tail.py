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

    def test_tail_bytes_accepts_decimal_megabyte_suffix(self):
        self.assertEqual(analyzer.parse_tail_bytes('2m'), 2_000_000)
        self.assertEqual(analyzer.parse_tail_bytes('3M'), 3_000_000)
        self.assertEqual(analyzer.parse_tail_bytes('123'), 123)
        self.assertEqual(analyzer.parse_tail_bytes('-1'), -1)
        with self.assertRaises(Exception):
            analyzer.parse_tail_bytes('-1m')
        with self.assertRaises(Exception):
            analyzer.parse_tail_bytes('2mb')

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

    def test_reset_recovery_reports_minimum_to_consensus_transition(self):
        log_a = b'2026-09-09T08:00:00+00:00 host_mono_ns=1 board=a port=p session=s | TEST RESET EXECUTED\n'
        log_b = b'2026-09-09T08:00:40+00:00 host_mono_ns=2 board=b port=p session=s | TEST RESET EXECUTED\n'
        cycles_a = [SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:00:30+00:00'), home='aaaa'),
                    SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:00:31+00:00'), home='bbbb'),
                    SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:01:00+00:00'), home='deadbeef')]
        cycles_b = [SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:00:30+00:00'), home='aaaa'),
                    SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:00:31+00:00'), home='aaaa'),
                    SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:01:00+00:00'), home='deadbeef')]
        with patch.object(analyzer.evidence, 'parse_evidence', side_effect=[(cycles_a, 0), (cycles_b, 0)]):
            rows = analyzer.reset_recovery_events(
                [('a', log_a), ('b', log_b)], required=2)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['bssid'], 'deadbeef')
        self.assertEqual(rows[0]['minimum_size'], 1)
        self.assertEqual(rows[0]['latency'], 0.0)

    def test_epoch_recovery_waits_for_every_post_marker_home(self):
        marker_a = b'2026-09-09T08:00:00+00:00 host_mono_ns=1 board=a port=p session=s | 00000.1 TEST EPOCH RESET reason=cold delay-usec=60000000\n'
        marker_b = b'2026-09-09T08:00:02+00:00 host_mono_ns=2 board=b port=p session=s | 00000.1 TEST EPOCH RESET reason=cold delay-usec=90000000\n'
        cycles_a = [SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:01:05+00:00'), home='deadbeef')]
        cycles_b = [SimpleNamespace(wall=analyzer.evidence.timestamp('2026-09-09T08:01:35+00:00'), home='deadbeef')]
        with patch.object(analyzer.evidence, 'parse_evidence',
                          side_effect=[(cycles_a, 0), (cycles_b, 0)]):
            rows = analyzer.epoch_recovery_events(
                [('a', marker_a), ('b', marker_b)], required=2)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['bssid'], 'deadbeef')
        self.assertEqual(rows[0]['latency'], 93.0)

    def test_merge_attribution_orders_observed_stages(self):
        def line(stamp, board, body):
            return (f'{stamp} host_mono_ns=1 board={board} port=p session=s | '
                    f'{body}\n').encode()

        marker_a = line('2026-09-09T08:00:00+00:00', 'a',
                        'TEST EPOCH RESET reason=cold')
        marker_b = line('2026-09-09T08:00:01+00:00', 'b',
                        'TEST EPOCH RESET reason=cold')
        log_a = (marker_a +
                 line('2026-09-09T08:00:20+00:00', 'a',
                      'visitor-positive-evidence target bbbb members 1 from aaaa full 1') +
                 line('2026-09-09T08:00:30+00:00', 'a',
                      'migration-proposal visitor target bbbb target-members 1') +
                 line('2026-09-09T08:00:40+00:00', 'a',
                      'migration-proposal committed target bbbb target-snapshot 1'))
        log_b = marker_b
        cycles_a = [
            SimpleNamespace(wall=analyzer.evidence.timestamp(
                '2026-09-09T08:00:10+00:00'), home='aaaa'),
            SimpleNamespace(wall=analyzer.evidence.timestamp(
                '2026-09-09T08:01:00+00:00'), home='bbbb')]
        cycles_b = [
            SimpleNamespace(wall=analyzer.evidence.timestamp(
                '2026-09-09T08:00:11+00:00'), home='bbbb'),
            SimpleNamespace(wall=analyzer.evidence.timestamp(
                '2026-09-09T08:01:01+00:00'), home='bbbb')]
        # epoch_recovery_events and complete_topology_snapshots each parse both
        # boards, so provide the same four results twice.
        parsed = [(cycles_a, 0), (cycles_b, 0), (cycles_a, 0), (cycles_b, 0)]
        with patch.object(analyzer.evidence, 'parse_evidence', side_effect=parsed):
            rows = analyzer.merge_attribution_events(
                [('a', log_a), ('b', log_b)], required=2)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['large_size'], 1)
        self.assertEqual(rows[0]['evidence_time'] - rows[0]['large_time'], 9.0)
        self.assertEqual(rows[0]['proposal_time'] - rows[0]['large_time'], 19.0)
        self.assertEqual(rows[0]['commit_time'] - rows[0]['large_time'], 29.0)
        self.assertEqual(rows[0]['tail_seconds'], 49.0)

    def test_snapshot_rejection_reason_and_observer_disagreement(self):
        fields = {
            'actual': 'reject-not-preferred', 'hm': '4', 'tm': '3',
            'h': '66a4b792e676', 't': '60a4b792da8a',
        }
        self.assertEqual(analyzer.decision_snapshot_reason(fields),
                         'target-smaller')

        body = ('@v v=1 e=1234 w=2 x=3 i=0 o=0 k=1 src=visitor '
                'h=66a4b792e676 hm=4 t=60a4b792da8a tm=3 '
                'pb=0 ph=0 pm=0 pa=0 tv=0 n=0 q=0 '
                'actual=reject-not-preferred replay=reject-not-preferred')
        line = (f'2026-09-09T08:00:20+00:00 host_mono_ns=1 board=a '
                f'port=p session=s | {body}\n').encode()
        topology = [{
            'host_time': analyzer.evidence.timestamp('2026-09-09T08:00:19+00:00'),
            'counts': {'66a4b792e676': 3, '60a4b792da8a': 4},
            'largest': 4,
        }]
        rows = analyzer.decision_snapshot_events([('a', line)], topology)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['reason'], 'target-smaller')
        self.assertTrue(rows[0]['observer_disagrees'])
        self.assertEqual(rows[0]['observer_home_members'], 3)
        self.assertEqual(rows[0]['observer_target_members'], 4)

    def test_equal_size_higher_bssid_reason(self):
        fields = {
            'actual': 'reject-not-preferred', 'hm': '4', 'tm': '4',
            'h': '60a4b792da8a', 't': '66a4b792e676',
        }
        self.assertEqual(analyzer.decision_snapshot_reason(fields),
                         'equal-higher-bssid')

    def test_membership_ttl_counterfactual_flips_stale_target(self):
        stamp = analyzer.evidence.timestamp('2026-09-09T08:00:20+00:00')
        large = {'host_time': stamp - 10, 'counts': {'aaaa': 4, 'bbbb': 3},
                 'largest': 4}
        decision = {
            'host_time': stamp, 'fields': {'k': '1'},
            'home': 'aaaa', 'target': 'bbbb',
            'home_members': 3, 'target_members': 2,
            'observer_home_members': 3, 'observer_target_members': 4,
            'association_rows': [
                {'m': f'{i:x}', 'b': 'aaaa', 'a': '0'} for i in range(1, 4)
            ] + [
                {'m': '10', 'b': 'bbbb', 'a': '0'},
                {'m': '11', 'b': 'bbbb', 'a': '0'},
                {'m': '12', 'b': 'bbbb', 'a': '7'},
                {'m': '13', 'b': 'bbbb', 'a': '8'},
            ],
        }
        epoch = {'epoch_time': stamp - 20, 'consensus_time': stamp + 20}
        with patch.object(analyzer, 'complete_topology_snapshots',
                          return_value=[large]), \
             patch.object(analyzer, 'decision_snapshot_events',
                          return_value=[decision]), \
             patch.object(analyzer, 'epoch_recovery_events',
                          return_value=[epoch]):
            report = analyzer.membership_ttl_counterfactual([], 7, (8,))
        self.assertEqual(report['reconstructed'], 1)
        self.assertEqual(report['ttls'][8]['rejected_to_preferred'], 1)
        self.assertEqual(report['ttls'][8]['observer_alignment_improved'], 1)
        self.assertEqual(report['ttls'][8]['preferred_to_rejected'], 0)

    def test_membership_counterfactual_excludes_baseline_mismatch(self):
        stamp = analyzer.evidence.timestamp('2026-09-09T08:00:20+00:00')
        decision = {
            'host_time': stamp, 'fields': {'k': '1'},
            'home': 'aaaa', 'target': 'bbbb',
            'home_members': 2, 'target_members': 1,
            'observer_home_members': None, 'observer_target_members': None,
            'association_rows': [],
        }
        with patch.object(analyzer, 'complete_topology_snapshots',
                          return_value=[{'host_time': stamp - 1, 'largest': 4}]), \
             patch.object(analyzer, 'decision_snapshot_events',
                          return_value=[decision]), \
             patch.object(analyzer, 'epoch_recovery_events', return_value=[{
                 'epoch_time': stamp - 2, 'consensus_time': stamp + 2}]):
            report = analyzer.membership_ttl_counterfactual([], 7, (8,))
        self.assertEqual(report['reconstructed'], 0)
        self.assertEqual(report['missing_or_mismatch'], 1)


if __name__ == '__main__':
    unittest.main()
