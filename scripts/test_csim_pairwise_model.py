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


if __name__ == '__main__':
    unittest.main()
