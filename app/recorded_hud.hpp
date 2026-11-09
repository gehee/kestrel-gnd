#pragma once
#include "hud/lvgl/controller.hpp"
#include "hud/lvgl/hud_protocol.hpp"
#include "system/recorded_telemetry.hpp"
#include "motion/imu_stream.hpp"
namespace app {
inline std::shared_ptr<const media::TelemetrySample> record_hud(const kestrel_ui::Controller& controller,
        const kestrel_ui::Model& m,uint64_t now) {
    auto s=std::make_shared<media::TelemetrySample>();auto& v=s->values;auto& text=s->text;
    const auto& b=m.backend;const auto& f=b.flight;
#define FLIGHT(field) v["flight." #field]=f.field
    FLIGHT(osd_src);FLIGHT(have_cell_v);FLIGHT(cell_v);FLIGHT(have_pack_v);FLIGHT(pack_v);
    FLIGHT(have_amps);FLIGHT(amps);FLIGHT(have_alt);FLIGHT(alt_m);FLIGHT(have_speed);FLIGHT(speed_kmh);
    FLIGHT(have_rssi);FLIGHT(rssi_pct);FLIGHT(have_dbm);FLIGHT(rssi_dbm);FLIGHT(have_lq);FLIGHT(lq_pct);
    FLIGHT(have_timer);FLIGHT(timer_s);FLIGHT(have_mode);FLIGHT(have_pitch);FLIGHT(pitch_deg);FLIGHT(have_roll);FLIGHT(roll_deg);
    FLIGHT(have_batt_pct);FLIGHT(batt_pct);FLIGHT(have_gps);FLIGHT(gps_fix);FLIGHT(sats);FLIGHT(have_pos);FLIGHT(lat);FLIGHT(lon);
    FLIGHT(have_home);FLIGHT(home_dist_m);FLIGHT(home_dir_deg);FLIGHT(arm);
#undef FLIGHT
    text["flight.mode"]=std::string(f.mode,std::find(f.mode,f.mode+sizeof(f.mode),'\0'));
    v["flight.fresh"]=f.stamp_us && now>=f.stamp_us && now-f.stamp_us<2000000;
#define VRX(field) v["vrx." #field]=b.vrx.field
    VRX(connected);VRX(mcs);VRX(frequency_mhz);VRX(quality_raw);VRX(frequency_raw);VRX(distance_raw);
    VRX(voltage_mv);VRX(cpu_temperature_x100);VRX(rf_temperature_raw);
#undef VRX
    v["vrx.fresh"]=b.vrx.fresh(now);v["vrx.link_valid"]=b.vrx.link_valid(now);
#define LINK(field) v["link." #field]=b.link.field
    LINK(distance_m);LINK(bandwidth_step);LINK(air_power_mw);LINK(air_live_dbm);LINK(video_mbps);LINK(link_mbps);
    LINK(standby);LINK(vtx_temperature_valid);LINK(vtx_temperature_c);
#undef LINK
    v["video.width"]=b.video.width;v["video.height"]=b.video.height;v["video.fps"]=b.video.fps;v["video.latency_ms"]=b.video.latency_ms;
    v["video.fresh"]=b.media.video_fresh(now);v["osd.fresh"]=b.serial.fresh(now);
    v["motion.dx"]=b.hud_dx;v["motion.dy"]=b.hud_dy;v["motion.bank_rad"]=b.hud_bank;v["motion.reactivity"]=m.dynamic;
    v["link.warning"]=controller.link_warning();s->grid=b.bf_grid;
    text["fc.variant"]=b.serial.variant;text["fc.firmware"]=b.serial.firmware;
    text["vtx.identity"]=b.vtx.identity;text["vtx.firmware"]=b.vtx.firmware;
    text["clock"]=m.clock_text(std::time(nullptr),now);
    auto& imu=motion::ImuStream::get();double rate[3]{};const bool rates=imu.recent_rate_dps(20000,rate);
    v["imu.rate_valid"]=rates;
    if(rates){v["imu.x_dps"]=rate[0];v["imu.y_dps"]=rate[1];v["imu.z_dps"]=rate[2];}
    const auto status=imu.status();motion::Quat q;
    const bool orientation=status.live && imu.orientation_at(status.newest_us,&q);v["imu.orientation_valid"]=orientation;
    if(orientation){v["imu.qw"]=q.w;v["imu.qx"]=q.x;v["imu.qy"]=q.y;v["imu.qz"]=q.z;}
    s->hud=kestrel_ui::hud_ipc::encode(controller.hud_data(now,true));return s;
}
inline double reading(const media::TelemetrySample& s,const char* key,double fallback=0) {
    const auto i=s.values.find(key);return i==s.values.end()?fallback:i->second;
}
}
