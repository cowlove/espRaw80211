import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import analyze_rendezvous as analyzer
from test_swarm_config import swarm_board_count


class SwarmOracleTests(unittest.TestCase):
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

    def test_current_home_distribution_is_popularity_sorted(self):
        def data(home):
            return (f'2026-09-08T12:00:00-07:00 host_mono_ns=1 board=x port=/dev/x session=s | '
                    f'00000.1 report-identity incarnation aa wake 1 wire-version 7 exchange 1\n'
                    f'2026-09-08T12:00:00-07:00 host_mono_ns=2 board=x port=/dev/x session=s | '
                    f'00005.0 ESP-NOW exchange phase started\n'
                    f'2026-09-08T12:00:01-07:00 host_mono_ns=3 board=x port=/dev/x session=s | '
                    f'00010.0 gossip home exchange healthy home {home} listeners 1 rawrx 1 rx 1 valid 1\n'
                    f'2026-09-08T12:00:02-07:00 host_mono_ns=4 board=x port=/dev/x session=s | '
                    f'00010.1 exchange complete interval 1\n').encode()
        datasets = [('c', data('bbb')), ('a', data('aaa')), ('b', data('aaa')),
                    ('missing', b'no complete observation\n')]
        rows, unknown = analyzer.current_home_distribution(datasets)
        self.assertEqual(rows, [
            {'bssid': 'aaa', 'devices': 2, 'boards': ['a', 'b']},
            {'bssid': 'bbb', 'devices': 1, 'boards': ['c']},
        ])
        self.assertEqual(unknown, ['missing'])
