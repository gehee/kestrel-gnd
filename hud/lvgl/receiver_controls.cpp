#include "receiver_controls.hpp"
#include "system/air/camera.hpp"
#include "system/air/encoder.hpp"
#include "system/air/power.hpp"
#include "system/air/bandwidth.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace kestrel_ui {
void ReceiverControls::update(BackendState& state, uint64_t now) {
    const auto g = ground_.state();
    const bool linked = !g.spectator.mode && g.link.active && g.link.state == gnd::kLinkUp &&
        g.link.last_update_ms && now / 1000 >= g.link.last_update_ms && now / 1000 - g.link.last_update_ms < 6000;
    const auto unit = unit_.state();
    auto& details = state.link;
    details.distance_m = linked ? g.dist_m : -1;
    details.bandwidth_step = g.spectator.mode || linked ? g.link.bw_step : -1;
    details.air_power_mw = linked ? g.link.air_pwr_mw : 0;
    details.air_live_dbm = linked ? air_.state().live_dbm : 0;
    details.video_mbps = linked || g.spectator.mode ? g.video_mbps : 0;
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
    // Values and labels match VRX Pro's on-wire domains. The camera rows the air
    // unit applies, as the VRX Pro's menu has them (UnitState::cam_settings):
    // those its info lists (cam_ctrl=: the Lites all of them, with White Balance's
    // Lock current; the P1 EV, White Balance Auto with Lock current and the
    // kelvin presets, Rotate, Saturation, Contrast, Sharpness, 3D DNR), all but
    // Lock current where it lists none (stock, an older kestrel-air). A kestrel-air
    // has said once its info came; a P1 that lists none applies none (its
    // settings echo back all the same: no proof the sensor has them).
    const char* cam_reason = "Not on this air unit: its camera has no such setting.";
    const bool cam_said = u.stock || (u.info.kestrel && !u.info.get("model").empty());
    const bool cam_listed = !u.info.get("cam_ctrl").empty() || !u.info.get("cam").empty();
    const uint16_t cam = !cam_said || (p1 && !cam_listed) ? 0 : u.cam_settings;
    auto has=[&](uint16_t bit){return (cam & bit) == bit;};
    const bool wb_lock=cam && air::cam_wb_lock(cam), wb_kelvin=cam && air::cam_wb_kelvin(cam);
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
    choice("Rotate",c.angle,c.angle_n || c.camera_n,has(air::kCamRotate),table(nullptr,air::kRotateLabels,2),cam_reason);
    // Scene: not listed where the air unit says it has none (the P1 Air: no
    // other tuning to switch to), as the VRX Pro's menu leaves it out.
    if(!cam || has(air::kCamScene))
        choice("Scene",c.scene,c.camera_n,has(air::kCamScene),table(air::kSceneVals,air::kSceneLabels,air::kSceneCount),cam_reason);
    choice("EV",c.ev_x10,c.camera_n,has(air::kCamEv),table(air::kEvSteps,air::kEvLabels,air::kEvCount),cam_reason);
    // Auto, then what the air unit takes: Lock current (wb_lock, kestrel-air), the
    // kelvin presets (wb, or an air unit that lists nothing) - the VRX Pro's.
    std::vector<ReceiverChoice> wb{{0,"Auto"}};
    if(wb_lock) wb.push_back({-1,"Lock current"});
    if(wb_kelvin) for(int i=1;i<air::kWbCount;++i) wb.push_back({air::wb_kelvin(i),air::wb_label(i)});
    choice("White Balance",c.awb,c.camera_n,has(air::kCamWb),wb,cam_reason);
    if(wb_lock && controls_["White Balance"].enabled)
        controls_["White Balance"].reason=wb_kelvin ?
            "Auto adapts white balance; Lock current holds the current colors, a Kelvin preset the camera tuning's for that light, across restarts." :
            "Auto adapts white balance; Lock current holds the current colors across restarts.";
    else if(cam!=air::kCamAll && wb_kelvin && controls_["White Balance"].enabled)
        controls_["White Balance"].reason="Auto adapts white balance; a Kelvin preset holds the camera tuning's colors for that light.";
    choice("Saturation",c.sat,c.camera_n,has(air::kCamSat),levels(air::kCamSatMax),cam_reason);
    choice("Contrast",c.contrast,c.camera_n,has(air::kCamContrast),levels(air::kCamContrastMax),cam_reason);
    choice("Sharpness",c.sharp,c.camera_n,has(air::kCamSharpness),levels(air::kCamSharpMax),cam_reason);
    choice("3D DNR",c.dnr3d,c.dnr_n || c.camera_n,has(air::kCamDnr3d),table(air::kDnrVals,air::kDnrLabels,air::kDnrCount),cam_reason);
    choice("Focus Mode",c.focus,c.camera_n,has(air::kCamFocus),{{0,"OFF"},{1,"ON"}},cam_reason);
    // The air unit's own video modes it can switch to: those its info lists
    // (modes=, the P1 Air: 720p90 and 1080p60), else the universal ones
    // (720p60/100, 1080p60/100) plus any its features add (864p120/144 on a
    // Lite+, 720p90 on a P1 Air that lists none). Screen Mode Auto follows the
    // stream; 1080p100 is captured and decoded at 100 fps but this HDMI output
    // tops out at 1080p60.
    // The mode it runs stays in the list whatever its feature bits say (they
    // arrive after the mode at link-up), so the row names it.
    std::vector<ReceiverChoice> modes;
    int offered = 0;
    for (int i = 0; i < int(video_modes_.size()); i++) {
        const auto& v = video_modes_[i];
        const bool runs = air::video_mode_offered(u.modes_said, u.video_modes, u.features, i, v.need);
        offered += runs;
        if (runs || i == c.video_mode) modes.push_back({i, v.name});
    }
    // A list of one (a P1 Air whose sensor driver has no 720p modes) is a
    // reading: nothing else to pick.
    const bool one_mode = u.modes_said && offered <= 1;
    choice("Video Mode", c.video_mode, c.mode_n && c.video_mode >= 0,
           (u.info.kestrel || u.stock) && !one_mode, modes,
           !one_mode ? "Requires a recognized air unit." :
           offered ? "This air unit runs this mode only: there is no other to pick." :
                     "This air unit lists no video mode this receiver has.");
    choice("Max Bitrate",e.max_kbps,true,u.info.kestrel,table(air::kArMaxBrVals,air::kArMaxBrLabels,air::kArMaxBrCount),
           "Requires an fpvOS air unit.");
    // Strips: only where both ends can (the feature bit), not listed otherwise.
    if(linked && (u.features & air::kFeatStrips))
        choice("Strips",e.strips,true,true,table(air::kArStripsVals,air::kArStripsLabels,air::kArStripsCount));
    // Max Exposure: only where the air unit takes it live (its info's maxexp=1).
    if(linked && (u.features & air::kFeatMaxExp))
        choice("Max Exposure",e.max_exp_us,true,true,table(air::kArMaxExpVals,air::kArMaxExpLabels,air::kArMaxExpCount));
    // Share the legacy menu's mode/level mapping and per-air-unit limits: the
    // saved level fitted to this air unit's list, as the link sends it. Known
    // once the air unit has said what it is (prj), as in the legacy menu.
    const int power_now=ar_pwr_fit(a.prj,a.tx_power_want_mw,a.fine);
    const bool power_known=a.tx_power_want_mw>0 && a.prj>0;
    const bool automatic=ar_pwr_is_auto(power_now);
    int tops[kArPwrTopsMax];
    const int count=ar_pwr_tops(a.prj,a.fine,automatic,tops);
    std::vector<ReceiverChoice> powers, power_modes;
    for(int i=0;i<count;++i) powers.push_back({tops[i],kArPwrLevels[ar_pwr_index(tops[i])].label});
    for(int mode=0;mode<2;++mode)
        if(ar_pwr_pick(a.prj,a.fine,mode,ar_pwr_top(power_now))>=0)
            power_modes.push_back({mode,mode?"AUTO":"FIXED"});
    choice("power_mode",automatic,power_known,g.rf_caps & gnd::RF_CAP_POWER,power_modes,
           "The radio does not expose power control.");
    choice("power",ar_pwr_top(power_now),power_known,g.rf_caps & gnd::RF_CAP_POWER,powers,
           "The radio does not expose power control.");
    choice("max_bw",g.max_bw_mhz,true,g.radio_fw==1,table(kArMaxBwVals,kArMaxBwLabels,kArMaxBwCount),"Requires Open8030.");

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
    if (g.spectator_supported) {
        auto local=[&](const char* id,int value,std::vector<ReceiverChoice> choices,const char* reason) {
            auto& c=controls_[id]; c={}; c.known=true; c.enabled=state.service.connected;
            c.value=value; c.choices=std::move(choices); c.reason=reason;
        };
        local("receiver_mode",g.spectator.mode,{{0,"PILOT"},{1,"SPECTATOR"}},
              "Spectator only receives video. Match the pilot's frequency and stream ID; the pilot controls the air unit.");
        std::vector<ReceiverChoice> frequencies,ids;
        for(int mhz=4915;mhz<=6400;++mhz) frequencies.push_back({mhz,std::to_string(mhz)+" MHz"});
        for(int id=0;id<256;++id) {char label[8];std::snprintf(label,sizeof label,"%02X",id);ids.push_back({id,label});}
        local("spectator_frequency",g.spectator.frequency_mhz,std::move(frequencies),"Initial receive frequency. Subsequent received hop controls are followed silently.");
        local("spectator_bandwidth",g.spectator.bandwidth_mhz,{{5,"5 MHz"},{10,"10 MHz"},{20,"20 MHz"},{40,"40 MHz"}},"Initial bandwidth. Subsequent received width controls are followed silently.");
        local("spectator_stream_id",g.spectator.stream_id,std::move(ids),"XOR of the four air-unit radio MAC bytes. This short filter is not authentication.");
        auto& frequency=controls_["stream_frequency"]; frequency.known=true;
        // Pilot status reads user 0: its RX bank is parked at 2100 MHz.
        // Use the same active-channel field as the pilot HUD.
        const int mhz=g.spectator.mode?g.link.rx_freq:g.link.tx_freq;
        frequency.text=mhz>0?std::to_string(mhz)+" MHz":"--";
        auto& bandwidth=controls_["stream_bandwidth"]; bandwidth.known=true;
        bandwidth.text=g.link.bw_step>=2 && g.link.bw_step<=5?std::to_string(5<<(g.link.bw_step-2))+" MHz":"--";
        auto& pilot_id=controls_["pilot_stream_id"]; pilot_id.known=true; pilot_id.text="--";
        unsigned mac[4]{};
        if(linked && std::sscanf(g.link.peer_mac,"%x:%x:%x:%x",&mac[0],&mac[1],&mac[2],&mac[3])==4 &&
           std::all_of(mac,mac+4,[](unsigned v){return v<=255;})) {
            char text[8]; std::snprintf(text,sizeof text,"%02X",mac[0]^mac[1]^mac[2]^mac[3]);
            pilot_id.text=text;
        }
        pilot_id.reason="Copy this ID to a spectator listening on the same channel and width.";
        auto& status=controls_["spectator_status"];status.known=true;
        const char* labels[]={"IDLE","WAITING FOR VIDEO","RECEIVING","UNAVAILABLE"};
        status.text=labels[std::clamp(g.spectator_status,0,3)];
        status.reason="The air periodically supplies recovery pictures. Missed hop controls may require manual retuning; no uplink requests are sent.";
        if(g.spectator.mode) {
            bind.enabled=false;bind.reason="Binding is disabled in spectator mode.";
            details.video_mbps=g.video_mbps;details.bandwidth_step=g.link.bw_step;
        }
    }
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
        if(u.info.proto) state.air_info["proto"]=std::to_string(u.info.proto);
        std::string caps;
        for(int i=0;i<u.info.caps_n;++i) caps+=std::string(i?", ":"")+u.info.caps[i];
        if(!caps.empty()) state.air_info["caps"]=caps;
    }
}
std::string ReceiverControls::apply(const std::string& id,int value,uint64_t now) {
    BackendState current; current.service.connected=service_connected_; update(current,now); initialized_=false;
    const auto at=controls_.find(id);
    if(at==controls_.end() || !at->second.enabled) throw std::runtime_error(at==controls_.end()?"Control unavailable":at->second.reason);
    if(!at->second.action && std::none_of(at->second.choices.begin(),at->second.choices.end(),[&](const auto& x){return x.value==value;}))
        throw std::invalid_argument("Unsupported control value");
    if(id=="receiver_mode" || id=="spectator_frequency" || id=="spectator_bandwidth" || id=="spectator_stream_id") {
        auto s=ground_.state().spectator;
        if(id=="receiver_mode") s.mode=value;
        else if(id=="spectator_frequency") s.frequency_mhz=value;
        else if(id=="spectator_bandwidth") s.bandwidth_mhz=value;
        else s.stream_id=value;
        if(!gnd::valid(s)) throw std::invalid_argument("Invalid spectator configuration");
        ground_.set_spectator(s);
        return "Receiver mode/tuning saved; change requested";
    }
    if(id=="bind") {ground_.bind();return "Binding requested";}
    if(id=="Max Bitrate") encoder_.set_max_bitrate(value);
    else if(id=="allow_spectators") unit_.set_spectators(value != 0);
    else if(id=="Strips") encoder_.set_strips(value);
    else if(id=="Max Exposure") encoder_.set_max_exposure(value);
    else if(id=="Video Mode") camera_.set_mode(value);
    else if(id=="power" || id=="power_mode") {
        const auto a=air_.state();
        const int now_mw=ar_pwr_fit(a.prj,a.tx_power_want_mw,a.fine);
        const bool mode=id=="power_mode" ? value!=0 : ar_pwr_is_auto(now_mw);
        const int top=id=="power" ? value : ar_pwr_top(now_mw);
        const int encoded=ar_pwr_pick(a.prj,a.fine,mode,top);
        if(encoded<0) throw std::invalid_argument("Unsupported TX power mode");
        air_.set_tx_power(encoded);
    }
    else if(id=="max_bw") ground_.set_max_bandwidth(value);
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
