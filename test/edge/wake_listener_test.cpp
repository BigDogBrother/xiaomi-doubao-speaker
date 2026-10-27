#include <cassert>
#include <chrono>
#include <memory>
#include "wakeup/local_listener.hpp"
using namespace xiaoai_plus::wakeup;
struct Fake : IKwsEngine {
 int raw=0,filtered=0,resets=0; bool raw_hit=false,filtered_hit=true;
 std::optional<KwsHit> AcceptPcm16(const uint8_t*,size_t,int,int,int) override {++raw;return raw_hit ? std::optional<KwsHit>{KwsHit{"豆包豆包","raw"}} : std::nullopt;}
 std::optional<KwsHit> AcceptFilteredPcm16(const uint8_t*,size_t,int,int,int) override {++filtered;return filtered_hit ? std::optional<KwsHit>{KwsHit{"豆包豆包","filtered"}} : std::nullopt;}
 void Reset() override {++resets;}
};
int main(){
 Gate gate(std::chrono::seconds(60),{});int calls=0;
 Trigger trigger({"豆包豆包"},[&](const std::string& text){++calls;return gate.TryWakeup(text);});
 auto engine=std::make_shared<Fake>();LocalListener::Config c;c.gate=&gate;c.trigger=&trigger;c.kws_engine=engine;c.min_trigger_interval_ms=0;
 LocalListener listener(c);std::vector<uint8_t> pcm(640,0);
 listener.AcceptPcm(pcm,pcm);assert(calls==1 && engine->raw==1 && engine->filtered==1);
 listener.AcceptPcm(pcm,pcm);assert(calls==1 && engine->raw==1 && engine->filtered==1);
 gate.Disarm("test");engine->raw_hit=true;engine->filtered_hit=false;
 listener.AcceptPcm(pcm,pcm);assert(calls==2 && engine->resets==1 && engine->raw==2 && engine->filtered==2);
 listener.AcceptPcm(pcm,pcm);gate.Disarm("test");engine->raw_hit=false;
 listener.AcceptPcm(pcm,pcm);assert(calls==2 && engine->resets==2);
 listener.Close();gate.Disarm("done");
}
