import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ExecutorTests(unittest.TestCase):
    def test_logical_rounds_and_coverage(self):
        source = r'''
#include <cassert>
#include "rendezvousExecutor.h"
using namespace RendezvousExecutor;
int main() {
    RoundClock a{0,100};
    assert(a.tick(105,30)==0 && a.remainder==5);
    assert(a.tick(125,30)==0 && a.remainder==25);
    assert(a.tick(130,30)==1 && a.remainder==0);
    assert(a.tick(191,30)==2 && a.remainder==1);
    // Sleep spanning a boundary, followed by a fresh boot at local zero.
    RoundClock reboot{a.remainder+40,0};
    assert(reboot.tick(0,30)==1 && reboot.remainder==11);
    assert(reboot.tick(19,30)==1 && reboot.remainder==0);
    RoundClock continuous{0,0};
    unsigned total=0;
    for(unsigned i=1;i<=300;++i) total+=continuous.tick(i,30);
    assert(total==10);
    assert(age(UINT32_MAX-1,10)==UINT32_MAX);
    assert(age(3,2)==5);
    Coverage c;
    c.begin(100,100,false,1000,500,20);
    assert(c.healthy(1020,503));
    assert(!c.healthy(1019,503));
    assert(!c.healthy(1020,502));
    Coverage late;
    late.begin(130,100,false,0,0,20);
    assert(!late.healthy(100,100));
    Coverage partial;
    partial.begin(100,100,true,0,0,20);
    assert(!partial.healthy(100,100));
    // Independent overlapping appointments take independent snapshots.
    Coverage scout;
    scout.begin(200,200,false,1010,502,20);
    assert(c.healthy(1020,503) && !scout.healthy(1020,503));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(pathlib.Path(directory) / 'executor')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-I', str(ROOT), '-x', 'c++',
                            '-', '-o', binary], input=source, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
