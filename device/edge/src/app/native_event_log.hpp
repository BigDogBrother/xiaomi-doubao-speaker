#pragma once
#include <fstream>
#include <functional>
#include <string>
#include <sys/stat.h>
namespace xiaoai_plus::app {
// Follow lazy-created, truncated and rotated native event files without changing them.
class NativeEventLog {
 public:
  explicit NativeEventLog(std::string path):path_(std::move(path)){}
  bool Poll(const std::function<void(const std::string&)>& consume){
    struct stat st{};
    if(stat(path_.c_str(),&st)!=0){missing_=true;file_.close();return false;}
    if(!file_.is_open() || st.st_ino!=inode_ || st.st_dev!=device_ || st.st_size<position_){
      file_.close();file_.clear();file_.open(path_);
      if(!file_)return false;
      if(!opened_ && !missing_)file_.seekg(0,std::ios::end);
      opened_=true;missing_=false;inode_=st.st_ino;device_=st.st_dev;
      position_=static_cast<off_t>(file_.tellg());
    }
    file_.clear();std::string line;
    for(int i=0;i<128 && std::getline(file_,line);++i){
      consume(line);
      auto p=file_.tellg();if(p>=0)position_=static_cast<off_t>(p);else position_=st.st_size;
    }
    return true;
  }
 private:
  std::string path_;std::ifstream file_;
  ino_t inode_{0};dev_t device_{0};off_t position_{0};
  bool opened_{false},missing_{false};
};
}
