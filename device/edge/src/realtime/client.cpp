// Adapted from xiaoai-plus (MIT). Realtime JSON API; no transcript logging.
#include "realtime/client.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
#include "common/log.hpp"

namespace xiaoai_plus::realtime {
namespace {
const auto kLog = xiaoai_plus::GetLogger("duplex");
const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string Encode(const std::vector<uint8_t>& in) {
  std::string out; out.reserve((in.size()+2)/3*4);
  for (size_t i=0; i<in.size(); i+=3) {
    unsigned n=unsigned(in[i])<<16;
    if(i+1<in.size())n|=unsigned(in[i+1])<<8;
    if(i+2<in.size())n|=in[i+2];
    out.push_back(kAlphabet[(n>>18)&63]); out.push_back(kAlphabet[(n>>12)&63]);
    out.push_back(i+1<in.size()?kAlphabet[(n>>6)&63]:'=');
    out.push_back(i+2<in.size()?kAlphabet[n&63]:'=');
  }
  return out;
}
std::vector<uint8_t> Decode(const std::string& s) {
  std::vector<uint8_t> out; out.reserve(s.size()*3/4); unsigned bits=0; int n=0;
  for(unsigned char c:s) {
    if(c=='=')break;
    const char* p=std::find(kAlphabet,kAlphabet+64,char(c));
    if(p==kAlphabet+64)continue;
    bits=(bits<<6)|unsigned(p-kAlphabet); n+=6;
    if(n>=8){n-=8;out.push_back(uint8_t((bits>>n)&255));}
  }
  return out;
}
std::vector<uint32_t> Characters(const std::string& s) {
  std::vector<uint32_t> out;
  for(size_t i=0;i<s.size();) {
    const auto c=static_cast<unsigned char>(s[i]);uint32_t cp=0;size_t n=1;
    if(c<128)cp=c;
    else if((c&0xe0)==0xc0){cp=c&31;n=2;}
    else if((c&0xf0)==0xe0){cp=c&15;n=3;}
    else if((c&0xf8)==0xf0){cp=c&7;n=4;}
    else {++i;continue;}
    if(i+n>s.size())break;
    for(size_t j=1;j<n;++j)cp=(cp<<6)|(static_cast<unsigned char>(s[i+j])&63);
    i+=n;
    if(cp>=0xff01 && cp<=0xff5e)cp-=0xfee0;
    if(cp<128){if(!std::isalnum(static_cast<unsigned char>(cp)))continue;cp=std::tolower(static_cast<unsigned char>(cp));}
    else if((cp>=0x3000 && cp<=0x303f) || (cp>=0xff00 && cp<=0xff65))continue;
    out.push_back(cp);
  }
  return out;
}
bool LooksLikeEcho(const std::string& assistant,const std::string& input) {
  const auto a=Characters(assistant), b=Characters(input);
  if(b.empty() || a.empty())return false;
  if(b.size()<4)return std::search(a.begin(),a.end(),b.begin(),b.end())!=a.end();
  std::vector<int> prev(a.size()+1,0),row(a.size()+1);
  for(size_t i=0;i<b.size();++i){
    row[0]=static_cast<int>(i+1);
    for(size_t j=0;j<a.size();++j)row[j+1]=std::min({prev[j+1]+1,row[j]+1,prev[j]+int(b[i]!=a[j])});
    prev.swap(row);
  }
  return *std::min_element(prev.begin(),prev.end())<=std::min<int>(5,b.size()/5);
}
}
Client::Client(config::Config cfg, Callbacks cb):cfg_(std::move(cfg)),callbacks_(std::move(cb)){}
Client::~Client(){Stop();}
bool Client::Start(){if(running_.exchange(true))return true;sender_=std::thread(&Client::SenderLoop,this);return true;}
void Client::Stop(){
  if(!running_.exchange(false))return;
  closing_=true; ready_=false; state_cv_.notify_all();
  if(sender_.joinable() && sender_.get_id()!=std::this_thread::get_id())sender_.join();
  if(ws_)ws_->stop(); ws_.reset();
}
void Client::Send(const nlohmann::json& event){
  std::lock_guard<std::mutex> lock(send_mu_);
  if(ws_ && ws_->getReadyState()==ix::ReadyState::Open)ws_->send(event.dump());
}
bool Client::StartSession(std::chrono::milliseconds timeout){
  if(!running_)return false;
  if(ws_){closing_=true;ws_->stop();ws_.reset();}
  closing_=false;ready_=false;speaking_=false;capacity_drop_=false;
  input_announced_=started_during_playback_=drop_echo_response_=awaiting_new_response_=false;
  assistant_text_="我在，请说。";input_text_.clear();
  {std::lock_guard<std::mutex> l(audio_mu_);audio_.clear();sending_=false;}
  ws_=std::make_unique<ix::WebSocket>();
  ws_->setUrl(cfg_.realtime.preset.ws_url);
  ws_->setExtraHeaders({{"X-Api-Key",cfg_.realtime.api_key}});
  ix::SocketTLSOptions tls;tls.caFile="/etc/ssl/certs/ca-certificates.crt";ws_->setTLSOptions(tls);
  ws_->disableAutomaticReconnection();
  ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg){OnMessage(msg);});
  ws_->start();
  std::unique_lock<std::mutex> lock(state_mu_);
  return state_cv_.wait_for(lock,timeout,[this]{return ready_.load() || closing_.load() || !running_.load();}) && ready_;
}
bool Client::FinishSession(std::chrono::milliseconds){
  if(closing_.exchange(true))return true;
  if(ready_)Send({{"type","session.close"}});
  ready_=false;
  {std::lock_guard<std::mutex> l(audio_mu_);audio_.clear();sending_=false;}
  state_cv_.notify_all();
  return true;
}
bool Client::SendSayHello(){return ready_;}  // The fixed Vivi greeting plays locally.
bool Client::SendProbe(){if(!IsReady())return false;Send({{"type","speech_text_buffer.commit"},{"text","连接测试。"}});return true;}
void Client::CancelResponse(){
  if(capacity_drop_.exchange(true))return;
  Send({{"type","response.cancel"}});
  speaking_=false;ended_at_=std::chrono::steady_clock::now();
  if(callbacks_.on_set_ai_speaking)callbacks_.on_set_ai_speaking(false);
}
bool Client::EnqueueAudio(const std::vector<uint8_t>& chunk){
  if(!ready_||closing_)return false;
  std::lock_guard<std::mutex> l(audio_mu_);
  if(audio_.size()+chunk.size()>32000)return false; // Bounded one-second backlog.
  audio_.insert(audio_.end(),chunk.begin(),chunk.end());return true;
}
bool Client::EnqueueAudio(std::vector<uint8_t>&& chunk){return EnqueueAudio(chunk);}
void Client::SenderLoop(){
  auto next=std::chrono::steady_clock::now();
  while(running_){
    next+=std::chrono::milliseconds(20);
    if(ready_&&!closing_){
      std::vector<uint8_t> frame;
      {std::lock_guard<std::mutex> lock(audio_mu_);
       if(audio_.size()>=640){frame.reserve(640);for(int i=0;i<640;++i){frame.push_back(audio_.front());audio_.pop_front();}}}
      if(frame.empty()){
        if(sending_){Send({{"type","input_audio_mute.commit"}});sending_=false;}
      }else{
        if(!sending_){Send({{"type","input_audio_unmute.commit"}});sending_=true;}
        Send({{"type","input_audio_buffer.append"},{"audio",Encode(frame)}});
      }
    }
    auto now=std::chrono::steady_clock::now();
    if(now-next>std::chrono::milliseconds(60))next=now;
    std::this_thread::sleep_until(next);
  }
}
void Client::OnMessage(const ix::WebSocketMessagePtr& msg){
  if(msg->type==ix::WebSocketMessageType::Open){
    if(closing_)return;
    Send({{"type","session.create"},{"session",{
      {"model",cfg_.realtime.preset.model},
      {"instructions","你是豆包，请用自然、简洁的中文对话。你没有米家设备控制、闹钟或音乐控制权限，不要声称已经操作这些功能。"},
      {"audio",{{"input",{{"format",{{"type","pcm"},{"rate",16000}}}}},
                {"output",{{"format",{{"type","pcm_s16le"},{"rate",24000}}},
                           {"voice","zh_female_vv_uranus_bigtts"},{"loudness",50}}}}},
      {"tools",nlohmann::json::array()}}}});
    return;
  }
  if(msg->type==ix::WebSocketMessageType::Close || msg->type==ix::WebSocketMessageType::Error){
    bool unexpected=!closing_.exchange(true);bool was_ready=ready_.exchange(false);state_cv_.notify_all();
    if(unexpected && was_ready && callbacks_.on_session_closed)callbacks_.on_session_closed("transport_lost");
    return;
  }
  if(msg->type!=ix::WebSocketMessageType::Message || msg->binary)return;
  try {
    auto ev=nlohmann::json::parse(msg->str);const std::string type=ev.value("type",std::string());
    if(type=="session.created"){
      if(closing_)return;
      Send({{"type","input_audio_mute.commit"}});ready_=true;state_cv_.notify_all();return;
    }
    if(type=="session.closed"){
      bool was_ready=ready_.exchange(false),requested=closing_.exchange(true);
      state_cv_.notify_all();
      if(was_ready && !requested && callbacks_.on_session_closed)callbacks_.on_session_closed("service_closed");
      return;
    }
    if(type=="error"){
      bool was_ready=ready_.exchange(false);closing_=true;state_cv_.notify_all();
      if(was_ready && callbacks_.on_session_closed)callbacks_.on_session_closed("service_error");return;
    }
    if(!ready_||closing_)return;
    if(type=="response.output_text.delta"){
      assistant_text_+=ev.value("delta",std::string());
      if(assistant_text_.size()>3600){
        size_t n=assistant_text_.size()-3600;
        while(n<assistant_text_.size() && (static_cast<unsigned char>(assistant_text_[n])&0xc0)==0x80)++n;
        assistant_text_.erase(0,n);
      }
    }else if(type=="conversation.item.input_audio_transcription.started"){
      input_text_.clear();input_announced_=false;drop_echo_response_=false;
      started_during_playback_=speaking_ || std::chrono::steady_clock::now()-ended_at_<std::chrono::milliseconds(1200);
    }else if(type=="conversation.item.input_audio_transcription.delta" || type=="conversation.item.input_audio_transcription.completed"){
      const bool complete=type.find("completed")!=std::string::npos;
      std::string text=ev.value("delta",std::string());if(text.empty())text=ev.value("transcript",ev.value("text",std::string()));
      if(!text.empty())input_text_=text;
      if(input_text_.size()>360){
        size_t n=input_text_.size()-360;
        while(n<input_text_.size() && (static_cast<unsigned char>(input_text_[n])&0xc0)==0x80)++n;
        input_text_.erase(0,n);
      }
      if(input_text_.empty())return;
      bool echo=started_during_playback_ && LooksLikeEcho(assistant_text_,input_text_);
      if(!input_announced_ && !echo && (Characters(input_text_).size()>=4 || complete || !started_during_playback_)){
        input_announced_=true;awaiting_new_response_=true;drop_echo_response_=false;
        if(callbacks_.on_user_activity)callbacks_.on_user_activity();
      }
      if(echo && !input_announced_)drop_echo_response_=true;
      if(complete && input_announced_ && callbacks_.on_asr_final)callbacks_.on_asr_final(input_text_);
    }else if(type=="response.output_audio.started"){
      capacity_drop_=false;
      awaiting_new_response_=false;
      if(drop_echo_response_){Send({{"type","response.cancel"}});return;}
      speaking_=true;if(callbacks_.on_set_ai_speaking)callbacks_.on_set_ai_speaking(true);
    }else if(type=="response.output_audio.delta"){
      if(capacity_drop_||drop_echo_response_||awaiting_new_response_)return;
      auto pcm=Decode(ev.value("delta",std::string()));
      if(!pcm.empty() && pcm.size()%2==0 && callbacks_.on_audio) {
        // Small chunks bound playback interruption latency even if the provider
        // delivers a large delta in one message.
        for(size_t i=0;i<pcm.size();i+=960){
          const auto end=std::min(pcm.size(),i+960);
          callbacks_.on_audio(std::vector<uint8_t>(pcm.begin()+i,pcm.begin()+end));
        }
      }
    }else if(type=="response.output_audio.done"){
      if(!capacity_drop_ && !drop_echo_response_){speaking_=false;ended_at_=std::chrono::steady_clock::now();if(callbacks_.on_set_ai_speaking)callbacks_.on_set_ai_speaking(false);}
    }else if(type=="response.done"){
      if(callbacks_.on_chat_ended)callbacks_.on_chat_ended();
    }
  }catch(const std::exception&){kLog->warn("invalid_realtime_event");}
}
} // namespace xiaoai_plus::realtime
