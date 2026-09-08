// Host-only replay of logged clock pairs through the production planner.
// Build: g++ -std=c++17 -I. scripts/replay_interval_planner.cpp -o /tmp/planner-replay
// Input: serial log lines on stdin; no hardware access or state changes.
#include <cstdio>
#include <iostream>
#include <string>
#include "rendezvousPlanner.h"

int main() {
    std::string line;
    unsigned observations=0, partial=0, invalid=0;
    while (std::getline(std::cin,line)) {
        const auto pos=line.find("beacon-clock-observation target ");
        if(pos==std::string::npos) continue;
        unsigned long long bssid,tsf,received,start,end,now;
        unsigned valid;
        if(std::sscanf(line.c_str()+pos,
            "beacon-clock-observation target %llx tsf %llu local-rx %llu exchange-start-local %llu planned-end-local %llu completion-local %llu valid %u",
            &bssid,&tsf,&received,&start,&end,&now,&valid)!=7 || !valid) continue;
        ++observations;
        RendezvousPlanner::Appointment first,second;
        if(!RendezvousPlanner::nextAppointment(bssid,tsf,received,now,
            30000000,5000000,5000000,true,first) ||
           !RendezvousPlanner::nextAppointment(bssid,tsf,received,first.end,
            30000000,5000000,5000000,true,second)) { ++invalid; continue; }
        RendezvousPlanner::Plan<4> plan(1000000);
        if(!plan.addHome(first) || !plan.addHome(second) || first.start<now ||
           second.start<first.end || first.end-first.start>5000000 ||
           second.end-second.start!=5000000 || second.late) { ++invalid; continue; }
        partial+=first.late;
    }
    std::cout<<"observations="<<observations<<" partial-current="<<partial
             <<" invalid="<<invalid<<" (30s period, 5s phase, 5s exchange)\n";
    return observations && !invalid ? 0 : 1;
}
