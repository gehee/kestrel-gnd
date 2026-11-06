#include "receiver_controls.hpp"
#include "system/air/power.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace kestrel_ui {
void ReceiverControls::update(BackendState& state, uint64_t now) {
    const auto g = ground_.state();
    const bool linked = g.link.active && g.link.state == gnd::kLinkUp &&
        g.link.last_update_ms && now / 1000 >= g.link.last_update_ms && now / 1000 - g.link.last_update_ms < 6000;
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
    // camera backend does not implement these ISP controls; never offer its
    // settings-only echoes as proof that a sensor/ISP control exists.
    const char* p1_reason = "The connected P1 camera driver does not implement this control.";
    bool camera = u.stock || (!p1 && u.info.kestrel && !u.info.get("model").empty());
    choice("Rotate",c.angle,c.angle_n || c.camera_n,camera,{{0,"0"},{180,"180"}},p1_reason);
    choice("Scene",c.scene,c.camera_n,camera,{{0,"Race"},{2,"Standard"}},p1_reason);
    choice("EV",c.ev_x10,c.camera_n,camera,{{-10,"-1.0"},{-7,"-0.7"},{-3,"-0.3"},{0,"0"},{3,"+0.3"},{7,"+0.7"},{10,"+1.0"}},p1_reason);
    std::vector<ReceiverChoice> wb{{0,"Auto"}}, level{{0,"Auto"}};
    for(int k=4000;k<=7000;k+=100) wb.push_back({k,std::to_string(k)+"K"});
    for(int k=1;k<=10;++k) level.push_back({k,std::to_string(k)});
    choice("White Balance",c.awb,c.camera_n,camera,wb,p1_reason);
    choice("Saturation",c.sat,c.camera_n,camera,level,p1_reason);
    choice("Contrast",c.contrast,c.camera_n,camera,level,p1_reason);
    choice("Sharpness",c.sharp,c.camera_n,camera,level,p1_reason);
    choice("3D DNR",c.dnr3d,c.dnr_n || c.camera_n,camera,{{0,"Off"},{2,"Low"},{3,"Mid"},{4,"High"},{5,"Auto"}},p1_reason);
    choice("Focus Mode",c.focus,c.camera_n,camera,{{0,"OFF"},{1,"ON"}},p1_reason);
    choice("Video Mode",0,true,false,{{0,"1920x1080 @ 60"}},"This native display/decoder currently supports 1080p60 only.");
    std::vector<ReceiverChoice> rates{{0,"AUTO"}};
    for(int kbps : {6000,8000,10000,12000,15000,18000,20000,22000,25000,30000,35000,40000})
        rates.push_back({kbps,std::to_string(kbps/1000)+" Mbps"});
    choice("Max Bitrate",e.max_kbps,true,u.info.kestrel,rates,"Requires an fpvOS air unit.");
    auto offer=ar_pwr_offered(a.prj); std::vector<ReceiverChoice> powers;
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
    auto& distance=controls_["distance"]; distance.known=g.dist_raw>=0; distance.enabled=linked && distance.known; distance.action=true;
    distance.text="ENTER: ZERO HERE"; distance.reason="Current offset: "+std::to_string(g.dist_offset)+" m.";
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
    if(id=="distance") {ground_.set_dist_offset(ground_.state().dist_raw);return "Distance calibration requested";}
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
