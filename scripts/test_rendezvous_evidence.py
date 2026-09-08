import unittest
import rendezvous_evidence as evidence
import analyze_rendezvous as analyzer
import contextlib
import io


def line(body, session='s1', second=0):
    return (f'2026-09-08T10:00:{second:02d}+00:00 host_mono_ns=1 '
            f'board=usb0 port=/dev/ttyUSB0 session={session} | {body}\n').encode()


def cycle(epoch='aa', wake=1, bssid='abc', start=1000000, end=6000000,
          second=0, extra=b'', session='s1', kind='home', end_second=None):
    return b''.join([
        line(f'00000.1 report-identity incarnation {epoch} wake {wake} wire-version 6', session, second),
        line('00005.0 ESP-NOW exchange phase started', session, second),
        extra,
        line(f'00010.0 beacon-clock target {bssid} tsf-packet 100 exchange {start}-{end}', session, second),
        line(f'00010.0 appointment complete exchange 1 kind {kind} target {bssid} full 1 healthy 1', session, second),
        line('00010.1 gossip home exchange incomplete home abc listeners 1 rawrx 0 rx 0 valid 0', session, second),
        line('00010.2 association-merge attempts 9 accepted 3 rejected 6 invalid 0 older-generation 1 not-fresher 5 table-full 0', session, second),
        line('00010.3 deep sleep 20 sec', session,
             second if end_second is None else end_second)])


class EvidenceTests(unittest.TestCase):
    def test_v7_multiple_exchanges_in_one_round(self):
        def exchange(sequence, extra=b''):
            return cycle(epoch='bb', wake=2, extra=extra).replace(
                b'wire-version 6', f'wire-version 7 exchange {sequence}'.encode()
            ).replace(b'deep sleep 20 sec', f'exchange complete interval {sequence}'.encode())
        data = exchange(10) + exchange(11)
        parsed, partial = evidence.parse_evidence(data)
        self.assertEqual([c.sequence for c in parsed], [10, 11])
        self.assertEqual(partial, 0)
        extra = line('00006.0 report-clock-rx sender 123 incarnation bb wake 2 packet 0 local-rx 6000000 bssid abc valid 1 exchange 10')
        receiver = cycle(extra=extra).replace(b'rawrx 0 rx 0 valid 0', b'rawrx 8 rx 8 valid 8')
        rows = evidence.overlaps([('a', receiver), ('b', data)])
        self.assertEqual(rows[0]['left_received_right'], 'report-observed')
        self.assertIn('unknown', rows[1]['left_received_right'])

    def test_convergence_cannot_span_logger_restart(self):
        data = (b'00005.0 ESP-NOW exchange phase started\n00010.0 deep sleep 20 sec\n'
                b'00010.1 TEST RESET EXECUTED\nlogger-session host=x\n'
                b'00005.0 ESP-NOW exchange phase started\n00010.0 deep sleep 20 sec\n'
                b'00010.1 test consensus 10/10\n')
        with contextlib.redirect_stdout(io.StringIO()):
            episodes, _ = analyzer.convergence_dashboard('test', data)
        self.assertEqual(episodes, [])

    def test_session_selection_and_partial_boundaries(self):
        data = line('logger-session host=x') + cycle()
        data += line('00005.0 ESP-NOW exchange phase started')
        data += line('logger-session host=x', 's2')
        data += line('00010.0 deep sleep 20 sec', 's2')
        selected, warnings = evidence.select(data)
        self.assertFalse(warnings)
        self.assertEqual(evidence.summarize(selected)['complete_cycles'], 0)
        self.assertEqual(evidence.summarize(data)['complete_cycles'], 1)
        self.assertEqual(evidence.summarize(data)['partial_cycles'], 1)

    def test_time_selection_and_tail_warning(self):
        data = cycle(second=10)
        selected, warnings = evidence.select(data, since=evidence.timestamp('2026-09-08T10:00:11Z'))
        self.assertTrue(warnings)
        self.assertEqual(evidence.summarize(selected)['complete_cycles'], 0)
        with self.assertRaises(ValueError):
            evidence.timestamp('2026-09-08T10:00:11')

    def test_summary_changes_and_merges(self):
        summary = evidence.summarize(cycle() + cycle(wake=2) + cycle(epoch='bb'))
        self.assertEqual(summary['observed_incarnation_changes'], 1)
        self.assertEqual(summary['zero_rawrx_cycles'], 3)
        self.assertEqual(summary['merge']['accepted'], 9)
        self.assertEqual(summary['latest']['wake'], 1)

    def test_overlap_is_half_open_and_same_bssid(self):
        a = cycle()
        self.assertFalse(evidence.overlaps([('a', a), ('b', cycle(start=6000000, end=7000000))]))
        self.assertFalse(evidence.overlaps([('a', a), ('b', cycle(bssid='def'))]))
        self.assertFalse(evidence.overlaps([('a', a), ('b', cycle(second=30))]))
        rows = evidence.overlaps([('a', a), ('b', cycle(start=5000000, end=9000000))])
        self.assertEqual(rows[0]['overlap_ms'], 1000)
        self.assertEqual(rows[0]['left_received_right'], 'no-raw-callbacks')

    def test_explicit_packet_identity_and_unknown_absence(self):
        extra = line('00006.0 report-clock-rx sender 123 incarnation bb wake 2 packet 0 local-rx 6000000 bssid abc clock-ms-low 1000 start-delta-ms 0 planned-end-delta-ms 5000 valid 1')
        a = cycle(extra=extra).replace(b'rawrx 0 rx 0 valid 0', b'rawrx 8 rx 8 valid 8')
        rows = evidence.overlaps([('a', a), ('b', cycle(epoch='bb', wake=2))])
        self.assertEqual(rows[0]['left_received_right'], 'report-observed')
        rows = evidence.overlaps([('a', a), ('b', cycle(epoch='bb', wake=3))])
        self.assertIn('unknown', rows[0]['left_received_right'])

    def test_old_firmware_windows_not_trusted(self):
        old = cycle().replace(b'wire-version 6', b'wire-version 4')
        self.assertFalse(evidence.overlaps([('a', old), ('b', cycle())]))

    def test_binary_noise_and_peer_mapping(self):
        extra = line('00010.1 espnow summary origin 123 radio-from 321 frames 8 valid 7 short 0')
        data = cycle(extra=extra).replace(b'00010.1', b'\x0000010.1')
        cycles, _ = evidence.parse_evidence(data)
        self.assertEqual(cycles[0].peers['123']['radio-from'], '321')

    def test_scout_pairwise_bandwidth_excludes_home_only(self):
        # Incarnations identify each board; clock samples map its wire origin.
        a_extra = (line('00006.0 report-clock-rx sender bbb incarnation bb wake 1 packet 0 local-rx 6000000 bssid abc valid 1 exchange 1') +
                   line('00009.0 espnow summary origin bbb radio-from bbb frames 8 valid 7 short 1'))
        b_extra = (line('00006.0 report-clock-rx sender aaa incarnation aa wake 1 packet 0 local-rx 6000000 bssid abc valid 1 exchange 1') +
                   line('00009.0 espnow summary origin aaa radio-from aaa frames 6 valid 5 short 1'))
        scout_rows = analyzer.scout_link_stats([
            ('a', cycle(epoch='aa', extra=a_extra, kind='scout', end_second=5)),
            ('b', cycle(epoch='bb', extra=b_extra, kind='home', end_second=5)),
        ])
        self.assertEqual(len(scout_rows), 1)
        self.assertEqual(scout_rows[0]['left_received_from_right']['valid_packets'], 7)
        self.assertEqual(scout_rows[0]['right_received_from_left']['valid_packets'], 5)
        home_rows = analyzer.scout_link_stats([
            ('a', cycle(epoch='aa', extra=a_extra, end_second=5)),
            ('b', cycle(epoch='bb', extra=b_extra, end_second=5)),
        ])
        self.assertEqual(home_rows, [])


if __name__ == '__main__':
    unittest.main()
