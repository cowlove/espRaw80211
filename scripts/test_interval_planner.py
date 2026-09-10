"""Compile the production planner, test invariants and recorded clock inputs."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IntervalPlannerTests(unittest.TestCase):
    def test_replay_reader(self):
        sample = ('2026-09-08T11:00:00Z | 00010.0 beacon-clock-observation target '
                  '60a4b792da8a tsf 13093895552384 local-rx 5007028 '
                  'exchange-start-local 5021603 planned-end-local 10021603 '
                  'completion-local 10021603 valid 1\n')
        with tempfile.TemporaryDirectory() as directory:
            binary = str(pathlib.Path(directory) / 'replay')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT), str(ROOT / 'scripts/replay_interval_planner.cpp'),
                            '-o', binary], check=True)
            result = subprocess.run([binary], input=sample, text=True,
                                    capture_output=True, check=True)
            self.assertIn('observations=1 partial-current=0 invalid=0', result.stdout)

    def test_planning_invariants_and_clock_edges(self):
        source = r'''
#include <cassert>
#include "rendezvousPlanner.h"
using namespace RendezvousPlanner;
int main() {
    Plan<8> p(2);
    assert(p.addHome({10,20,1}));
    assert(p.addHome({40,50,1}));
    assert(p.addScout({18,42,2},20));
    assert(p.intervalCount()==1 && p.interval(0).start==10 && p.interval(0).end==50);
    assert(p.interval(0).appointments==7 && p.awakeUsec()==40);
    assert(!p.addScout({60,61,3},20)); // cumulative budget exhausted
    assert(p.appointmentCount()==3 && p.intervalCount()==1);
    assert(p.sleepUntilNext(0,3)==7);
    assert(p.sleepUntilNext(8,3)==0);
    assert(p.sleepUntilNext(30,0)==0);
    assert(p.sleepUntilNext(50,0)==0); // horizon exhausted: replan
    Plan<8> covered(2);
    assert(covered.addHome({10,20,1}));
    assert(covered.addScout({12,19,2},0));
    assert(!covered.addScout({22,23,3},2)); // bridging gap counts too
    assert(covered.addScout({22,23,3},3));
    assert(covered.intervalCount()==1 && covered.awakeUsec()==13);
    Plan<1> capacity(0);
    assert(capacity.addHome({1,2,1}));
    assert(!capacity.addScout({1,2,2},0) && capacity.valid());
    assert(!capacity.addHome({3,4,1}) && !capacity.valid());
    assert(capacity.sleepUntilNext(0,0)==0);
    Plan<2> ordering(0);
    assert(!ordering.addScout({1,2,2},1)); // scouts cannot stand in for home
    assert(ordering.addHome({1,2,1}));
    assert(ordering.addScout({3,4,2},1));
    assert(!ordering.addHome({5,6,1}) && !ordering.valid());
    Plan<4> adjacent(0);
    assert(adjacent.addHome({10,20,1}));
    assert(adjacent.addHome({20,30,1}));
    assert(adjacent.intervalCount()==1); // touching windows need no sleep
    Plan<16> aggressive(1);
    assert(aggressive.addHome({10,15,1}));
    assert(aggressive.addHome({40,45,1}));
    for(uint64_t i=0;i<8;++i)
        assert(aggressive.addScout({12+i*4,17+i*4,10+i},UINT64_MAX));
    assert(aggressive.appointmentCount()==10);
    assert(aggressive.intervalCount()==1);
    assert(aggressive.interval(0).start==10 && aggressive.interval(0).end==45);
    Plan<2> overflow(UINT64_MAX);
    assert(overflow.addHome({0,1,1}));
    assert(overflow.addHome({UINT64_MAX-1,UINT64_MAX,1}));
    assert(overflow.awakeUsec()==UINT64_MAX);

    uint64_t candidates[]={30,10,20,10,0,40};
    assert(chooseScout(candidates,6,20,0)==10);
    assert(chooseScout(candidates,6,20,10)==30);
    assert(chooseScout(candidates,6,20,30)==40);
    assert(chooseScout(candidates,6,20,40)==10);
    assert(chooseScout(candidates,6,20,25)==30); // removed previous candidate
    assert(chooseScout(nullptr,0,20,0)==0);
    assert(!includeScout(0,0));
    assert(!includeScout(999999,0));
    assert(includeScout(0,1));
    assert(!includeScout(1,1));
    assert(includeScout(499999,500000));
    assert(!includeScout(500000,500000));
    assert(includeScout(UINT32_MAX,1000000));
    uint64_t priority[]={40};
    // Eligible tickets are 30,10,40, with two additional tickets for 40.
    assert(chooseWeightedScout(candidates,6,priority,1,20,0,2)==30);
    assert(chooseWeightedScout(candidates,6,priority,1,20,1,2)==10);
    assert(chooseWeightedScout(candidates,6,priority,1,20,2,2)==40);
    assert(chooseWeightedScout(candidates,6,priority,1,20,3,2)==40);
    assert(chooseWeightedScout(candidates,6,priority,1,20,4,2)==40);
    Appointment a;
    assert(nextAppointment(1,0,0,0,30,5,5,true,a));
    assert(a.start==5 && a.end==10 && !a.late);
    assert(nextAppointment(1,0,0,7,30,5,5,true,a));
    assert(a.start==7 && a.end==10 && a.late);
    assert(nextAppointment(1,0,0,10,30,5,5,true,a));
    assert(a.start==35 && a.end==40 && !a.late);
    // A shared nonzero phase stays exact in beacon-clock coordinates.
    assert(nextAppointment(1,30000000,0,4500000,30000000,5000000,5000000,true,a));
    assert(a.start==5000000 && a.end==10000000 && !a.late);
    assert(nextAppointment(1,32,0,0,30,29,5,true,a));
    assert(a.start==0 && a.end==2 && a.late); // previous cycle crosses boundary
    assert(nextAppointment(1,2,0,0,30,29,5,true,a));
    assert(a.start==27 && !a.late); // no fictitious negative TSF cycle
    assert(!nextAppointment(1,UINT64_MAX,0,1,30,5,5,true,a));
    assert(!nextAppointment(1,0,1,0,30,5,5,true,a));
    assert(!nextAppointment(1,0,0,0,0,5,5,true,a));
    assert(!nextAppointment(1,0,0,0,30,30,5,true,a));
    assert(!nextAppointment(1,0,0,UINT64_MAX-1,30,5,5,true,a));
    // Recorded local USB0 target observation, wake 11. Its current window
    // ends exactly at completion; next appointment is a future TSF slot.
    assert(nextAppointment(0x60a4b792da8aULL,13093895552384ULL,5007028,
                           10021603,30000000,5000000,5000000,true,a));
    assert(a.start==34454644 && a.end==39454644 && !a.late);
    // Multi-hour production period: no assumptions about a 30-second test.
    assert(nextAppointment(1,0,0,0,10800000000ULL,5000000,5000000,true,a));
    assert(a.start==5000000 && a.end==10000000);

    // Deterministic property sweep: all accepted appointments remain covered
    // regardless of input order/overlap, optional rollback and merge threshold.
    uint32_t random=17;
    for(unsigned trial=0;trial<1000;++trial) {
        Plan<16> plan(trial%7);
        for(unsigned i=0;i<8;++i) {
            random=random*1664525U+1013904223U;
            uint64_t start=random%1000;
            assert(plan.addHome({start,start+1+random%40,1}));
        }
        uint64_t homeCost=plan.awakeUsec();
        for(unsigned i=0;i<8;++i) {
            random=random*1664525U+1013904223U;
            uint64_t start=random%1000;
            plan.addScout({start,start+1+random%40,2},100);
        }
        assert(plan.valid() && plan.awakeUsec()<=homeCost+100);
        for(size_t i=0;i<plan.appointmentCount();++i) {
            bool found=false;
            for(size_t j=0;j<plan.intervalCount();++j) {
                const auto &w=plan.interval(j);
                if(w.appointments & (1ULL<<i)) {
                    assert(w.start<=plan.appointment(i).start);
                    assert(w.end>=plan.appointment(i).end);
                    found=true;
                }
                if(j) assert(plan.interval(j-1).end<w.start);
            }
            assert(found);
        }
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(pathlib.Path(directory) / 'planner-test')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-I', str(ROOT), '-x', 'c++',
                            '-', '-o', binary], input=source, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
