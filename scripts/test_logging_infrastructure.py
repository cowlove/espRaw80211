"""Host-only regression tests; never opens a serial port or runs screen."""
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

import analyze_rendezvous as analyzer
import deploy_usb_screens as deploy
import test_farm_supervisor as supervisor
import timestamp_serial as serial_logger


class InfrastructureTests(unittest.TestCase):
    def test_reset_request_survives_until_consumed(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'usb0.request'
            path.write_text('epoch-17\n')
            self.assertEqual(serial_logger.requested_reset(path), 'epoch-17')
            self.assertTrue(path.exists())

    def test_hard_reset_keeps_io0_high(self):
        source = unittest.mock.Mock()
        with patch.object(serial_logger.time, 'sleep'):
            serial_logger.pulse_hard_reset(source)
        self.assertEqual(source.method_calls, [
            unittest.mock.call.setDTR(False),
            unittest.mock.call.setRTS(True),
            unittest.mock.call.setRTS(False),
        ])

    def test_supervisor_requires_fresh_unanimous_homes(self):
        now = 1_800_000_000
        def board(name, home, age=1):
            line = (f'2027-01-15T08:00:00+00:00 host_mono_ns=1 board={name} '
                    f'port=p session=s | 00001.0 @i e=aa w=1 v=7 max=180\n'
                    f'2027-01-15T08:00:01+00:00 host_mono_ns=2 board={name} '
                    f'port=p session=s | 00002.0 @e begin=1\n'
                    f'2027-01-15T08:00:02+00:00 host_mono_ns=3 board={name} '
                    f'port=p session=s | 00003.0 gossip home exchange healthy home {home} listeners 7 rawrx 3 rx 3 valid 3 peers 1\n'
                    f'2027-01-15T08:00:03+00:00 host_mono_ns=4 board={name} '
                    f'port=p session=s | 00004.0 @e end=1\n')
            data = line.encode()
            parsed = supervisor.evidence.summarize(data)
            parsed['latest']['host_time'] = now - age
            return data, parsed
        datasets, summaries = [], {}
        for index in range(7):
            name = f'board{index}'
            data, summaries[name] = board(name, 'abc')
            datasets.append((name, data))
        with patch.object(supervisor.evidence, 'summarize',
                          side_effect=lambda data: next(
                              summaries[name] for name, value in datasets if value is data)):
            home, reason = supervisor.convergence_snapshot(datasets, now, 7, 180)
        self.assertEqual((home, reason), ('abc', None))

    def test_epoch_ack_requires_marker_after_request(self):
        data = (
            b'2026-09-09T10:00:00+00:00 host_mono_ns=1 board=usb0 port=p session=s | TEST EPOCH RESET reason=cold delay-usec=60000000\n'
        )
        marker = supervisor.evidence.timestamp('2026-09-09T10:00:00+00:00')
        self.assertEqual(set(supervisor.epoch_acknowledgments(
            [('usb0', data)], marker - 1)), {'usb0'})
        self.assertEqual(supervisor.epoch_acknowledgments([('usb0', data)],
                                                          marker + 5), {})

    def test_latest_epoch_boundary_uses_newest_retained_marker(self):
        def marker(second):
            return (f'2026-09-09T10:00:{second:02d}+00:00 host_mono_ns=1 '
                    f'board=usb port=p session=s | TEST EPOCH RESET reason=cold\n').encode()
        datasets = [('local usb0', marker(1)), ('miner6 usb0', marker(4))]
        expected = supervisor.evidence.timestamp('2026-09-09T10:00:04+00:00')
        self.assertEqual(supervisor.latest_epoch_boundary(datasets, 10), expected)
        self.assertEqual(supervisor.latest_epoch_boundary(datasets[:1], 2),
                         supervisor.evidence.timestamp('2026-09-09T10:00:01+00:00'))

    def test_supervisor_rejects_pre_epoch_home_observation(self):
        now = 1_800_000_000
        datasets = [('usb0', b'data')]
        summary = {'latest': {'home': 'abc', 'host_time': now - 10}}
        with patch.object(supervisor.evidence, 'summarize', return_value=summary):
            home, reason = supervisor.convergence_snapshot(
                datasets, now, 1, 180, {'usb0': now - 5})
        self.assertIsNone(home)
        self.assertEqual(reason, 'usb0 awaiting post-epoch home observation')

    def test_supervisor_formats_compact_grouped_topology(self):
        groups = {
            '60a4b792da8a': ['local usb3', 'miner6 usb1', 'local usb0'],
            '66a4b792e676': ['miner6 usb0', 'local usb4'],
        }
        self.assertEqual(
            supervisor.format_home_groups(groups, ['local usb2', 'local usb1']),
            '60..da8a=L0,L3,M1 66..e676=L4,M0 ?=L1,L2')

    def test_supervisor_marks_recent_foreign_association(self):
        data = (b'2026-09-09T10:00:00+00:00 host_mono_ns=1 board=usb0 '
                b'port=p session=s | association origin e072a1a23784 '
                b'selected 60a4b792e676 generation 1 age 0 selected-home 1 '
                b'fresh 1 qualifies 1\n')
        now = supervisor.evidence.timestamp('2026-09-09T10:00:05+00:00')
        self.assertTrue(supervisor.foreign_present([('usb0', data)], now, 180))

    def test_supervisor_ignores_stale_foreign_association(self):
        data = (b'2026-09-09T09:00:00+00:00 host_mono_ns=1 board=usb0 '
                b'port=p session=s | association origin e072a1a23784 '
                b'selected 60a4b792e676 generation 1 age 0 selected-home 1 '
                b'fresh 1 qualifies 1\n')
        now = supervisor.evidence.timestamp('2026-09-09T10:00:05+00:00')
        self.assertFalse(supervisor.foreign_present([('usb0', data)], now, 180))

    def test_supervisor_marks_received_foreign_packet(self):
        data = (b'2026-09-09T10:00:00+00:00 host_mono_ns=1 board=usb0 '
                b'port=p session=s | espnow summary origin e072a1a23784 '
                b'radio-from e072a1a23784 frames 3 valid 3\n')
        now = supervisor.evidence.timestamp('2026-09-09T10:00:05+00:00')
        self.assertTrue(supervisor.foreign_present([('usb0', data)], now, 180))

    def test_supervisor_marks_compact_received_foreign_packet(self):
        data = (b'2026-09-09T10:00:00+00:00 host_mono_ns=1 board=usb0 '
                b'port=p session=s | @p o=e072a1a23784 r=e072a1a23784 '
                b'f=3 v=3\n')
        now = supervisor.evidence.timestamp('2026-09-09T10:00:05+00:00')
        self.assertTrue(supervisor.foreign_present([('usb0', data)], now, 180))

    def test_supervisor_marks_compact_foreign_radio_sender(self):
        data = (b'2026-09-09T10:00:00+00:00 host_mono_ns=1 board=usb0 '
                b'port=p session=s | @p o=8437a2a172e0 r=e072a1a23784 '
                b'f=3 v=3\n')
        now = supervisor.evidence.timestamp('2026-09-09T10:00:05+00:00')
        self.assertTrue(supervisor.foreign_present([('usb0', data)], now, 180))

    def test_supervisor_timestamp_is_compact(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            logfile = Path(directory) / 'supervisor.log'
            with patch.object(supervisor.time, 'localtime') as localtime, \
                 patch.object(supervisor, 'status_log', logfile), \
                 patch('builtins.print') as output:
                localtime.return_value = supervisor.time.struct_time(
                    (2026, 9, 9, 7, 8, 9, 2, 252, -1))
                supervisor.timestamped('not converged', 123)
                supervisor.timestamped('converged', 124)
            output.assert_any_call('07:08:09 not converged', flush=True)
            self.assertEqual(logfile.read_text().splitlines(),
                             ['07:08:09 not converged', '07:08:09 converged'])

    def test_prefixed_cycle_matches_legacy(self):
        lines = ['00005.0 ESP-NOW exchange phase started',
                 '00010.0 gossip home exchange healthy home abc listeners 6',
                 '00010.1 deep sleep 20 sec', '00010.2 test consensus 10/10']
        legacy = '\n'.join(lines).encode()
        prefixed = '\n'.join('2026-09-08T10:00:00Z host_mono_ns=1 | ' + s
                             for s in lines).encode()
        self.assertEqual(analyzer.parse(legacy, 10), analyzer.parse(prefixed, 10))

    def test_session_does_not_complete_previous_partial_cycle(self):
        data = b'00005.0 ESP-NOW exchange phase started\nlogger-session host=x\n00010.0 deep sleep 20 sec\n'
        self.assertEqual(analyzer.parse(data, 10), [])

    def test_erase_failure_prevents_upload_and_logger(self):
        with patch.object(deploy.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'erase')) as run, patch.object(deploy, 'start_screen') as start:
            with self.assertRaises(subprocess.CalledProcessError):
                deploy.deploy(deploy.UsbSession('esp.usb1', 1), Path('/project'),
                              False, True, Path('/tools/esptool'))
            self.assertEqual(run.call_count, 1)
            self.assertIn('erase_flash', run.call_args.args[0])
            start.assert_not_called()

    def test_fresh_screen_name_does_not_reuse_pid(self):
        with patch.object(deploy.subprocess, 'run') as run:
            deploy.start_screen(deploy.UsbSession('123.esp.usb1', 1), 'logger', False)
            self.assertEqual(run.call_args.args[0][2], 'esp.usb1')

    def test_upload_uses_prebuilt_artifacts(self):
        with patch.object(deploy.subprocess, 'run') as run, \
             patch.object(deploy, 'start_logger'):
            deploy.deploy(deploy.UsbSession('esp.usb2', 2), Path('/project'),
                          False, False, Path('/tools/esptool'))
            command = run.call_args.args[0]
            self.assertIn('upload-only', command)
            self.assertNotIn('upload', command)

    def test_parallel_deploy_visits_every_board(self):
        sessions = [deploy.UsbSession(f'esp.usb{i}', i) for i in range(3)]
        with patch.object(deploy, 'upload_command_template',
                          return_value=['esptool', '--port', '__UPLOAD_PORT__']), \
             patch.object(deploy, 'deploy') as upload:
            deploy.deploy_parallel(sessions, Path('/project'), False, False,
                                   Path('/tools/esptool'), 2)
        self.assertEqual({call.args[0].index for call in upload.call_args_list},
                         {0, 1, 2})
        self.assertTrue(all(call.args[5] for call in upload.call_args_list))


if __name__ == '__main__':
    unittest.main()
