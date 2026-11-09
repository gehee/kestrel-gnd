#pragma once
#include "hud/lvgl/backend_state.hpp"
#include "region/zones.hpp"
#include "system/gnd/radio.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace ascent {
// Application composition: region policy is shared with VRX Pro; the LVGL
// frontend receives only values and choices and never calls platform services.
class RegionMenu {
public:
    bool owns(const std::string& id) const { return id=="area" || id=="country" || id=="timezone" || id=="licence"; }
    void update(kestrel_ui::BackendState& state) const {
        using kestrel_ui::ReceiverChoice;
        auto add=[&](const char* id,int value,std::vector<ReceiverChoice> choices) {
            auto& c=state.controls[id]; c={}; c.known=true; c.enabled=true;
            c.value=value; c.choices=std::move(choices);
        };
        const int selected=zones::selected();
        const int area=selected<0?-1:zones::zone(selected).area;
        std::vector<ReceiverChoice> areas{{-1,"NOT SET"}}, countries, tz;
        for(int i=0;i<zones::area_count();++i) areas.push_back({i,zones::area_name(i)});
        add("area",area,std::move(areas));
        add("licence",zones::licensed(),{{0,"NO"},{1,"YES"}});
        if(selected>=0) {
            const int country=zones::zone(selected).country;
            for(int c:zones::countries_in(area)) countries.push_back({c,zones::country(c).name});
            for(int z:zones::zones_in(area,country)) tz.push_back({z,zones::zone(z).city});
            add("country",country,std::move(countries));
            add("timezone",selected,std::move(tz));
            state.controls["timezone"].reason=zones::zone(selected).name;
        } else {
            add("country",-1,{{-1,"NOT SET"}});
            add("timezone",-1,{{-1,"NOT SET"}});
            for(const char* id:{"country","timezone"}) {
                state.controls[id].enabled=false; state.controls[id].reason="Choose an area first.";
            }
        }
    }
    std::string apply(const std::string& id,int value) const {
        kestrel_ui::BackendState state; update(state);
        const auto at=state.controls.find(id);
        if(at==state.controls.end() || !at->second.enabled ||
           std::none_of(at->second.choices.begin(),at->second.choices.end(),[&](const auto& v){return v.value==value;}))
            throw std::invalid_argument("Region choices changed; reopen this setting");
        if(id=="licence") zones::set_licensed(value!=0);
        else if(id=="area") {
            if(value<0) zones::select(-1);
            else if(zones::selected()<0 || zones::zone(zones::selected()).area!=value) {
                const auto countries=zones::countries_in(value);
                if(countries.empty()) throw std::runtime_error("No countries in this area");
                zones::select(zones::zones_in(value,countries.front()).front());
            }
        } else if(id=="country") {
            const auto& current=zones::zone(zones::selected());
            if(current.country!=value) zones::select(zones::zones_in(current.area,value).front());
        } else zones::select(value);
        zones::apply_pending_tz();
        return "Region preference applied";
    }
};
class ChannelMenu {
public:
    explicit ChannelMenu(gnd::Radio& radio) : radio_(radio) {}
    void update(kestrel_ui::BackendState& state,uint64_t now,bool open) {
        if(open!=open_) {radio_.scan_screen(open); open_=open;}
        gnd::ScanSample sample;
        while(radio_.take_scan(sample)) {
            scan_=sample;
            for(int i=0;i<std::clamp(sample.chan_num,0,64);++i) {
                const int freq=sample.freq_mhz[i], power=sample.power_dbm[i];
                auto& peak=peaks_[freq];
                if(!peak.second || now-peak.second>30000000 || power>=peak.first) peak={power,now};
            }
        }
        const auto radio=radio_.state();
        auto& control=state.controls["channel"]; control={}; control.known=true; control.action=true;
        const bool fresh=scan_.last_update_ms && now/1000>=scan_.last_update_ms && now/1000-scan_.last_update_ms<6000;
        control.enabled=state.service.connected && state.vrx.link_valid(now) && fresh && (radio.rf_caps&gnd::RF_CAP_CHAN);
        control.text=radio.chan_auto?"AUTO":"MANUAL";
        control.reason=fresh?"OK opens the live channel scan.":"Waiting for a channel scan from the radio.";
        if(!state.service.connected || !state.vrx.link_valid(now)) control.reason="Connect an air unit.";
        else if(!(radio.rf_caps&gnd::RF_CAP_CHAN)) control.reason="This radio does not expose channel selection.";
        // Scan bars update at 14 Hz, numeric labels at a bounded rate in View.
        auto& result=state.channels;
        const int bw=std::max(1,int(std::lround(std::atof(gnd::bw_step_label(radio.link.bw_step)))));
        const int country=zones::selected_country();
        if(scan_.last_update_ms!=last_scan_ || result.fresh!=fresh || result.current_mhz!=radio.link.tx_freq ||
           result.automatic!=radio.chan_auto || generation_!=zones::generation() || licensed_!=zones::licensed() || bw_!=bw) {
            result.fresh=fresh; result.current_mhz=radio.link.tx_freq; result.automatic=radio.chan_auto;
            result.readings.clear(); ++result.revision;
            for(int i=0;i<std::clamp(scan_.chan_num,0,64);++i) {
                const int freq=scan_.freq_mhz[i], power=scan_.power_dbm[i];
                if(freq<4000 || freq>7000 || power>0 || power< -160) continue;
                const auto v=zones::judge(country,freq,bw,zones::licensed());
                std::string rule;
                switch(v.state) {
                case zones::kAllowed: rule=v.amateur?"OK - AMATEUR":"OK"; break;
                case zones::kIndoor: rule="INDOOR ONLY"; break;
                case zones::kLicence: rule="LICENCE NEEDED"; break;
                case zones::kNotAllowed: rule="NOT ALLOWED"; break;
                default: rule="RULES UNKNOWN"; break;
                }
                if(v.cap_mw) rule+=" / "+std::to_string(v.cap_mw)+" mW";
                result.readings.push_back({freq,power,peaks_[freq].first,v.state==zones::kAllowed || v.state==zones::kNoRules,rule});
            }
            std::sort(result.readings.begin(),result.readings.end(),[](const auto& a,const auto& b){return a.mhz<b.mhz;});
            last_scan_=scan_.last_update_ms; generation_=zones::generation(); licensed_=zones::licensed(); bw_=bw;
        }
    }
    std::string apply(int mhz,const kestrel_ui::BackendState& state) {
        const auto at=state.controls.find("channel");
        if(at==state.controls.end() || !at->second.enabled) throw std::runtime_error("Channel scan is unavailable");
        if(!mhz) radio_.search_channels();
        else {
            const auto& readings=state.channels.readings;
            if(std::none_of(readings.begin(),readings.end(),[&](const auto& c){return c.mhz==mhz;}))
                throw std::runtime_error("Channel is no longer in the radio's scan");
            radio_.set_channel(mhz);
        }
        return mhz?"Channel change requested":"Automatic channel search requested";
    }
private:
    gnd::Radio& radio_; gnd::ScanSample scan_{};
    std::map<int,std::pair<int,uint64_t>> peaks_;
    uint64_t last_scan_=0;
    uint32_t generation_=0;
    bool open_=false,licensed_=false;
    int bw_=0;
};
}
