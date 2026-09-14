"""Compile and exercise the appointment-boundary replay model."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class AppointmentDecisionSnapshotTests(unittest.TestCase):
    def test_replay_validation_ordering_and_overflow(self):
        source = r'''
#include <cassert>
#include "appointmentDecisionSnapshot.h"
using namespace AppointmentDecisionSnapshot;

Snapshot observed(uint32_t homeMembers, uint64_t home,
                  uint32_t targetMembers, uint64_t target) {
    Snapshot s;
    s.homeMembers = homeMembers;
    s.homeBssid = home;
    s.targetMembers = targetMembers;
    s.targetBssid = target;
    return s;
}

int main() {
    Snapshot bad;
    assert(replay(bad) == Action::InvalidSnapshot);
    bad = observed(3, 20, 4, 30);
    bad.version = schemaVersion + 1;
    assert(replay(bad) == Action::InvalidSnapshot);
    bad = observed(3, 20, 4, 30);
    bad.associationExpectedRows = 4;
    bad.associationObservedRows = 3;
    assert(replay(bad) == Action::InvalidSnapshot); // truncated row set
    bad.associationObservedRows = 4;
    assert(replay(bad) == Action::InvalidSnapshot); // missing fingerprint
    bad.associationFingerprint = 123;
    assert(replay(bad) == Action::StartProposal);

    assert(replay(observed(1, 20, 1, 10)) == Action::CoalesceSingletons);
    assert(replay(observed(1, 20, 2, 30)) == Action::JoinLargerGroup);
    assert(replay(observed(3, 20, 2, 30)) == Action::RejectNotPreferred);

    Snapshot canceled = observed(3, 20, 2, 30);
    canceled.pendingBssid = 30;
    canceled.pendingHome = 20;
    assert(replay(canceled) == Action::CancelNotPreferred);

    Snapshot refreshed = observed(3, 20, 4, 30);
    refreshed.pendingBssid = 30;
    refreshed.pendingHome = 20;
    assert(replay(refreshed) == Action::RefreshProposal);

    Snapshot weaker = observed(3, 20, 4, 40);
    weaker.pendingBssid = 30;
    weaker.pendingHome = 20;
    weaker.pendingMembers = 4;
    assert(replay(weaker) == Action::RejectWeakerThanPending);
    weaker.targetBssid = 10;
    assert(replay(weaker) == Action::StartProposal);

    Snapshot commit;
    commit.kind = DecisionKind::CommitCheck;
    commit.homeBssid = 20;
    commit.homeMembers = 3;
    commit.pendingBssid = 30;
    commit.pendingHome = 20;
    commit.pendingMembers = 4;
    commit.pendingActRound = 12;
    commit.wakeGeneration = 11;
    assert(replay(commit) == Action::AwaitActivation);
    commit.wakeGeneration = 12;
    assert(replay(commit) == Action::CommitProposal);
    commit.homeMembers = 4;
    assert(replay(commit) == Action::CancelHomeCaughtUp);
    commit.pendingHome = 40;
    assert(replay(commit) == Action::CancelHomeChanged);

    Snapshot first = observed(3, 20, 4, 30);
    first.incarnation = 7;
    first.wakeGeneration = 9;
    first.exchangeSequence = 2;
    first.appointmentIndex = 0;
    Snapshot second = first;
    second.appointmentIndex = 1;
    assert(orderedAfter(second, first));
    assert(!orderedAfter(first, second));
    Snapshot rebooted = second;
    rebooted.incarnation = 8;
    assert(!orderedAfter(rebooted, first));

    Buffer<2> buffer;
    assert(buffer.append(first));
    assert(buffer.append(second));
    assert(!buffer.append(second));
    assert(buffer.size() == 2);
    assert(buffer.overflowed());
    assert(buffer[0].appointmentIndex == 0);
    assert(buffer[1].appointmentIndex == 1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = pathlib.Path(directory) / "test.cpp"
            binary = pathlib.Path(directory) / "test"
            src.write_text(source)
            subprocess.run(
                ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT), str(src), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
