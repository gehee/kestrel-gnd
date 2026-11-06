#include "app/ascent_menu.hpp"
#include "region/settings.hpp"
#include "hud/lvgl/controller.hpp"
#include <cstdio>
#include <stdexcept>
#include <unistd.h>
using namespace kestrel_ui;
namespace {
void check(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
struct Radio : gnd::Radio {
    gnd::RadioState value{}; gnd::ScanSample sample{}; bool have=false,fast=false; int changed=-1;
    gnd::RadioState state() const override {return value;}
    uint32_t version() const override {return 0;}
    void watch(bus::Waker*) override {}
    Ticket set_firmware(int) override {return {};}
    Ticket set_frame(int) override {return {};}
    Ticket set_max_mcs(int) override {return {};}
    Ticket set_max_bandwidth(int) override {return {};}
    Ticket set_steady(bool) override {return {};}
    Ticket set_channel(int f) override {changed=f;return {1,f};}
    void search_channels() override {changed=0;}
    void scan_screen(bool v) override {fast=v;}
    void bind() override {}
    void set_dist_offset(int) override {}
    bool take_scan(gnd::ScanSample& s) override {if(!have)return false;s=sample;have=false;return true;}
    bool take_rate(float&) override {return false;}
    uint64_t dropped() const override {return 0;}
};
}
int main() {
 try {
    char dir[]="ascent-menu-test-XXXXXX"; check(mkdtemp(dir),"Create private test directory");
    const std::string path=std::string(dir)+"/settings";
    {
      Settings settings; settings.load(path); zones::load(settings);
      ascent::RegionMenu regions; BackendState state; regions.update(state);
      check(!state.controls.at("country").enabled,"Country requires area");
      const int zone=zones::find("America/Toronto"), area=zones::zone(zone).area, country=zones::zone(zone).country;
      regions.apply("area",area); regions.apply("country",country); regions.apply("timezone",zone); regions.apply("licence",1);
      regions.update(state);
      check(zones::selected()==zone && state.controls.at("country").value==country,"Area, country and zone are coherent");
      bool failed=false; try {regions.apply("timezone",-1);} catch(const std::invalid_argument&) {failed=true;}
      check(failed && zones::selected()==zone,"Invalid dependent choice leaves region unchanged");
      settings.flush(); Settings loaded; loaded.load(path);
      check(loaded.get(region::setting::time_zone)=="America/Toronto" && loaded.get(region::setting::amateur_licence),"Region persists through shared settings");
      Model model; Controller controller(model); Radio radio; ascent::ChannelMenu channels(radio);
      auto now=monotonic_us(); model.backend.service.connected=true;
      model.backend.vrx.connected=true; model.backend.vrx.mcs=5; model.backend.vrx.stamp_us=now;
      radio.value.link.tx_freq=5800; radio.value.link.bw_step=4; radio.value.rf_caps=gnd::RF_CAP_CHAN;
      radio.sample.chan_num=3; radio.sample.last_update_ms=now/1000;
      radio.sample.freq_mhz[0]=5800; radio.sample.power_dbm[0]=-90;
      radio.sample.freq_mhz[1]=5820; radio.sample.power_dbm[1]=-95;
      radio.sample.freq_mhz[2]=5780; radio.sample.power_dbm[2]=-85;
      radio.have=true; channels.update(model.backend,now,true);
      check(radio.fast && model.backend.channels.readings.front().mhz==5780,"Scan sorted and fast polling requested");
      controller.apply_receiver=[&](const std::string& id,int value) {check(id=="channel","Channel dispatch");return channels.apply(value,model.backend);};
      model.channel_open=true; model.channel_mhz=5800;
      controller.key(Key::Up); check(model.channel_mhz==5820 && radio.changed==-1,"Quietest selects by peak without changing RF");
      controller.key(Key::Enter); check(radio.changed==5820,"OK requests selected frequency");
      controller.key(Key::Down); check(model.channel_mhz==5800,"Down returns to current link");
      now+=1000; radio.sample.last_update_ms=now/1000; radio.sample.power_dbm[1]=-110; radio.have=true;
      channels.update(model.backend,now,true);
      check(model.backend.channels.readings.back().peak_dbm==-95,"Peak holds for thirty seconds");
      channels.update(model.backend,now+7000000,true);
      bool stale=false; try {channels.apply(5800,model.backend);} catch(const std::runtime_error&) {stale=true;}
      check(stale,"Stale scan cannot retune");
      controller.key(Key::Back); check(!model.channel_open,"Back closes scan");
      model.clock=4; check(!model.clock_text(1730808000,now).empty(),"Always clock displays");
      model.clock=2; model.backend.media.video_monitored=true; model.backend.media.video_stamp_us=now;
      check(model.clock_text(1730808000,now).empty(),"Idle-only clock hides over fresh video");
      model.backend.media.video_stamp_us=0;
      check(!model.clock_text(1730808000,now).empty(),"Idle-only clock displays without video");
    }
    unlink(path.c_str()); rmdir(dir);
    puts("PASS: region persistence, dependent choices, channel controls, freshness, peak hold and clock"); return 0;
 } catch(const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what()); return 1;}
}
