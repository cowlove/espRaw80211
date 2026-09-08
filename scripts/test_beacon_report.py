"""Wire-format and timing regression checks, without serial/network access."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class BeaconReportTests(unittest.TestCase):
    def test_wire_roundtrip_bounds_and_framing(self):
        source = r'''
#include <cassert>
#include <cstring>
#include "beaconReport.h"
int main() {
    BeaconReportHeader h = {};
    h.version = 5;
    h.incarnation = 0x12345678;
    h.claimCount = 3;
    h.associationCount = 4;
    assert(sizeof(h) + 3*sizeof(BeaconClaimEntry) +
           4*sizeof(BeaconAssociationEntry) + 4 == 199);
    assert(validReportLength(h, 195));
    assert(!validReportLength(h, 194));
    assert(!validReportLength(h, 196));
    h.claimCount = 4;
    assert(!validReportLength(h, 216));
    h.claimCount = 3;
    h.associationCount = 5;
    assert(!validReportLength(h, 217));
    h.associationCount = 4;
    h.version = 4;
    assert(!validReportLength(h, 195));
    h.version = 5;
    assert(setReportTiming(h, 0x60a4b792da8aULL, 100000000,
                           7000000, 6000000, 11000000));
    assert(h.clockMsLow == 100000);
    assert(h.exchangeStartDeltaMs == -1000);
    assert(h.plannedEndDeltaMs == 4000);
    assert(reportClockBssid(h) == 0x60a4b792da8aULL);
    unsigned char wire[sizeof(h)];
    memcpy(wire, &h, sizeof(h));
    assert(wire[25] == 0x78 && wire[28] == 0x12);
    assert(wire[29] == 0x60 && wire[34] == 0x8a);
    BeaconReportHeader copy;
    memcpy(&copy, wire, sizeof(copy));
    assert(copy.incarnation == h.incarnation);
    assert(copy.exchangeStartDeltaMs == -1000);
    // Low-millisecond rollover preserves bounded signed interval deltas.
    const uint64_t wrapped = ((uint64_t)UINT32_MAX + 2) * 1000;
    assert(setReportTiming(h, 1, wrapped, 2000000, 1000000, 3000000));
    assert(h.clockMsLow == 1 && h.exchangeStartDeltaMs == -1000);
    assert(h.plannedEndDeltaMs == 1000);
    int16_t d;
    assert(reportDeltaMs(32767000, 0, d) && d == 32767);
    assert(!reportDeltaMs(32768000, 0, d));
    assert(reportDeltaMs(0, 32768000, d) && d == -32768);
    assert(!reportDeltaMs(0, 32769000, d));
    assert(!setReportTiming(h, 0, 100000000, 1, 1, 2));
    assert(h.timingValid == 0);
    assert(!setReportTiming(h, 1, 100000000, 1, 1, 40000000));
    assert(h.timingValid == 0);
    assert(!setReportTiming(h, 1, 1, 7000, 1, 8000));
    assert(!setReportTiming(h, 1, 100000000, 1, 3, 2));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(pathlib.Path(directory) / 'beacon-report-test')
            subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT), '-x', 'c++', '-', '-o', binary],
                           input=source, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
