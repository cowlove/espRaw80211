import unittest
import rendezvous_evidence as evidence
import analyze_rendezvous as analyzer
import contextlib
import io
import tempfile
from pathlib import Path


def line(body, session='s1', second=0):
    return (f'2026-09-08T10:00:{second:02d}+00:00 host_mono_ns=1 '
            f'board=usb0 port=/dev/ttyUSB0 session={session} | {body}\n').encode()


def cycle(epoch='aa', wake=1, bssid='abc', start=1000000, end=6000000,
          second=0, extra=b'', session='s1', kind='home', end_second=None,
          wire=6):
    identity_suffix = ' exchange 1' if wire >= 7 else ''
    return b''.join([
        line(f'00000.1 report-identity incarnation {epoch} wake {wake} wire-version {wire}{identity_suffix}', session, second),
        line('00005.0 ESP-NOW exchange phase started', session, second),
        extra,
        line(f'00010.0 beacon-clock target {bssid} tsf-packet 100 exchange {start}-{end}', session, second),
        line(f'00010.0 appointment complete exchange 1 kind {kind} target {bssid} full 1 healthy 1', session, second),
        line('00010.1 gossip home exchange incomplete home abc listeners 1 rawrx 0 rx 0 valid 0', session, second),
        line('00010.2 association-merge attempts 9 accepted 3 rejected 6 invalid 0 older-generation 1 not-fresher 5 table-full 0', session, second),
        line('00010.3 deep sleep 20 sec', session,
             second if end_second is None else end_second)])


class EvidenceTests(unittest.TestCase):
    def test_rendezvous_status_shows_current_consensus_progress(self):
        data = cycle(extra=line('00009.9 test consensus 7/10'))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            analyzer.rendezvous_dashboard('usb0', data)
        self.assertIn('latest-consensus=7/10', output.getvalue())
        self.assertIn('reset=-', output.getvalue())

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

    def test_all_sessions_without_time_filter_is_zero_copy(self):
        data = cycle()
        selected, warnings = evidence.select(data, session='all')
        self.assertIs(selected, data)
        self.assertEqual(warnings, [])

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

    def test_concatenated_tx_does_not_overwrite_rx_incarnation(self):
        extra = line(
            '00006.0 report-clock-rx sender aaa incarnation a1 wake 1 packet 0 '
            'local-rx 6000000 bssid abc valid 1 exchange 1report-clock-tx '
            'incarnation b2 wake 2 packet 0 bssid abc')
        cycles, _ = evidence.parse_evidence(cycle(epoch='b2', extra=extra, wire=7))
        self.assertEqual(cycles[0].received_clocks[0]['sender'], 'aaa')
        self.assertEqual(cycles[0].received_clocks[0]['incarnation'], 'a1')

    def test_received_clock_does_not_replace_exchange_identity(self):
        extra = (line(
            '00006.0 report-clock-rx sender aaa incarnation a1 wake 1 packet 0 '
            'local-rx 6000000 bssid abc valid 1 exchange 1') +
            line('00007.0 ESP-NOW exchange phase started'))
        cycles, partial = evidence.parse_evidence(
            cycle(epoch='b2', extra=extra, wire=7))
        self.assertEqual(len(cycles), 1)
        self.assertEqual(partial, 1)
        self.assertEqual(cycles[0].epoch, 'b2')

    def test_scout_pairwise_bandwidth_excludes_home_only(self):
        # Incarnations identify each board; clock samples map its wire origin.
        a_extra = (line('00006.0 report-clock-rx sender bbb incarnation bb wake 1 packet 0 local-rx 6000000 bssid abc valid 1 exchange 1') +
                   line('00009.0 espnow summary origin bbb radio-from bbb frames 8 valid 7 short 1'))
        b_extra = (line('00006.0 report-clock-rx sender aaa incarnation aa wake 1 packet 0 local-rx 6000000 bssid abc valid 1 exchange 1') +
                   line('00009.0 espnow summary origin aaa radio-from aaa frames 6 valid 5 short 1'))
        scout_rows = analyzer.scout_link_stats([
            ('a', cycle(epoch='aa', extra=a_extra, kind='scout', end_second=5, wire=7)),
            ('b', cycle(epoch='bb', extra=b_extra, kind='home', end_second=5, wire=7)),
        ])
        self.assertEqual(len(scout_rows), 1)
        self.assertEqual(scout_rows[0]['left_received_from_right']['valid_packets'], 7)
        self.assertEqual(scout_rows[0]['right_received_from_left']['valid_packets'], 5)
        home_rows = analyzer.scout_link_stats([
            ('a', cycle(epoch='aa', extra=a_extra, end_second=5, wire=7)),
            ('b', cycle(epoch='bb', extra=b_extra, end_second=5, wire=7)),
        ])
        self.assertEqual(home_rows, [])
        steady_rows = analyzer.pairwise_link_stats([
            ('a', cycle(epoch='aa', extra=a_extra, end_second=5, wire=7)),
            ('b', cycle(epoch='bb', extra=b_extra, end_second=5, wire=7)),
        ], 'home')
        self.assertEqual(len(steady_rows), 1)
        self.assertEqual(steady_rows[0]['left_received_from_right']['valid_packets'], 7)
        self.assertAlmostEqual(
            steady_rows[0]['left_received_from_right']['valid_per_overlap_second'],
            7 / 5)
        combined_rows = analyzer.pairwise_link_stats([
            ('a', cycle(epoch='aa', extra=a_extra, kind='scout', end_second=5, wire=7)),
            ('b', cycle(epoch='bb', extra=b_extra, kind='home', end_second=5, wire=7)),
        ], 'all')
        self.assertEqual(len(combined_rows), 1)
        legacy_rows = analyzer.scout_link_stats([
            ('a', cycle(epoch='aa', extra=a_extra, kind='scout', end_second=5)),
            ('b', cycle(epoch='bb', extra=b_extra, kind='home', end_second=5)),
        ])
        self.assertEqual(legacy_rows, [])

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            analyzer.print_pairwise_ascii_table(scout_rows, 'scout', ['a', 'b', 'c'])
        table = output.getvalue()
        self.assertIn('valid packets/second matrix', table)
        self.assertIn('healthy overlap sessions matrix', table)
        self.assertIn('rows receive from columns', table)
        self.assertIn('1.40', table)  # a receives 7 packets during 5 seconds
        self.assertIn('1.00', table)  # b receives 5 packets during 5 seconds
        self.assertIn('100%', table)
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / 'links.h'
            scan = line('00010.0 matrix scan-observed beacon abc packets 25 '
                        'span 5.000 sec maxgap 0.2 sec rssi avg -60 min -65 '
                        'max -55 eligible yes')
            parsed = {
                'a': evidence.parse_evidence(cycle(epoch='aa', extra=scan, wire=7)),
                'b': evidence.parse_evidence(cycle(epoch='bb', extra=scan, wire=7)),
            }
            analyzer.write_csim_pairwise_header(
                header, combined_rows, ['a', 'b'], parsed)
            generated = header.read_text()
            self.assertIn('boardCount = 2', generated)
            self.assertIn('packetsPerSecond', generated)
            self.assertIn('healthyWindowPercent', generated)
            self.assertIn('BeaconEnvironment', generated)
            self.assertIn('{0xabcULL, -60.000f, 0.000f, 5.000000f', generated)
            with self.assertRaises(ValueError):
                analyzer.write_csim_pairwise_header(header, combined_rows,
                                                    ['a', 'b', 'unsampled'])

    def test_historical_usb_swaps_remap_to_current_alias(self):
        def observed(epoch, sender):
            return line(f'00006.0 report-clock-rx sender {sender} incarnation {epoch} '
                        'wake 1 packet 0 local-rx 6000000 bssid abc valid 1 exchange 1')

        # USB labels swapped between the old and current captures. Peer clock
        # records provide the stable MAC for every incarnation.
        streams = {
            'usb0': evidence.parse_evidence(
                cycle(epoch='b1', second=0, wire=7) +
                cycle(epoch='a2', second=20, wire=7))[0],
            'usb1': evidence.parse_evidence(
                cycle(epoch='a1', second=0, wire=7) +
                cycle(epoch='b2', second=20, wire=7))[0],
            'observer': evidence.parse_evidence(cycle(
                epoch='observer', second=40, wire=7,
                extra=(observed('a1', 'aaa') + observed('a2', 'aaa') +
                       observed('b1', 'bbb') + observed('b2', 'bbb'))))[0],
        }
        remapped, origins = analyzer.remap_cycles_by_current_identity(streams)
        self.assertEqual([cycle.epoch for cycle in remapped['usb0']], ['a2', 'a1'])
        self.assertEqual([cycle.epoch for cycle in remapped['usb1']], ['b1', 'b2'])
        self.assertEqual(origins['usb0'], {'aaa'})
        self.assertEqual(origins['usb1'], {'bbb'})


if __name__ == '__main__':
    unittest.main()
