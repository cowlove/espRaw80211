"""Read the firmware's test-only oracle, never count USB/log connections."""
from pathlib import Path
import re


def swarm_board_count(path=None):
    path = path or Path(__file__).resolve().parents[1] / 'testSwarmConfig.h'
    match = re.search(r'^#define ARTIFICIAL_TEST_SWARM_BOARD_COUNT ([1-9][0-9]*)$',
                      Path(path).read_text(), re.MULTILINE)
    if not match:
        raise ValueError(f'Missing positive artificial test board count in {path}')
    return int(match[1])
