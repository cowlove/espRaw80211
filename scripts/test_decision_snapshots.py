import unittest

from decision_snapshots import parse_lines


SNAPSHOT = (
    "board 1.0 @v v=1 e=1234 w=2 x=3 i=4 o=0 k=1 src=direct "
    "h=000000000020 hm=3 t=000000000030 tm=4 pb=000000000000 "
    "ph=000000000000 pm=0 pa=0 tv=0 n=1 q=570de08f77c2a84f "
    "actual=propose replay=propose"
)
TABLE = "board 1.0 @vt v=1 e=1234 w=2 x=3 t=0 n=1 q=570de08f77c2a84f"
ROW = (
    "board 1.0 @vrow e=1234 w=2 x=3 t=0 r=0 m=000000000001 "
    "b=000000000020 g=2 a=0 z=00000005"
)
END = "board 1.0 @vend e=1234 w=2 x=3 tables=1 decisions=1 overflow=0"


class DecisionSnapshotParserTests(unittest.TestCase):
    def test_complete_snapshot(self):
        parsed = parse_lines([TABLE, ROW, SNAPSHOT, END])
        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0].fields["actual"], "propose")

    def test_truncated_snapshot_rejected(self):
        with self.assertRaisesRegex(ValueError, "association rows"):
            parse_lines([TABLE, SNAPSHOT, END])

    def test_replay_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, "decision mismatch"):
            parse_lines([TABLE, ROW,
                         SNAPSHOT.replace("replay=propose", "replay=refresh"), END])

    def test_unknown_version_rejected(self):
        with self.assertRaisesRegex(ValueError, "unsupported snapshot version"):
            parse_lines([TABLE, ROW, SNAPSHOT.replace("@v v=1", "@v v=2"), END])

    def test_overflow_rejected(self):
        with self.assertRaisesRegex(ValueError, "decision trace overflow"):
            parse_lines([TABLE, ROW, SNAPSHOT, END.replace("overflow=0", "overflow=1")])

    def test_end_count_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, "decision count mismatch"):
            parse_lines([TABLE, ROW, SNAPSHOT,
                         END.replace("decisions=1", "decisions=2")])


if __name__ == "__main__":
    unittest.main()
