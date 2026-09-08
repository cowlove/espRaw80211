import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class MacTests(unittest.TestCase):
    def test_hardware_and_simulator_order(self):
        source = r'''
#include <cassert>
#include "macIdentity.h"
int main() {
    const uint64_t radio = 0x4022d8ffa354ULL;
#ifdef CSIM
    const uint64_t origin = radio;
#else
    const uint64_t origin = 0x54a3ffd82240ULL;
#endif
    assert(protocolRadioMac(origin) == radio);
    assert(protocolRadioMac(radio) == origin);
    assert(protocolRadioMac(origin) != 0x4022d8ffa355ULL);
    assert(protocolRadioMac(0) == 0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(pathlib.Path(directory) / 'mac-test')
            for defines in ([], ['-DCSIM']):
                subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                                *defines, '-I', str(ROOT), '-x', 'c++', '-', '-o', binary],
                               input=source, text=True, check=True)
                subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
