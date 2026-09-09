"""Host-only regression tests; never opens a serial port or runs screen."""
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

import analyze_rendezvous as analyzer
import deploy_usb_screens as deploy


class InfrastructureTests(unittest.TestCase):
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
