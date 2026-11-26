#include "platform/status_led.hpp"
#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
int main(){using namespace platform;
 assert(receiver_status(false,false,true,false)==ReceiverStatus::Video);
 assert(receiver_status(false,false,false,true)==ReceiverStatus::NoVideo);
 assert(receiver_status(false,true,true,true)==ReceiverStatus::Pairing);
 assert(receiver_status(true,true,true,true)==ReceiverStatus::Fault);
 assert(status_color(ReceiverStatus::Starting,0)==StatusColor({0,255,0}));
 assert(status_color(ReceiverStatus::Starting,1000)==StatusColor({0,0,0}));
 assert(status_color(ReceiverStatus::Recording,0)==StatusColor({0,0,255}));
 assert(status_color(ReceiverStatus::Recording,200)==StatusColor({0,255,0}));
 char dir[]="/tmp/kestrel-led-test-XXXXXX"; assert(mkdtemp(dir));
 auto path=std::filesystem::path(dir);
 std::ofstream(path/"multi_index")<<"red green blue\n";
 std::ofstream(path/"brightness")<<"0\n";
 std::ofstream(path/"multi_intensity")<<"0 0 0\n";
 {
  StatusLed led(path.string());led.set(ReceiverStatus::Video);
  bool green=false;
  for(int i=0;i<100&&!green;i++) {
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
   std::ifstream f(path/"multi_intensity");unsigned r=9,g=9,b=9;
   green=bool(f>>r>>g>>b)&&r==0&&g==255&&b==0;
  }
  assert(green);
 }
 {std::ifstream f(path/"brightness");unsigned v=9;f>>v;assert(v==0);}
 std::filesystem::remove_all(path);
 {StatusLed absent("/no-such-status-led");absent.set(ReceiverStatus::Fault);}
}
