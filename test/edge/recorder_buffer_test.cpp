#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include "audio/recorder.hpp"
int main(int argc,char**argv){
 assert(argc==2);std::string dir=argv[1];setenv("PATH",dir.c_str(),1);setenv("EDGE_RECORDER_TEST_DIR",dir.c_str(),1);
 xiaoai_plus::config::Audio cfg;cfg.sample_rate=16000;cfg.channels=2;cfg.bits_per_sample=16;cfg.buffer_size=8000;cfg.period_size=320;
 xiaoai_plus::audio::ArecordRecorder recorder(cfg);std::atomic<int> frames{0};std::atomic<bool> sizes{true};
 assert(recorder.Start([&](const std::vector<uint8_t>& b){if(b.size()!=1280)sizes=false;++frames;}));
 for(int i=0;i<300 && frames<3;i++)std::this_thread::sleep_for(std::chrono::milliseconds(10));
 recorder.Stop();assert(frames==3 && sizes);
 std::ifstream f(dir+"/argv");std::string line,all;while(std::getline(f,line))all+=line+' ';
 assert(all.find("--buffer-size 8000")!=std::string::npos && all.find("--period-size 320")!=std::string::npos);
}
