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
using SingletonJoinPolicy::mayAdopt;
int main() {
    assert(!mayAdopt(0, 2));
    assert(!mayAdopt(1, 0));
    assert(!mayAdopt(1, 1));
    assert(mayAdopt(1, 2));
    assert(mayAdopt(1, 7));
    assert(!mayAdopt(2, 7)); // relaxed path stops after joining a group
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
