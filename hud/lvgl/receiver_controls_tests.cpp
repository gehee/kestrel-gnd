#include "receiver_controls.hpp"
#include "controller.hpp"
#include "system/air/power.hpp"
#include <cstdio>
#include <algorithm>
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
struct Encoder : Fake<air::Encoder,air::EncoderState> { Ticket set_max_bitrate(int v) override { return request(v); } Ticket set_max_exposure(int v) override { return request(v); } Ticket set_strips(int v) override { return request(v); } };
struct Air : Fake<air::Radio,air::RadioState> { bool take_resend(air::Resend&) override {return false;} uint64_t resends_dropped() const override {return 0;} Ticket set_tx_power(int v) override { return request(v); } };
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
    Ticket set_channel_auto(bool) override {return {};}
    void region_changed() override {}
    void search_channels() override {request(0);}
    void scan_screen(bool) override {}
    void bind() override {request(0);}
    bool take_scan(gnd::ScanSample&) override {return false;}
    bool take_rate(float&) override {return false;}
    uint64_t dropped() const override {return 0;}
};
}
int main() {
    try {
        Camera camera; Encoder encoder; Air air; Unit unit; Ground ground;
        ReceiverControls controls(camera,encoder,air,unit,ground,
                                  {{"720p60",0},{"864p120",0x40},{"864p144",0x100},{"1080p60",0}});
        Model model; Controller controller(model); auto& state=model.backend;
        const auto now=monotonic_us();
        state.service.connected=true;
        ground.value.link.active=true; ground.value.link.state=gnd::kLinkUp;
        ground.value.link.last_update_ms=now/1000;
        ground.value.radio_fw=1; ground.value.rf_caps=gnd::RF_CAP_POWER;
        camera.value.camera_n=1; unit.value.info.version=true; unit.value.info.kestrel=true;
        unit.value.info.kv_n=1; std::strcpy(unit.value.info.kv[0].key,"model");
        std::strcpy(unit.value.info.kv[0].value,"BETAFPV P1 Air");
        ground.value.dist_m=42; ground.value.link.bw_step=4;
        ground.value.link.air_pwr_mw=101; ground.value.video_mbps=4.5f;
        ground.value.link.rx_data_rate_kbps=9700;
        unit.value.low_power=true; unit.value.vtx_temp_valid=false;
        controls.update(state,now);
        check(state.link.distance_m==42 && state.link.bandwidth_step==4 && state.link.air_power_mw==101 &&
              state.link.video_mbps==4.5f && state.link.link_mbps==9.7f && state.link.standby && !state.link.vtx_temperature_valid,
              "HUD consumes calibrated native units and actual standby without fabricating temperature");
        check(!state.controls.at("EV").enabled,"P1 settings echoes must not enable missing ISP controls");
        {
            // Video Mode: the modes any air unit has, those its feature bits
            // add, and always the one it runs (the bits arrive after it).
            auto offered=[&]{ std::string s; for(const auto& m:state.controls.at("Video Mode").choices) s+=m.label+" "; return s; };
            auto changed=[&]{ ++camera.revision; ++unit.revision; controls.update(state,now); };
            camera.value.mode_n=1; camera.value.video_mode=0; unit.value.features=0; changed();
            check(offered()=="720p60 1080p60 ","Video Mode offers only what any air unit has without feature bits");
            unit.value.features=0x40; changed();
            check(offered()=="720p60 864p120 1080p60 ","Video Mode adds the modes the air unit's features give");
            unit.value.features=0; camera.value.video_mode=2; changed();
            check(offered()=="720p60 864p144 1080p60 ","Video Mode names the running mode before its feature bit arrives");
            camera.value.mode_n=0; camera.value.video_mode=0; unit.value.features=0; changed();
        }
        {
            // Strips: listed only where both ends can (the feature bit), after
            // Max Bitrate, the air unit's count by value.
            check(!state.controls.count("Strips"),"Strips not listed without the feature bit");
            unit.value.features=air::kFeatStrips; encoder.value.strips=2; ++unit.revision; ++encoder.revision;
            controls.update(state,now);
            const auto& strips=state.controls.at("Strips");
            check(strips.enabled && strips.choices.size()==4 && strips.choices[0].label=="OFF" && strips.choices[3].value==4,
                  "Strips offers OFF/2/3/4 where both ends can");
            model.tab=0;
            const auto rows=model.rows(now);
            check(rows[2].id=="Strips" && rows[2].value=="2" && rows[2].editable,"Strips follows Max Bitrate and shows the count");
            controls.apply("Strips",3,now);
            check(encoder.calls==1 && encoder.last==3,"Strips asks the encoder for the count");
            encoder.calls=0; unit.value.features=0; ++unit.revision; controls.update(state,now);
            check(!state.controls.count("Strips"),"Strips goes when the bit does");
        }
        check(state.controls.at("Max Bitrate").enabled,"P1 bitrate is supported");
        {
            // The VRX Pro's tables, by the value on the wire.
            const auto& rotate=state.controls.at("Rotate").choices;
            check(rotate.size()==2 && rotate[0].value==0 && rotate[1].value==1 && rotate[1].label=="180",
                  "Rotate sends 0/1 as the VRX Pro does, shown as 0/180");
            const auto& wb=state.controls.at("White Balance").choices;
            check(wb.size()==32 && wb.front().label=="Auto" && wb[1].value==4000 && wb.back().value==7000 && wb.back().label=="7000K",
                  "White balance is Auto plus 4000-7000 K");
            const auto& rates=state.controls.at("Max Bitrate").choices;
            check(rates.size()==13 && rates.front().label=="AUTO" && rates.back().value==40000,"Bitrate list is the VRX Pro's");
        }
        // Radio choices use wire values, preserve power mode, and reject
        // unsupported levels rather than exposing a combined mode/level list.
        air.value.prj=AR_AIR_LITE; air.value.tx_power_want_mw=25; ++air.revision;
        ground.value.max_bw_mhz=40; ++ground.revision; controls.update(state,now);
        check(state.controls.at("max_bw").choices.size()==5 && state.controls.at("max_bw").value==40,
              "Open8030 exposes AUTO and 5/10/20/40 MHz");
        state.vrx.connected=true;state.vrx.mcs=5;state.vrx.stamp_us=now;model.tab=1;
        const auto radio_rows=model.rows(now);
        auto has_row=[&](const char* id){return std::any_of(radio_rows.begin(),radio_rows.end(),
            [&](const auto& row){return row.id==id;});};
        check(has_row("max_bw") && has_row("power_mode"),"Native radio menu exposes both new controls");
        model.tab=0;
        controls.apply("max_bw",20,now);
        check(ground.last==20,"Bandwidth sends MHz, not menu index");
        controls.apply("max_bw",0,now); check(ground.last==0,"AUTO sends zero");
        bool bad_bw=false;
        try {controls.apply("max_bw",80,now);} catch(const std::invalid_argument&) {bad_bw=true;}
        check(bad_bw,"Reject unsupported bandwidth");
        controls.apply("power_mode",1,now);
        check(air.last==101,"Stock Lite AUTO selects its supported 100mW cap");
        air.value.tx_power_want_mw=101; ++air.revision; controls.update(state,now);
        check(state.controls.at("power").choices.size()==1 && state.controls.at("power").value==100,
              "Stock AUTO exposes only supported cap, separate from mode");
        controls.apply("power_mode",0,now); check(air.last==100,"FIXED preserves AUTO cap");
        air.value.prj=AR_AIR_P1;air.value.fine=1;air.value.tx_power_want_mw=151;++air.revision;
        controls.update(state,now); controls.apply("power",200,now);
        check(air.last==201,"P1 power level edit preserves AUTO encoding");
        bool bad_power=false;
        try {controls.apply("power",500,now);} catch(const std::invalid_argument&) {bad_power=true;}
        check(bad_power,"P1 never offers unsupported 500mW");
        ground.value.radio_fw=0;++ground.revision;controls.update(state,now);
        check(!state.controls.at("max_bw").enabled,"Native width control requires Open8030");
        ground.value.radio_fw=1;ground.value.link.active=false;++ground.revision;controls.update(state,now);
        check(!state.controls.at("max_bw").enabled && !state.controls.at("power_mode").enabled,
              "Disconnected radio cannot apply power or bandwidth");
        ground.value.link.active=true;++ground.revision;controls.update(state,now);
        ground.calls=0;air.calls=0;
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
        std::strcpy(unit.value.info.kv[1].value,"rotate,ev,wb,wb_lock,sat,contrast,sharpness,3dnr");++unit.revision;
        controls.update(state,now);
        check(state.controls.at("Rotate").enabled && state.controls.at("Saturation").enabled &&
              state.controls.at("Contrast").enabled && state.controls.at("Sharpness").enabled &&
              state.controls.at("3D DNR").enabled,"P1 advertising its ISP settings enables them");
        check(!state.controls.at("Scene").enabled && !state.controls.at("Focus Mode").enabled,"Unlisted P1 controls stay unavailable");
        controls.apply("White Balance",5200,now);
        check(camera.field==air::CameraField::AWB && camera.last==5200,"P1 advertising wb takes Kelvin presets");
        controls.apply("White Balance",-1,now);
        check(camera.field==air::CameraField::AWB && camera.last==-1,"P1 keeps Lock current beside Kelvin presets");
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
