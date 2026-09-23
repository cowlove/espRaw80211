import pathlib
import subprocess
import tempfile
import unittest


class CsimPairwiseModelTests(unittest.TestCase):
    def test_generated_seven_board_model_and_window_gating(self):
        project = pathlib.Path(__file__).resolve().parents[1]
        source = r'''
#define CSIM
#include "csimPairwiseModel.h"
#include <assert.h>
int main() {
    using namespace CsimPairwiseModel;
    assert(boardCount == 7);
    assert(index(firstMac) == 0 && index(firstMac + 6) == 6);
    assert(index(firstMac + 7) == -1);
    assert(receptionScale == 0.60f);
    assert(scaledSuccess(1.0f) > 0.59f && scaledSuccess(1.0f) < 0.61f);
    assert(scaledSuccess(0.0f) == 0.0f);
    assert(scaledSuccess(0.5f) > 0.29f && scaledSuccess(0.5f) < 0.31f);
    receptionScale = 2.0f;
    assert(scaledSuccess(0.75f) == 1.0f);
    receptionScale = 0.60f;

    // Inactive receivers and measured-zero links always drop.
    assert(drop(firstMac + 1, firstMac));
    beginWindow(firstMac + 2, 1);
    assert(drop(firstMac + 5, firstMac + 2));
    endWindow(firstMac + 2);

    // A strong 100%-healthy link still experiences the separate packet gate.
    int delivered = 0;
    for (unsigned window = 1; window <= 1000; ++window) {
        beginWindow(firstMac, window);
        delivered += !drop(firstMac + 1, firstMac);
    }
    assert(delivered > 250 && delivered < 400);

    // The 67%-healthy directed link must produce complete dead windows.
    bool foundDeadWindow = false;
    for (unsigned window = 1; window <= 100 && !foundDeadWindow; ++window) {
        beginWindow(firstMac + 1, window);
        bool allDropped = true;
        for (int packet = 0; packet < 20; ++packet)
            allDropped &= drop(firstMac, firstMac + 1);
        foundDeadWindow = allDropped;
    }
    assert(foundDeadWindow);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = pathlib.Path(directory) / 'model.cpp'
            binary = pathlib.Path(directory) / 'model'
            source_path.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-I', str(project),
                            str(source_path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_twenty_board_model_expands_off_diagonal_links(self):
        project = pathlib.Path(__file__).resolve().parents[1]
        source = r'''
#define CSIM
#define CONTEXT_COUNT 20
#include "csimPairwiseModel.h"
#include <assert.h>
int main() {
    using namespace CsimPairwiseModel;
    assert(boardCount == 20);
    assert(empiricalBoardCount == 7);
    assert(index(firstMac + 19) == 19);
    assert(index(firstMac + 20) == -1);
    for (size_t receiver = 0; receiver < boardCount; ++receiver) {
        for (size_t sender = 0; sender < boardCount; ++sender) {
            EmpiricalLink link = empiricalLink(receiver, sender);
            assert(link.receiver < empiricalBoardCount);
            assert(link.sender < empiricalBoardCount);
            if (receiver != sender) assert(link.receiver != link.sender);
        }
    }

    // The positive-link graph must be strongly connected; otherwise the
    // expander itself has manufactured a permanently isolated class.
    for (size_t start = 0; start < boardCount; ++start) {
        bool reached[boardCount] = {};
        reached[start] = true;
        for (size_t pass = 0; pass < boardCount; ++pass)
            for (size_t receiver = 0; receiver < boardCount; ++receiver)
                for (size_t sender = 0; sender < boardCount; ++sender)
                    if (reached[sender] &&
                        packetsPerSecond(receiver, sender) > 0 &&
                        healthyWindowPercent(receiver, sender) > 0)
                        reached[receiver] = true;
        for (bool value : reached) assert(value);
    }

    // Logical devices that share an empirical row still retain independent
    // receiver state and packet counters.
    beginWindow(firstMac, 10);
    beginWindow(firstMac + 7, 11);
    assert(states[0].active && states[7].active);
    assert(states[0].window == 10 && states[7].window == 11);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = pathlib.Path(directory) / 'model20.cpp'
            binary = pathlib.Path(directory) / 'model20'
            source_path.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-I', str(project),
                            str(source_path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
