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

    def test_initial_qualification_and_age(self):
        required = swarm_board_count()
        data = (f'2026-09-08T12:00:00-07:00 host_mono_ns=1 board=usb0 port=/dev/x session=s | logger-session x\n'
                f'2026-09-08T12:01:00-07:00 host_mono_ns=2 board=usb0 port=/dev/x session=s | 00005.0 ESP-NOW exchange phase started\n'
                f'2026-09-08T12:01:01-07:00 host_mono_ns=3 board=usb0 port=/dev/x session=s | 00010.0 gossip home exchange healthy home abc listeners {required}\n'
                f'2026-09-08T12:01:02-07:00 host_mono_ns=4 board=usb0 port=/dev/x session=s | 00010.1 deep sleep 20 sec\n').encode()
        first = analyzer.initial_qualification(data, required)
        self.assertEqual(first, analyzer.evidence.timestamp('2026-09-08T12:01:00-07:00'))
        self.assertEqual(analyzer.human_age(3661), '1h1m')
