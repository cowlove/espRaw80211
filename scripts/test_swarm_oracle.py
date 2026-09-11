import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import analyze_rendezvous as analyzer
from test_swarm_config import swarm_board_count


class SwarmOracleTests(unittest.TestCase):
    def test_cold_epoch_delay_is_one_to_three_rendezvous_periods(self):
        config = (Path(__file__).resolve().parents[1] / 'testSwarmConfig.h').read_text()
        self.assertIn('#define ARTIFICIAL_TEST_COLD_RESET_CLEARS_STATE 1', config)
        self.assertIn('#define ARTIFICIAL_TEST_COLD_RESET_MIN_SLEEP_SECONDS 60', config)
        self.assertIn('#define ARTIFICIAL_TEST_COLD_RESET_JITTER_SECONDS 60', config)

    def test_shared_parameter(self):
        self.assertGreater(swarm_board_count(), 0)
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'config.h'
            config.write_text('#define ARTIFICIAL_TEST_SWARM_BOARD_COUNT 12\n')
            self.assertEqual(swarm_board_count(config), 12)

    def test_unlogged_members_can_qualify_single_log(self):
        def data(listeners):
            return (f'00005.0 ESP-NOW exchange phase started\n'
                    f'00010.0 gossip home exchange healthy home abc listeners {listeners}\n'
                    f'00010.1 deep sleep 20 sec\n').encode()
        # One log can observe a nine-board swarm: no nine-USB prerequisite.
        with patch.object(analyzer, 'swarm_board_count', return_value=9), contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(analyzer.rendezvous_dashboard('one USB', data(8)))
            self.assertTrue(analyzer.rendezvous_dashboard('one USB', data(9)))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(analyzer.rendezvous_dashboard('one USB', data(swarm_board_count()-1)))
            self.assertTrue(analyzer.rendezvous_dashboard('one USB', data(swarm_board_count())))

    def test_global_home_convergence_ignores_listener_count(self):
        def data(board, minute, homes):
            lines = []
            for i, home in enumerate(homes):
                stamp = f'2026-09-08T12:{minute+i:02d}:00-07:00'
                prefix = f'{stamp} host_mono_ns={i+1} board={board} port=/dev/x session=s | '
                lines.extend([prefix + '00005.0 ESP-NOW exchange phase started',
                              prefix + f'00010.0 gossip home exchange healthy home {home} listeners 1',
                              prefix + '00010.1 deep sleep 20 sec'])
            return ('\n'.join(lines) + '\n').encode()
        datasets = [('a', data('a', 0, ['aaa', 'bbb'])),
                    ('b', data('b', 0, ['aaa', 'bbb']))]
        events = analyzer.global_home_convergences(datasets, 2)
        self.assertEqual([event['bssid'] for event in events], ['aaa', 'bbb'])
        self.assertEqual(analyzer.human_age(3661), '1h1m')

    def test_foreign_coverage_classifies_strict_threshold_and_gap(self):
        # Evidence establishes presence for a fixed freshness interval.  The
        # union, rather than record count, is what labels an epoch.
        covered, records = analyzer.foreign_coverage(100, 200, [90, 140], 50)
        self.assertEqual((covered, records), (90, 2))
        self.assertEqual(analyzer.foreign_coverage(100, 200, [], 50), (0, 0))
        self.assertEqual(analyzer.foreign_condition(covered / 100, covered, .60), 'F+')
        partial, _ = analyzer.foreign_coverage(100, 200, [150], 30)
        self.assertEqual(analyzer.foreign_condition(partial / 100, partial, .60),
                         'inconclusive')
        self.assertEqual(analyzer.foreign_condition(0, 0, .60), 'F-')

    def test_current_home_distribution_is_popularity_sorted(self):
        def data(home, minute=0, wake=1):
            prefix = f'2026-09-08T12:{minute:02d}'
            return (f'{prefix}:00-07:00 host_mono_ns=1 board=x port=/dev/x session=s | '
                    f'00000.1 report-identity incarnation aa wake {wake} wire-version 7 exchange {wake}\n'
                    f'{prefix}:00-07:00 host_mono_ns=2 board=x port=/dev/x session=s | '
                    f'00005.0 ESP-NOW exchange phase started\n'
                    f'{prefix}:01-07:00 host_mono_ns=3 board=x port=/dev/x session=s | '
                    f'00010.0 gossip home exchange healthy home {home} listeners 1 rawrx 1 rx 1 valid 1\n'
                    f'{prefix}:02-07:00 host_mono_ns=4 board=x port=/dev/x session=s | '
                    f'00010.1 exchange complete interval {wake}\n').encode()
        datasets = [('c', data('bbb')), ('a', data('aaa')), ('b', data('aaa')),
                    ('missing', b'no complete observation\n')]
        rows, unknown = analyzer.current_home_distribution(datasets)
        self.assertEqual(rows, [
            {'bssid': 'aaa', 'devices': 2, 'boards': ['a', 'b']},
            {'bssid': 'bbb', 'devices': 1, 'boards': ['c']},
        ])
        self.assertEqual(unknown, ['missing'])

    def test_current_distribution_stability_resets_on_a_move(self):
        def data(board, observations):
            chunks = []
            for wake, (minute, home) in enumerate(observations, 1):
                prefix = f'2026-09-08T12:{minute:02d}'
                chunks.append(
                    f'{prefix}:00-07:00 host_mono_ns=1 board={board} port=/dev/x session=s | '
                    f'00000.1 report-identity incarnation aa wake {wake} wire-version 7 exchange {wake}\n'
                    f'{prefix}:00-07:00 host_mono_ns=2 board={board} port=/dev/x session=s | '
                    f'00005.0 ESP-NOW exchange phase started\n'
                    f'{prefix}:01-07:00 host_mono_ns=3 board={board} port=/dev/x session=s | '
                    f'00010.0 gossip home exchange healthy home {home} listeners 1 rawrx 1 rx 1 valid 1\n'
                    f'{prefix}:02-07:00 host_mono_ns=4 board={board} port=/dev/x session=s | '
                    f'00010.1 exchange complete interval {wake}\n')
            return ''.join(chunks).encode()
        datasets = [
            ('a', data('a', [(0, 'aaa'), (2, 'bbb'), (4, 'bbb')])),
            ('b', data('b', [(1, 'aaa'), (3, 'bbb'), (5, 'bbb')])),
        ]
        # a's move starts the current assignment; b's later move changes it
        # again, followed by two more unchanged observations.
        self.assertEqual(analyzer.current_home_unchanged_cycles(datasets), 3)
