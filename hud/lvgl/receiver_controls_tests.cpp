#include "receiver_controls.hpp"
#include "controller.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace kestrel_ui;
namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class I, class S> struct Fake : I {
    S value{}; uint32_t revision=1; int calls=0, last=0;
    S state() const override { return value; }
    uint32_t version() const override { return revision; }
    void watch(bus::Waker*) override {}
    Ticket request(int v) { ++calls; last=v; return {uint32_t(calls),v}; }
};
struct Camera : Fake<air::Camera,air::CameraState> {
    air::CameraField field{};
    Ticket set(air::CameraField f,int v) override { field=f; return request(v); }
    Ticket set_mode(int v) override { return request(v); }
};
struct Encoder : Fake<air::Encoder,air::EncoderState> { Ticket set_max_bitrate(int v) override { return request(v); } };
struct Air : Fake<air::Radio,air::RadioState> { Ticket set_tx_power(int v) override { return request(v); } };
struct Unit : Fake<air::Unit,air::UnitState> {
    Ticket set_standby(bool v) override { return request(v); }
    Ticket set_logs(bool v) override { return request(v); }
};
struct Ground : Fake<gnd::Radio,gnd::RadioState> {
    Ticket set_firmware(int v) override {return request(v);}
    Ticket set_frame(int v) override {return request(v);}
    Ticket set_max_mcs(int v) override {return request(v);}
    Ticket set_max_bandwidth(int v) override {return request(v);}
    Ticket set_steady(bool v) override {return request(v);}
    Ticket set_channel(int v) override {return request(v);}
    void search_channels() override {request(0);}
    void scan_screen(bool) override {}
    void bind() override {request(0);}
    void set_dist_offset(int v) override {request(v);}
    bool take_scan(gnd::ScanSample&) override {return false;}
    bool take_rate(float&) override {return false;}
    uint64_t dropped() const override {return 0;}
};
}
int main() {
    try {
        Camera camera; Encoder encoder; Air air; Unit unit; Ground ground;
        ReceiverControls controls(camera,encoder,air,unit,ground);
        Model model; Controller controller(model); auto& state=model.backend;
        const auto now=monotonic_us();
        state.service.connected=true;
        ground.value.link.active=true; ground.value.link.state=gnd::kLinkUp;
        ground.value.link.last_update_ms=now/1000;
        ground.value.radio_fw=1; ground.value.rf_caps=gnd::RF_CAP_POWER;
        camera.value.camera_n=1; unit.value.info.version=true; unit.value.info.kestrel=true;
        unit.value.info.kv_n=1; std::strcpy(unit.value.info.kv[0].key,"model");
        std::strcpy(unit.value.info.kv[0].value,"BETAFPV P1 Air");
        controls.update(state,now);
        check(!state.controls.at("EV").enabled,"P1 settings echoes must not enable missing ISP controls");
        check(state.controls.at("Max Bitrate").enabled,"P1 bitrate is supported");
        controls.apply("Max Bitrate",18000,now);
        check(encoder.calls==1 && encoder.last==18000,"Bitrate uses kbps, not an option index");
        bool rejected=false;
        try {controls.apply("Max Bitrate",18,now);} catch(const std::invalid_argument&) {rejected=true;}
        check(rejected && encoder.calls==1,"Invalid value must not reach transport");
        unit.value.info.kv_n=2;
        std::strcpy(unit.value.info.kv[1].key,"cam_ctrl");
        std::strcpy(unit.value.info.kv[1].value,"ev,wb_lock");++unit.revision;
        controls.update(state,now);
        check(state.controls.at("EV").enabled && state.controls.at("White Balance").enabled,"P1 advertises implemented controls explicitly");
        check(!state.controls.at("Saturation").enabled && !state.controls.at("Rotate").enabled,"Capability list does not enable unrelated controls");
        controls.apply("White Balance",-1,now);
        check(camera.field==air::CameraField::AWB && camera.last==-1,"P1 lock uses its advertised value");
        rejected=false;
        try{controls.apply("White Balance",5200,now);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"Uncalibrated Kelvin presets are not offered on P1");
        camera.value.ev_x10=-7;camera.value.awb=-1;++camera.revision;
        controls.update(state,now);
        model.menu_open=true;model.side=1;model.tab=0;model.selected=4;model.stage=MenuStage::Browse;
        controller.apply_receiver=[&](const std::string& id,int v){return controls.apply(id,v,monotonic_us());};
        check(model.rows(now)[4].value=="-0.7" && model.rows(now)[5].value=="Lock current","Camera readback updates EV and signed WB lock");
        controller.key(Key::Enter);controller.key(Key::Down);controller.key(Key::Enter);
        check(camera.field==air::CameraField::EV && camera.last==-3,"Next edit starts from reported EV");
        check(model.rows(now)[4].value=="-0.7","Request alone must not claim application");
        camera.value.ev_x10=-3;++camera.revision;controls.update(state,now);
        check(model.rows(now)[4].value=="-0.3","Applied camera report completes edit");
        std::strcpy(unit.value.info.kv[1].value,"not_ev,wb_lock_extra");++unit.revision;
        controls.update(state,now);
        check(!state.controls.at("EV").enabled && !state.controls.at("White Balance").enabled,"Capability tokens require an exact match");
        unit.value.info.kv_n=1;++unit.revision;controls.update(state,now);
        check(!state.controls.at("EV").enabled,"Reconnecting to older P1 firmware disables missing controls");
        std::strcpy(unit.value.info.kv[0].value,"Ascent Lite"); ++unit.revision;
        controls.update(state,now);
        controls.apply("White Balance",5200,now);
        check(camera.field==air::CameraField::AWB && camera.last==5200,"White balance uses Kelvin");
        controls.apply("EV",-7,now);
        check(camera.field==air::CameraField::EV && camera.last==-7,"EV uses tenths, including negative values");
        controls.update(state,now);
        state.vrx.connected=true; state.vrx.mcs=5; state.vrx.stamp_us=now;
        model.menu_open=true; model.side=1; model.tab=0; model.selected=1; model.stage=MenuStage::Browse;
        controller.apply_receiver=[&](const std::string& id,int v){return controls.apply(id,v,monotonic_us());};
        controller.key(Key::Enter); controller.key(Key::Down); controller.key(Key::Back);
        check(encoder.calls==1 && model.pending("Max Bitrate"),"Arrows and Back stage without sending");
        controller.key(Key::Enter); controller.key(Key::Enter);
        check(encoder.calls==2 && encoder.last==6000 && !model.pending("Max Bitrate"),"OK applies actual value exactly once");
        check(model.rows(now)[1].value=="AUTO","Queued request is not a hardware readback");
        controller.key(Key::Enter); controller.key(Key::Down);
        ground.value.link.active=false; ++ground.revision; controls.update(state,now);
        controller.tick(now);
        check(model.stage==MenuStage::Browse && !model.pending("Max Bitrate"),"Disconnect cancels pending remote edit");
        check(encoder.calls==2,"Disconnect must not send a stale setting");
        ground.value.link.active=true; ++ground.revision; controls.update(state,now);
        check(model.rows(now)[1].value=="AUTO","Reconnect follows live value without stale preview");
        controller.apply_receiver=[](const std::string&,int)->std::string {throw std::runtime_error("transport down");};
        controller.key(Key::Enter); controller.key(Key::Down); controller.key(Key::Enter);
        check(!model.pending("Max Bitrate") && model.rows(now)[1].value=="AUTO","Failed apply clears preview");
        check(model.feedback.find("transport down")!=std::string::npos,"Apply error is visible");
        controls.update(state,now+6000001);
        check(!state.controls.at("Max Bitrate").enabled && state.air_info.empty(),"Stale radio data disables controls and identity");
        ground.value.link.active=false; ++ground.revision; controls.update(state,now);
        controls.apply("bind",0,now);
        check(ground.calls==1,"Binding available without an air connection");
        std::puts("PASS: receiver values, capability gating, staged edits, disconnect and failure recovery");
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1;}
}
