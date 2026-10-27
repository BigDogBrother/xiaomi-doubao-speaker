#include "app/native_lease.hpp"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>
namespace xiaoai_plus::app {
NativeLease::NativeLease(std::string helper):helper_(std::move(helper)){
  unsigned char raw[16];const int fd=open("/dev/urandom",O_RDONLY);
  if(fd<0 || read(fd,raw,sizeof(raw))!=sizeof(raw)){if(fd>=0)close(fd);throw std::runtime_error("lease entropy unavailable");}
  close(fd);const char* hex="0123456789abcdef";
  for(auto b:raw){token_+=hex[b>>4];token_+=hex[b&15];}
}
NativeLease::~NativeLease(){Release();}
bool NativeLease::Run(const char* command){
  auto pid=fork();if(pid<0)return false;
  if(pid==0){
    int nullfd=open("/dev/null",O_WRONLY);if(nullfd>=0){dup2(nullfd,1);dup2(nullfd,2);close(nullfd);}
    // The watchdog must not inherit the audio pipes or cloud socket.
    for(int fd=3;fd<1024;++fd)close(fd);
    execl("/bin/sh","sh",helper_.c_str(),command,token_.c_str(),static_cast<char*>(nullptr));_exit(127);
  }
  int status=0;while(waitpid(pid,&status,0)<0){if(errno!=EINTR)return false;}
  return WIFEXITED(status) && WEXITSTATUS(status)==0;
}
bool NativeLease::Acquire(){
  std::lock_guard<std::mutex> lock(mu_);if(active_)return false;
  acquiring_=true;
  if(!Run("begin")){acquiring_=false;return false;}
  active_=true;lost_=false;stopping_=false;
  acquiring_=false;
  try {heartbeat_=std::thread([this]{
    while(!stopping_){
      for(int i=0;i<5&&!stopping_;++i)std::this_thread::sleep_for(std::chrono::milliseconds(300));
      if(!stopping_ && !Run("heartbeat")){lost_=true;break;}
    }
  });}catch(...){Run("end");active_=false;throw;}
  return true;
}
void NativeLease::Release(){
  std::lock_guard<std::mutex> lock(mu_);if(!active_)return;
  stopping_=true;if(heartbeat_.joinable())heartbeat_.join();
  Run("end");active_=false;
}
}
