#include <algorithm>
#include <csignal>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "common/log.hpp"

#include "app/app.hpp"
#include "config/config.hpp"
#include "wakeup/kws_zipformer.hpp"

namespace {
volatile std::sig_atomic_t g_stop_requested = 0;

void SignalHandler(int) {
  g_stop_requested = 1;
}
}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  bool probe_api = false;
  bool probe_lease = false;
  bool observe_only = false;
  bool probe_session = false;
  std::string probe_kws;
  bool probe_kws_streams = false;
  std::string probe_first, probe_second;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-c" && i + 1 < argc) {
      config_path = argv[++i];
    }
    if (arg == "--probe-api") probe_api = true;
    if (arg == "--probe-lease") probe_lease = true;
    if (arg == "--observe-only") observe_only = true;
    if (arg == "--probe-session") probe_session = true;
    if (arg == "--probe-kws" && i + 1 < argc) probe_kws = argv[++i];
    if (arg == "--probe-kws-streams" && i + 1 < argc) {probe_kws = argv[++i];probe_kws_streams = true;}
    if (arg == "--probe-dialog" && i + 2 < argc) {probe_first=argv[++i];probe_second=argv[++i];}
  }

  try {
    auto cfg = xiaoai_plus::config::load(config_path);
    if (probe_lease) {
      xiaoai_plus::app::NativeLease lease;
      if (!lease.Acquire()) {std::cerr << "lease_probe_busy\n";return 1;}
      std::cout << "lease_probe_acquired" << std::endl;
      while(lease.Healthy())std::this_thread::sleep_for(std::chrono::milliseconds(250));
      return 1;
    }
    if (!probe_first.empty()) {
      auto read_pcm=[](const std::string& path) {
        std::ifstream file(path,std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
      };
      auto first=read_pcm(probe_first), second=read_pcm(probe_second);
      if(first.empty()||second.empty())return 1;
      std::atomic<int> speech{0}, answer_starts{0}, answer_bytes{0};
      std::atomic<bool> speaking{false}, interruption_while_speaking{false};
      xiaoai_plus::realtime::Client::Callbacks callbacks;
      callbacks.on_audio = [&](const std::vector<uint8_t>& b) {answer_bytes += b.size();};
      callbacks.on_user_activity = [&] {if(++speech >= 2 && speaking)interruption_while_speaking=true;};
      callbacks.on_set_ai_speaking = [&](bool state) {speaking=state;if(state)++answer_starts;};
      xiaoai_plus::realtime::Client client(cfg, callbacks);
      if(!client.Start()||!client.StartSession(std::chrono::seconds(12))){client.Stop();std::cerr<<"dialog_connect_failed\n";return 1;}
      size_t first_i=0,second_i=0; bool began_second=false;
      auto tick=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
      while(std::chrono::steady_clock::now()<deadline && client.IsReady()) {
        std::vector<uint8_t> frame(640,0);
        if(first_i<first.size()){
          const auto n=std::min<size_t>(frame.size(),first.size()-first_i);
          std::copy_n(first.data()+first_i,n,frame.data());first_i+=n;
        }else if(answer_starts>0 && second_i<second.size()){
          began_second=true;
          const auto n=std::min<size_t>(frame.size(),second.size()-second_i);
          std::copy_n(second.data()+second_i,n,frame.data());second_i+=n;
        }
        if(!client.EnqueueAudio(frame))break;
        if(began_second && second_i==second.size() && speech>=2 && answer_starts>=2 && answer_bytes>9600)break;
        std::this_thread::sleep_until(tick); tick+=std::chrono::milliseconds(20);
      }
      client.FinishSession();client.Stop();
      std::cout << "dialog_probe speech=" << speech << " answers=" << answer_starts
                << " barge_during_generation=" << interruption_while_speaking << " bytes=" << answer_bytes << "\n";
      return speech>=2 && answer_starts>=2 && answer_bytes>9600 ? 0 : 1;
    }
    if (!probe_kws.empty()) {
      std::ifstream f(probe_kws,std::ios::binary);
      std::vector<uint8_t> pcm((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
      if (pcm.empty()) return 1;
      xiaoai_plus::wakeup::ZipformerKwsEngine kws(cfg.wakeup);
      pcm.resize(pcm.size()+64000,0);
      if (probe_kws_streams) {
        std::vector<uint8_t> silence(640,0);
        for (int filtered=0;filtered<2;++filtered) {
          kws.Reset();bool positive=false,negative=false;
          for(size_t i=0;i+640<=pcm.size();i+=640) {
            auto raw=kws.AcceptPcm16(filtered ? silence.data() : pcm.data()+i,640,16000,1,16);
            auto clean=kws.AcceptFilteredPcm16(filtered ? pcm.data()+i : silence.data(),640,16000,1,16);
            auto hit=filtered ? clean : raw;auto miss=filtered ? raw : clean;
            positive |= hit && hit->keyword=="豆包豆包";
            negative |= miss.has_value();
          }
          if(!positive || negative){std::cerr << "kws_stream_isolation_failed\n";return 1;}
        }
        std::cout << "kws_stream_isolation_ok\n";return 0;
      }
      for(size_t i=0;i+640<=pcm.size();i+=640) {
        auto hit=kws.AcceptPcm16(pcm.data()+i,640,16000,1,16);
        if(hit && hit->keyword=="豆包豆包") {std::cout << "kws_ok\n";return 0;}
      }
      std::cerr << "kws_no_match\n";return 1;
    }
    if (probe_api) {
      std::atomic<int> audio_bytes{0}; std::atomic<bool> finished{false};
      xiaoai_plus::realtime::Client::Callbacks callbacks;
      callbacks.on_audio = [&](const std::vector<uint8_t>& b) {audio_bytes += b.size();};
      callbacks.on_set_ai_speaking = [&](bool speaking) {if(!speaking)finished = true;};
      xiaoai_plus::realtime::Client client(cfg, callbacks);
      if (!client.Start() || !client.StartSession(std::chrono::seconds(12)) || !client.SendProbe()) {
        client.Stop(); std::cerr << "api_probe_connect_failed\n"; return 1;
      }
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
      while(!finished && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      client.FinishSession(); client.Stop();
      if (!finished || audio_bytes < 9600) {std::cerr << "api_probe_audio_failed\n";return 1;}
      std::cout << "api_vivi_audio_ok " << audio_bytes << " bytes\n";
      return 0;
    }
    if (observe_only) setenv("DOUBAO_EDGE_OBSERVE_ONLY", "1", 1);
    if (probe_session) setenv("DOUBAO_EDGE_DIAGNOSTICS", "1", 1);
    xiaoai_plus::app::App app(cfg);
    g_stop_requested = 0;
    std::atomic<bool> watcher_done{false};

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    std::thread signal_watcher([&app, &watcher_done]() {
      while (!watcher_done.load()) {
        if (g_stop_requested) {
          app.Stop();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
    std::thread probe;
    if (probe_session) probe = std::thread([&] {
      std::this_thread::sleep_for(std::chrono::seconds(4));
      std::cout << (app.ProbeWake() ? "probe_wake_started" : "probe_wake_rejected") << std::endl;
      for (int i=0;i<80 && !watcher_done;++i)std::this_thread::sleep_for(std::chrono::milliseconds(250));
      app.Stop();
    });

    bool run_ok = false;
    try {
      run_ok = app.Run();
    } catch (...) {
      app.Stop();
      watcher_done.store(true);
      if (probe.joinable()) probe.join();
      if (signal_watcher.joinable()) {
        signal_watcher.join();
      }
      throw;
    }

    watcher_done.store(true);
    if (probe.joinable()) probe.join();
    if (signal_watcher.joinable()) {
      signal_watcher.join();
    }
    if (!run_ok) {
      std::cerr << "app stopped with error" << std::endl;
      return 1;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "startup failed: " << e.what() << std::endl;
    return 1;
  }
}
