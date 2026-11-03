#include "realtime/memory.hpp"
#include <cassert>
#include <filesystem>
#include <sys/stat.h>
using xiaoai_plus::realtime::Memory;
int main() {
  const auto path=(std::filesystem::temp_directory_path()/"doubao-memory-test.json").string();
  unlink(path.c_str());
  const long long now=1800000000;
  Memory m(path);
  assert(m.SetSession("test-id"));assert(m.Resume(now).empty());
  assert(m.Touch(now));assert(m.Resume(now+1799)=="test-id");
  assert(m.Resume(now+1800).empty());assert(m.Resume(now-1).empty());
  Memory restored(path);assert(restored.Resume(now+10)=="test-id");
  // Greeting alone does not extend the idle deadline.
  assert(restored.SetSession("test-id"));assert(restored.Resume(now+1800).empty());
  auto args=nlohmann::json{{"action","put"},{"kind","hobby"},{"key","绘画"},{"value","喜欢画画"},{"evidence","我喜欢画画"},{"important",true}};
  assert(!m.Execute(args,"讲个故事",false)["ok"].get<bool>());
  assert(!m.Execute(args,"我喜欢画画",true)["ok"].get<bool>());
  assert(m.Execute(args,"我喜欢画画",false)["ok"].get<bool>());
  assert(m.Execute(args,"我喜欢画画",false)["ok"].get<bool>());
  assert(m.Preferences().size()==1);
  auto remove=nlohmann::json{{"action","remove"},{"key","绘画"}};
  assert(!m.Execute(remove,"讲个故事",false)["ok"].get<bool>());
  assert(m.Execute(remove,"忘记绘画这个爱好",false)["ok"].get<bool>());
  assert(m.Preferences().empty());assert(m.Resume(now).empty());
  assert(m.Execute(args,"我喜欢画画",false)["ok"].get<bool>());
  for(int i=0;i<20;++i) {
    args["key"]=std::to_string(i);args["value"]="偏好"+std::to_string(i);args["important"]=false;
    m.Execute(args,"我喜欢画画",false);assert(m.Preferences().dump().size()<=240);
  }
  args={{"action","clear"}};
  assert(!m.Execute(args,"讲个故事",false)["ok"].get<bool>());
  assert(m.Execute(args,"清空爱好和习惯",false)["ok"].get<bool>());
  assert(m.Preferences().empty());assert(m.Resume(now).empty());
  struct stat st{};assert(stat(path.c_str(),&st)==0);assert((st.st_mode&0777)==0600);
  Memory broken("/nonexistent-memory-test-dir/memory.json");
  assert(!broken.SetSession("x"));assert(broken.Resume(now).empty());
  unlink(path.c_str());
}
