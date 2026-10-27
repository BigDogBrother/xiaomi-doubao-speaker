#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include "app/native_lease.hpp"
#include "wakeup/gate.hpp"
using namespace std::chrono_literals;
int main(int argc,char**argv){
  assert(argc==3);
  const std::string helper=argv[1], dir=argv[2];
  setenv("EDGE_TEST_DIR",dir.c_str(),1);
  {
    xiaoai_plus::app::NativeLease lease(helper);
    assert(lease.Acquire()); assert(lease.Healthy());assert(!lease.Acquire());
    std::this_thread::sleep_for(1800ms);assert(lease.Healthy());
    {std::ofstream marker(dir+"/fail-heartbeat");marker << "fail";}
    std::this_thread::sleep_for(1800ms);assert(!lease.Healthy());
    lease.Release();assert(!lease.Active());
    std::ifstream events(dir+"/events");std::string line;int begin=0,beat=0,end=0;
    while(std::getline(events,line)){if(line=="begin")++begin;if(line=="heartbeat")++beat;if(line=="end")++end;}
    assert(begin==1 && beat>=2 && end==1);
  }
  {
    std::atomic<int> armed{0},released{0};
    xiaoai_plus::wakeup::Hooks h;
    h.on_arm=[&](const std::string&){++armed;};h.after_disarm=[&](const std::string&){++released;};
    xiaoai_plus::wakeup::Gate gate(100ms,h);
    assert(gate.TryWakeup("豆包"));assert(!gate.TryWakeup("豆包"));
    gate.SetAiSpeaking(true);std::this_thread::sleep_for(200ms);
    assert(gate.step()==xiaoai_plus::wakeup::Step::kActive && released==0);
    gate.SetAiSpeaking(false);std::this_thread::sleep_for(180ms);
    assert(gate.step()==xiaoai_plus::wakeup::Step::kIdle && released==1);
    gate.Disarm("again");assert(released==1 && armed==1);
    assert(gate.TryWakeup("豆包"));
    gate.ReserveForResponse(300ms);std::this_thread::sleep_for(180ms);
    assert(gate.step()==xiaoai_plus::wakeup::Step::kActive);
    std::this_thread::sleep_for(180ms);
    assert(gate.step()==xiaoai_plus::wakeup::Step::kIdle && released==2);
  }
  {
    std::atomic<bool> cleaning{false};
    xiaoai_plus::wakeup::Hooks h;
    h.after_disarm=[&](const std::string&){cleaning=true;std::this_thread::sleep_for(180ms);};
    xiaoai_plus::wakeup::Gate gate(5s,h);
    assert(gate.TryWakeup("豆包"));
    std::thread release([&]{gate.Disarm("test");});
    while(!cleaning)std::this_thread::sleep_for(1ms);
    assert(!gate.TryWakeup("豆包"));
    release.join();assert(gate.TryWakeup("豆包"));gate.Disarm("test");
  }
}
