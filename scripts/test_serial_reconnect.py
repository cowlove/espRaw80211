"""Exercise actual serial hangup/reconnect through Linux pseudo terminals."""
import os
from pathlib import Path
import pty
import subprocess
import sys
import tempfile
import time
import unittest


class ReconnectTests(unittest.TestCase):
    def test_reconnect_append_and_session_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            port, logfile = root / 'port', root / 'log'
            logfile.write_text('existing log\n')
            master, slave = pty.openpty()
            port.symlink_to(os.ttyname(slave))
            process = subprocess.Popen([sys.executable,
                str(Path(__file__).with_name('timestamp_serial.py')),
                '--board', 'test', '--port', str(port), '--logfile', str(logfile)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            def wait_for(predicate):
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    if predicate(logfile.read_text()):
                        return
                    self.assertIsNone(process.poll())
                    time.sleep(.05)
                self.fail(logfile.read_text())
            try:
                wait_for(lambda text: 'logger-session' in text)
                os.write(master, b'before\nunfinished')
                wait_for(lambda text: '| before\n' in text)
                os.close(master)
                os.close(slave)
                master, slave = pty.openpty()
                port.unlink()
                port.symlink_to(os.ttyname(slave))
                wait_for(lambda text: text.count('logger-session') == 2)
                os.write(master, b'after\n')
                wait_for(lambda text: '| after\n' in text)
                text = logfile.read_text()
                self.assertTrue(text.startswith('existing log\n'))
                self.assertNotIn('unfinishedafter', text)
                sessions = [line.split('session=')[1].split()[0]
                            for line in text.splitlines() if 'logger-session' in line]
                self.assertNotEqual(*sessions)
            finally:
                process.terminate()
                process.wait(timeout=5)
                os.close(master)
                os.close(slave)
