"""Compile actual firmware merge code with a host clock stub; no hardware IO."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class TimingMergeTests(unittest.TestCase):
    def test_projection_and_merge_decisions(self):
        sketch = (ROOT / 'espRaw80211.ino').read_text()
        merge = sketch.split('    bool mergeAssociation(', 1)[1].split(
            '    size_t listenerCount(', 1)[0]
        source = r'''
#include <cassert>
#include <stddef.h>
#include "rendezvousTiming.h"
uint64_t micros() { return 123; }
struct BeaconAssociation {
    uint64_t originMac, selectedBeacon;
    uint32_t originGeneration, ageCycles;
    uint64_t received;
};
struct Harness {
    static constexpr unsigned associationTableSize = 2;
    BeaconAssociation associations[2] = {};
    uint32_t associationMergeAttempts=0, associationMergeAccepted=0,
        associationMergeInvalid=0, associationMergeOlder=0,
        associationMergeNotFresher=0, associationMergeFull=0;
    uint32_t associationAgeCycles(const BeaconAssociation &a) { return a.ageCycles; }
    bool mergeAssociation(''' + merge + r'''
};
int main() {
    uint64_t projected;
    // Nonzero setup time; observation falls after exchange start.
    assert(projectBeaconTsf(100000000, 7000000, 6000000, projected));
    assert(projected == 99000000);
    assert(projectBeaconTsf(100000000, 7000000, 11000000, projected));
    assert(projected == 104000000);
    assert(projectBeaconTsf(100, 7, 7, projected) && projected == 100);
    assert(!projectBeaconTsf(1, 7, 5, projected));
    assert(!projectBeaconTsf(UINT64_MAX, 7, 8, projected));
    Harness h;
    assert(!h.mergeAssociation(0, 10, 5, 0));
    assert(!h.mergeAssociation(1, 0, 5, 0));
    assert(h.mergeAssociation(1, 10, 5, 3));
    assert(h.associations[0].ageCycles == 4);
    assert(!h.mergeAssociation(1, 20, 4, 0, true));
    assert(h.associations[0].selectedBeacon == 10);
    assert(!h.mergeAssociation(1, 20, 5, 4));
    assert(h.mergeAssociation(1, 10, 5, 1));
    assert(h.associations[0].ageCycles == 2);
    assert(h.mergeAssociation(1, 20, 6, 0, true));
    assert(h.associations[0].selectedBeacon == 20);
    assert(h.associations[0].ageCycles == 0);
    assert(h.mergeAssociation(1, 20, 6, 0, true));
    assert(h.mergeAssociation(2, 10, 1, UINT32_MAX));
    assert(h.associations[1].ageCycles == UINT32_MAX);
    assert(!h.mergeAssociation(3, 10, 1, 0));
    assert(h.associationMergeAccepted == 5);
    assert(h.associationMergeInvalid == 2);
    assert(h.associationMergeOlder == 1);
    assert(h.associationMergeNotFresher == 1);
    assert(h.associationMergeFull == 1);
    assert(h.associationMergeAttempts == 10);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(pathlib.Path(directory) / 'timing-merge-test')
            subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT), '-x', 'c++', '-', '-o', binary],
                           input=source, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
