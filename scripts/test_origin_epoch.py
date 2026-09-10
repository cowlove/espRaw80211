"""Exercise production epoch gating, merges and persistence without hardware."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class OriginEpochTests(unittest.TestCase):
    def test_reset_relays_and_deep_sleep(self):
        sketch = (ROOT / 'espRaw80211.ino').read_text()
        records = 'struct BeaconClaim {' + sketch.split('struct BeaconClaim {', 1)[1].split(
            'struct RemoteBeaconStats', 1)[0]
        methods = 'bool epochMatches(' + sketch.split('    bool epochMatches(', 1)[1].split(
            '    size_t listenerCount(', 1)[0]
        claim = 'bool mergeClaim(' + sketch.split('    bool mergeClaim(', 1)[1].split(
            '    void onBroadScan(', 1)[0]
        source = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "originEpoch.h"
using std::string;
uint64_t micros() { return 123; }
uint64_t steadyMicros() { return micros(); }
template<typename... T> void out(const char *, T...) {}
struct Stored {
    string value;
    string read() const { return value; }
    void operator=(const string &v) { value = v; }
};
''' + records + r'''
struct Harness {
    static constexpr unsigned claimTableSize=8, associationTableSize=4;
    BeaconClaim claims[claimTableSize] = {};
    BeaconAssociation associations[associationTableSize] = {};
    OriginEpoch originEpochs[32] = {};
    uint64_t deviceMac=99, startUsec=10;
    uint32_t incarnation=77, wakeGeneration=5, epochRejected=0;
    uint32_t associationMergeAttempts=0, associationMergeAccepted=0,
        associationMergeInvalid=0, associationMergeOlder=0,
        associationMergeNotFresher=0, associationMergeFull=0;
    Stored spiffsOrigins, spiffsClaims, spiffsAssociations;
''' + methods + claim + r'''
};
int main() {
    Harness h;
    assert(!h.acceptOrigin(1, 0, true));
    assert(h.acceptOrigin(1, 100, true));
    h.mergeClaim(1, 10, 900, -40, 100);
    h.mergeClaim(1, 20, 901, -50, 100);
    assert(h.mergeAssociation(1, 10, 901, 0, true, 100));
    // Unrelated peer must survive another origin's reset.
    assert(h.acceptOrigin(2, 200, false));
    h.mergeClaim(2, 30, 700, -60, 200);
    assert(h.mergeAssociation(2, 30, 700, 0, true, 200));
    // Reset epoch is numerically LOWER: IDs are not generation counters.
    assert(h.acceptOrigin(1, 50, true));
    for (const auto &c : h.claims) assert(c.originMac != 1);
    for (const auto &a : h.associations) assert(a.originMac != 1);
    assert(h.mergeAssociation(1, 20, 1, 0, true, 50));
    h.mergeClaim(1, 20, 1, -41, 50);
    assert(!h.acceptOrigin(1, 100, false)); // old relay cannot roll back
    assert(!h.acceptOrigin(1, 999, false)); // unknown epoch cannot replace
    assert(h.acceptOrigin(1, 50, false));
    assert(!h.mergeAssociation(1, 30, 0, 0, true, 50));
    assert(h.acceptOrigin(99, 77, true));
    assert(!h.acceptOrigin(99, 77, false)); // no relayed self updates
    assert(!h.acceptOrigin(99, 66, true));
    h.saveClaims(); h.saveAssociations(); h.saveOrigins();
    Harness reboot;
    reboot.spiffsOrigins = h.spiffsOrigins.read();
    reboot.spiffsClaims = h.spiffsClaims.read();
    reboot.spiffsAssociations = h.spiffsAssociations.read();
    reboot.loadOrigins(); reboot.loadClaims(); reboot.loadAssociations();
    assert(reboot.epochMatches(1, 50));
    assert(!reboot.acceptOrigin(1, 100, false));
    bool foundOne=false, foundTwo=false;
    for (const auto &a : reboot.associations) {
        if (a.originMac == 1) {
            assert(a.originEpoch == 50 && a.originGeneration == 1);
            assert(a.selectedBeacon == 20 && a.ageCycles == 0); // boot alone does not age evidence
            foundOne=true;
        }
        if (a.originMac == 2) foundTwo=true;
    }
    assert(foundOne && foundTwo);
    // Inconsistent checkpoint records are discarded, never mislabeled.
    reboot.spiffsClaims = string("1,a,900,-40,5,64;");
    reboot.spiffsAssociations = string("1,a,900,0,64;");
    reboot.loadClaims(); reboot.loadAssociations();
    for (const auto &c : reboot.claims) assert(c.originMac != 1);
    for (const auto &a : reboot.associations) assert(a.originMac != 1);
    OriginEpoch full[1] = {};
    assert(acceptEpoch(full, 1, 1, 10, false) == EpochDecision::introduced);
    assert(acceptEpoch(full, 1, 2, 20, true) == EpochDecision::rejected);
    assert(acceptEpoch(full, 1, 1, 11, true) == EpochDecision::replaced);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(pathlib.Path(directory) / 'origin-epoch-test')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-Wno-class-memaccess', '-I', str(ROOT), '-x', 'c++',
                            '-', '-o', binary], input=source, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
