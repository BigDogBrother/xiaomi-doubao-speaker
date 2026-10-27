#include <cassert>
#include <fstream>
#include <cstdio>
#include <string>
#include "app/native_event_log.hpp"
#include "app/session_progress.hpp"
int main(int argc,char**argv){
 assert(argc==2);std::string file=std::string(argv[1])+"/native-events";std::remove(file.c_str());
 xiaoai_plus::app::NativeEventLog log(file);int lines=0;
 auto read=[&](const std::string&){++lines;};
 assert(!log.Poll(read));{std::ofstream f(file);f<<"native-wake\n";}
 assert(log.Poll(read) && lines==1);assert(log.Poll(read) && lines==1);
 {std::ofstream f(file,std::ios::app);f<<"native-finish\n";}
 assert(log.Poll(read) && lines==2);
 {std::ofstream f(file);f<<"reset\n";}
 assert(log.Poll(read) && lines==3);
 assert(std::rename(file.c_str(),(file+".old").c_str())==0);
 {std::ofstream f(file);f<<"rotated\n";}
 assert(log.Poll(read) && lines==4);
 std::remove(file.c_str());assert(!log.Poll(read));
 {std::ofstream f(file);f<<"recreated\n";}
 assert(log.Poll(read) && lines==5);
 xiaoai_plus::app::NativeEventLog existing(file);assert(existing.Poll(read) && lines==5);
 xiaoai_plus::app::SessionProgress progress;progress.Touch(1000);
 for(long long t=1000;t<3600000;t+=20000){progress.Touch(t);assert(!progress.Stalled(t+19999));}
 progress.Touch(3600000);assert(!progress.Stalled(3660000));assert(progress.Stalled(3660001));
}
