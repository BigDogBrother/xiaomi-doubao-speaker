#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace xiaoai_plus::realtime {
// Private files are never part of a release. No audio or dialogue text is stored.
class Memory {
 public:
  explicit Memory(std::string path="/data/doubao-edge/memory.json") : path_(std::move(path)) {
    try {
      std::ifstream f(path_); if(f) f >> state_;
      if(!state_.is_object() || !state_.value("preferences",nlohmann::json::array()).is_array()) state_=Empty();
      if(!state_.value("id",nlohmann::json("")).is_string() || !state_.value("last_turn",nlohmann::json(0)).is_number_integer())state_=Empty();
      for(const auto& p:Preferences()) {
        if(!p.is_object() || !p.value("key",nlohmann::json()).is_string() || !p.value("value",nlohmann::json()).is_string() || !p.value("important",nlohmann::json()).is_boolean()) {state_=Empty();break;}
      }
      if(Preferences().dump().size()>240) state_["preferences"]=nlohmann::json::array();
    } catch(...) { state_=Empty(); }
  }
  static long long Now() {return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());}
  std::string Resume(long long now=Now()) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto t=state_.value("last_turn",0LL);
    return now>=1700000000 && t>0 && now>=t && now-t<1800 ? state_.value("id",std::string()) : "";
  }
  bool SetSession(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto next=state_; if(next.value("id",std::string())!=id)next["last_turn"]=0;
    next["id"]=id; return Commit(next);
  }
  bool Touch(long long now=Now()) {std::lock_guard<std::recursive_mutex> lock(mu_);auto next=state_;next["last_turn"]=now;return Commit(next);}
  bool ForgetSession() {std::lock_guard<std::recursive_mutex> lock(mu_);auto next=state_;next["id"]="";next["last_turn"]=0;return Commit(next);}
  nlohmann::json Preferences() const {std::lock_guard<std::recursive_mutex> lock(mu_);return state_.value("preferences",nlohmann::json::array());}
  nlohmann::json Execute(const nlohmann::json& a, const std::string& utterance, bool other_speaker) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto action=a.value("action",std::string());
    if(action=="list")return {{"ok",true},{"preferences",Preferences()}};
    if(action=="restart") {
      if(utterance.find("重新开始")==std::string::npos)return {{"ok",false},{"reason","explicit_restart_required"}};
      return {{"ok",ForgetSession()},{"reset_context",true}};
    }
    if(other_speaker && !a.value("guardian",false))return {{"ok",false},{"reason","speaker_not_primary"}};
    auto next=state_;auto prefs=Preferences();bool reset=false;
    if(action=="clear") {
      if(utterance.find("清空")==std::string::npos && utterance.find("全部忘")==std::string::npos && utterance.find("全删")==std::string::npos)
        return {{"ok",false},{"reason","explicit_clear_required"}};
      prefs=nlohmann::json::array();reset=true;
    }
    else {
      auto kind=a.value("kind",std::string()), value=a.value("value",std::string());
      auto evidence=a.value("evidence",std::string());
      if(action=="remove") {
        auto key=a.value("key",std::string());
        if((utterance.find("忘")==std::string::npos && utterance.find("删")==std::string::npos) ||
           ((value.empty() || utterance.find(value)==std::string::npos) && (key.empty() || utterance.find(key)==std::string::npos)))
          return {{"ok",false},{"reason","explicit_removal_required"}};
        auto kept=nlohmann::json::array();
        for(const auto& p:prefs)if(p.value("key",std::string())!=key && p.value("value",std::string())!=value)kept.push_back(p);
        next["preferences"]=kept;next["id"]="";next["last_turn"]=0;
        bool ok=Commit(next);return {{"ok",ok},{"reset_context",ok},{"preferences",Preferences()}};
      }
      if(evidence.empty() || utterance.find(evidence)==std::string::npos)return {{"ok",false},{"reason","evidence_required"}};
      if(kind!="hobby" && kind!="habit" && kind!="reply_style")return {{"ok",false},{"reason","unsupported_kind"}};
      if(value.empty() || value.size()>72 || value.find('\n')!=std::string::npos)return {{"ok",false},{"reason","invalid_value"}};
      if(action!="put" && action!="remove")return {{"ok",false},{"reason","invalid_action"}};
      auto key=a.value("key",std::string());
      if(key.empty() || key.size()>36)return {{"ok",false},{"reason","invalid_key"}};
      auto kept=nlohmann::json::array();bool important=a.value("important",false);
      for(const auto& p:prefs) {
        if(p.value("key",std::string())==key || p.value("value",std::string())==value) {
          important=important || p.value("important",false);reset=true;
        }else kept.push_back(p);
      }
      prefs=kept;
      if(action=="put")prefs.push_back({{"key",key},{"kind",kind},{"value",value},{"important",important}});
      if(action=="remove")reset=true;
      // A deliberately conservative UTF-8 byte budget, NOT an exact tokenizer.
      while(prefs.dump().size()>240 && !prefs.empty()) {
        size_t victim=0;
        for(size_t i=0;i<prefs.size();++i)if(!prefs[i].value("important",false)){victim=i;break;}
        prefs.erase(prefs.begin()+victim);
      }
      if(action=="put") {
        bool present=false;for(const auto& p:prefs)present|=p.value("value",std::string())==value;
        if(!present)return {{"ok",false},{"reason","budget_full"}};
      }
    }
    next["preferences"]=prefs;
    if(reset){next["id"]="";next["last_turn"]=0;}
    bool ok=Commit(next);
    return {{"ok",ok},{"reset_context",ok&&reset},{"preferences",Preferences()}};
  }
 private:
  static nlohmann::json Empty(){return {{"version",1},{"id",""},{"last_turn",0},{"preferences",nlohmann::json::array()}};}
  bool Commit(const nlohmann::json& next) {
    const auto tmp=path_+".tmp";int fd=open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0)return false;
    const auto payload=next.dump();size_t done=0;bool ok=fchmod(fd,0600)==0;
    while(ok && done<payload.size()){auto n=write(fd,payload.data()+done,payload.size()-done);if(n<=0)ok=false;else done+=n;}
    if(fsync(fd)!=0)ok=false;if(close(fd)!=0)ok=false;
    if(ok && rename(tmp.c_str(),path_.c_str())==0){state_=next;return true;}
    unlink(tmp.c_str());return false;
  }
  std::string path_;
  mutable std::recursive_mutex mu_;
  nlohmann::json state_=Empty();
};
}
