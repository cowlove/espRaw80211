import unittest

from decision_snapshots import parse_lines


SNAPSHOT = (
    "board 1.0 @v v=1 e=1234 w=2 x=3 i=4 o=0 k=1 src=direct "
    "h=000000000020 hm=3 t=000000000030 tm=4 pb=000000000000 "
    "ph=000000000000 pm=0 pa=0 n=1 q=570de08f77c2a84f "
    "actual=propose replay=propose"
)
ROW = (
    "board 1.0 @vrow e=1234 w=2 x=3 i=4 o=0 m=000000000001 "
    "b=000000000020 g=2 a=0 z=00000005"
)


class DecisionSnapshotParserTests(unittest.TestCase):
    def test_complete_snapshot(self):
        parsed = parse_lines([SNAPSHOT, ROW])
        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0].fields["actual"], "propose")

    def test_truncated_snapshot_rejected(self):
        with self.assertRaisesRegex(ValueError, "association rows"):
            parse_lines([SNAPSHOT])

    def test_replay_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, "decision mismatch"):
            parse_lines([SNAPSHOT.replace("replay=propose", "replay=refresh"), ROW])

    def test_unknown_version_rejected(self):
        with self.assertRaisesRegex(ValueError, "unsupported snapshot version"):
            parse_lines([SNAPSHOT.replace("@v v=1", "@v v=2"), ROW])


if __name__ == "__main__":
    unittest.main()
