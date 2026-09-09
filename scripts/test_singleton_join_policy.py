"""Compile and exercise the production singleton adoption rule."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class SingletonJoinPolicyTests(unittest.TestCase):
    def test_only_singleton_directly_joins_observed_group(self):
        source = r'''
#include <cassert>
#include "singletonJoinPolicy.h"
using namespace SingletonJoinPolicy;
int main() {
    assert(!mayEvaluateScout(0));
    assert(mayEvaluateScout(1));
    assert(mayEvaluateScout(20));
    assert(!mayAdopt(0, 2));
    assert(!mayAdopt(1, 0));
    assert(!mayAdopt(1, 1));
    assert(mayAdopt(1, 2));
    assert(mayAdopt(1, 7));
    assert(!mayAdopt(2, 7)); // relaxed path stops after joining a group
    assert(!mayPropose(1, 20, 7, 10)); // singleton path is separate
    assert(mayPropose(2, 20, 2, 10));
    assert(!mayPropose(2, 10, 2, 20));
    assert(mayPropose(3, 20, 3, 10));
    assert(!mayPropose(3, 10, 3, 20));
    assert(mayPropose(4, 20, 4, 10));
    assert(!mayPropose(4, 10, 4, 20));
    assert(!mayPropose(3, 20, 2, 10));
    assert(mayPropose(3, 20, 4, 30));
    assert(groupPreferred(4, 30, 3, 20));
    assert(groupPreferred(3, 10, 3, 20));
    assert(!groupPreferred(3, 20, 3, 10));
    assert(!groupPreferred(3, 20, 3, 20));
    assert(mayCoalesce(1, 1, 20, 10));
    assert(!mayCoalesce(1, 1, 10, 20));
    assert(!mayCoalesce(2, 1, 20, 10));
    assert(!mayCoalesce(1, 2, 20, 10));
    assert(proposalDelay(0)==2);
    assert(proposalDelay(3)==2);
    assert(proposalDelay(4)==3);
    assert(proposalDelay(12)==5);
    assert(proposalDelay(100)==5);
    assert(reinforce(0,true)==1);
    assert(reinforce(12,true)==12);
    assert(reinforce(3,false)==3);
    assert(decay(3)==2 && decay(0)==0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = pathlib.Path(directory) / 'test.cpp'
            binary = pathlib.Path(directory) / 'test'
            src.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT), str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
