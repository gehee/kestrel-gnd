#include "receiver_controls.hpp"
#include "system/air/camera.hpp"
#include "system/air/encoder.hpp"
#include "system/air/power.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace kestrel_ui {
void ReceiverControls::update(BackendState& state, uint64_t now) {
    const auto g = ground_.state();
    const bool linked = g.link.active && g.link.state == gnd::kLinkUp &&
        g.link.last_update_ms && now / 1000 >= g.link.last_update_ms && now / 1000 - g.link.last_update_ms < 6000;
    const auto unit = unit_.state();
    auto& details = state.link;
    details.distance_m = linked ? g.dist_m : -1;
    details.bandwidth_step = linked ? g.link.bw_step : -1;
    details.air_power_mw = linked ? g.link.air_pwr_mw : 0;
    details.air_live_dbm = linked ? air_.state().live_dbm : 0;
    details.video_mbps = linked ? g.video_mbps : 0;
    details.link_mbps = linked ? g.link.rx_data_rate_kbps / 1000.0f : 0;
    details.standby = linked && unit.low_power;
    details.vtx_temperature_valid = linked && unit.vtx_temp_valid;
    details.vtx_temperature_c = unit.vtx_cpu_c;
    const std::array<uint32_t,5> versions{camera_.version(),encoder_.version(),air_.version(),unit_.version(),ground_.version()};
    if (initialized_ && versions == versions_ && linked == connected_ && state.service.connected == service_connected_) return;
    service_connected_ = state.service.connected;
    initialized_ = true; versions_ = versions; connected_ = linked;
    const auto c = camera_.state(); const auto e = encoder_.state();
    const auto a = air_.state(); const auto u = unit_.state();
    const bool p1 = u.info.get("model").find("P1 Air") != std::string::npos;
    controls_.clear();
    auto choice = [&](const char* id, int value, bool known, bool supported,
                      std::vector<ReceiverChoice> choices, const char* why = "") {
        auto& control = controls_[id];
        control.value = value; control.known = linked && known;
        control.enabled = linked && known && supported;
        control.choices = std::move(choices);
        control.reason = !linked ? "Connect an air unit." : !supported ? why : !known ? "Waiting for the air unit's settings." : "";
    };
    // Values and labels match VRX Pro's on-wire domains. P1's current native
    // camera backend advertises implemented controls explicitly; never offer its
    // settings-only echoes as proof that a sensor/ISP control exists.
    const char* p1_reason = "The connected P1 camera driver does not implement this control.";
    const auto camera_controls=u.info.get("cam_ctrl");
    auto supports=[&](const char* name) {
        const auto list=","+camera_controls+",";
        return u.info.kestrel && list.find(std::string(",")+name+",")!=std::string::npos;
    };
    // A P1 offers what its cam_ctrl lists: EV, White Balance Auto with Lock
    // current (wb_lock) and the kelvin presets (wb, locked at its tuning's
    // white points), Rotate, Saturation, Contrast, Sharpness and 3D DNR.
    auto p1_has=[&](const char* name){return p1 && supports(name);};
    const bool native_ev=p1_has("ev"), native_lock=p1_has("wb_lock"), native_kelvin=p1_has("wb");
    const bool native_wb=native_lock || native_kelvin;
    bool camera = u.stock || (!p1 && u.info.kestrel && !u.info.get("model").empty());
    // The VRX Pro's own tables (system/air/camera.hpp), by the value on the wire.
    auto table=[](const int* values,const char* const* labels,int n) {
        std::vector<ReceiverChoice> out;
        for(int i=0;i<n;++i) out.push_back({values?values[i]:i,labels[i]});
        return out;
    };
    auto levels=[](int max) {
        std::vector<ReceiverChoice> out;
        for(int k=0;k<=max;++k) out.push_back({k,air::cam_auto_label(k)});
        return out;
    };
    choice("Rotate",c.angle,c.angle_n || c.camera_n,camera || p1_has("rotate"),table(nullptr,air::kRotateLabels,2),p1_reason);
    choice("Scene",c.scene,c.camera_n,camera,table(air::kSceneVals,air::kSceneLabels,air::kSceneCount),p1_reason);
    choice("EV",c.ev_x10,c.camera_n,camera || native_ev,table(air::kEvSteps,air::kEvLabels,air::kEvCount),p1_reason);
    // The VRX Pro's table; a P1 that lists its white balance offers Auto and
    // what it lists: Lock current (wb_lock), the kelvin presets (wb).
    std::vector<ReceiverChoice> wb;
    if(!native_wb) for(int i=0;i<air::kWbCount;++i) wb.push_back({air::wb_kelvin(i),air::wb_label(i)});
    else {
        wb.push_back({0,"Auto"});
        if(native_lock) wb.push_back({-1,"Lock current"});
        if(native_kelvin) for(int i=1;i<air::kWbCount;++i) wb.push_back({air::wb_kelvin(i),air::wb_label(i)});
    }
    choice("White Balance",c.awb,c.camera_n,camera || native_wb,wb,p1_reason);
    if(native_wb && controls_["White Balance"].enabled)
        controls_["White Balance"].reason=native_kelvin && native_lock ?
            "Auto adapts white balance; Lock current holds the current colors, a Kelvin preset the camera tuning's for that light, across restarts." :
            native_lock ? "Auto adapts white balance; Lock current holds the current colors across restarts." :
            "Auto adapts white balance; a Kelvin preset holds the camera tuning's colors for that light.";
    choice("Saturation",c.sat,c.camera_n,camera || p1_has("sat"),levels(air::kCamSatMax),p1_reason);
    choice("Contrast",c.contrast,c.camera_n,camera || p1_has("contrast"),levels(air::kCamContrastMax),p1_reason);
    choice("Sharpness",c.sharp,c.camera_n,camera || p1_has("sharpness"),levels(air::kCamSharpMax),p1_reason);
    choice("3D DNR",c.dnr3d,c.dnr_n || c.camera_n,camera || p1_has("3dnr"),table(air::kDnrVals,air::kDnrLabels,air::kDnrCount),p1_reason);
    choice("Focus Mode",c.focus,c.camera_n,camera,{{0,"OFF"},{1,"ON"}},p1_reason);
    choice("Video Mode",0,true,false,{{0,"1920x1080 @ 60"}},"This native display/decoder currently supports 1080p60 only.");
    choice("Max Bitrate",e.max_kbps,true,u.info.kestrel,table(air::kArMaxBrVals,air::kArMaxBrLabels,air::kArMaxBrCount),
           "Requires an fpvOS air unit.");
    auto offer=ar_pwr_offered(a.prj,a.fine); std::vector<ReceiverChoice> powers;
    for(int i=0;i<offer.n;++i) powers.push_back({offer.mw[i],kArPwrLevels[ar_pwr_index(offer.mw[i])].label});
    choice("power",a.tx_power_want_mw,a.tx_power_want_mw>0,g.rf_caps & gnd::RF_CAP_POWER,powers,"The radio does not expose power control.");
    choice("standby",u.standby,u.standby_n,true,{{0,"OFF"},{1,"ON"}});
    std::vector<ReceiverChoice> mcs{{-1,"AUTO"}};
    for(int i=0;i<12;++i) mcs.push_back({i,"MCS "+std::to_string(i)});
    choice("max_mcs",g.max_mcs,true,g.radio_fw==1,mcs,"Requires Open8030.");
    auto& fw=controls_["radio_firmware"]; fw.known=true; fw.text=g.radio_fw?"OPEN8030":"STOCK";
    fw.reason="Native SDIO firmware is supplied by fpvOS; live firmware replacement is not supported.";
    auto& bind=controls_["bind"]; bind.known=true; bind.enabled=state.service.connected; bind.action=true;
    bind.text=g.bind_state==gnd::BIND_RUNNING?"ENTER: CANCEL":"ENTER: BIND";
    if(g.bind_state==gnd::BIND_RUNNING) bind.reason="Binding: "+std::to_string(g.bind_secs_left)+" seconds remaining.";
    else if(g.bind_state==gnd::BIND_FAILED) bind.reason="No air unit found. Press OK to retry.";
    else if(g.bind_state==gnd::BIND_OK) bind.reason="Binding succeeded.";
    state.controls=controls_;
    state.air_info.clear();
    if(linked && u.info.version) {
        state.air_info["model"]=u.info.get("model");
        if(state.air_info["model"].empty()) state.air_info["model"]=ar_air_name(a.prj);
        state.air_info["hw_version"]=u.info.hw<0?"--":std::to_string(u.info.hw)+" / RF "+std::to_string(u.info.rf_hw);
        char serial[48]; std::snprintf(serial,sizeof(serial),"%s%06X",ar_air_sn_prefix(a.prj),u.info.chipid);
        state.air_info["sn"]=p1 && !u.info.chipid ? "--" : serial;
        state.air_info["sw_version"]=std::to_string(u.info.stock[0])+"."+std::to_string(u.info.stock[1])+"."+std::to_string(u.info.stock[2]);
        for(int i=0;i<u.info.kv_n;++i) state.air_info[u.info.kv[i].key]=u.info.kv[i].value;
    }
}
std::string ReceiverControls::apply(const std::string& id,int value,uint64_t now) {
    BackendState current; current.service.connected=service_connected_; update(current,now); initialized_=false;
    const auto at=controls_.find(id);
    if(at==controls_.end() || !at->second.enabled) throw std::runtime_error(at==controls_.end()?"Control unavailable":at->second.reason);
    if(!at->second.action && std::none_of(at->second.choices.begin(),at->second.choices.end(),[&](const auto& x){return x.value==value;}))
        throw std::invalid_argument("Unsupported control value");
    if(id=="bind") {ground_.bind();return "Binding requested";}
    if(id=="Max Bitrate") encoder_.set_max_bitrate(value);
    else if(id=="power") air_.set_tx_power(value);
    else if(id=="standby") unit_.set_standby(value!=0);
    else if(id=="max_mcs") ground_.set_max_mcs(value);
    else {
        const std::map<std::string,air::CameraField> fields{{"Rotate",air::CameraField::ANGLE},{"Scene",air::CameraField::SCENE},
            {"EV",air::CameraField::EV},{"White Balance",air::CameraField::AWB},{"Saturation",air::CameraField::SAT},
            {"Contrast",air::CameraField::CONTRAST},{"Sharpness",air::CameraField::SHARPNESS},{"3D DNR",air::CameraField::DNR3D},{"Focus Mode",air::CameraField::FOCUS}};
        auto f=fields.find(id); if(f==fields.end()) throw std::invalid_argument("Unknown receiver control");
        camera_.set(f->second,value);
    }
    // A queued transport operation is not an acknowledgement from the hardware.
    return "Change requested; waiting for receiver/air-unit state";
}
}
