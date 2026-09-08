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
