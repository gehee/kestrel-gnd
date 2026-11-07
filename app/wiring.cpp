#include "app/wiring.hpp"

#include "dvr/dvr_sink.hpp"
#include "dvr/dvr.hpp"
#include "hud/osd.hpp"
#include "motion/imu_reader.hpp"
#include "radio/ar8030_link.hpp"
#include "radio/model.hpp"

void run_radio(radio::Model& model, const Ar8030Config& cfg, Settings& settings,
               const VideoCodec& codec, std::shared_ptr<Vdec> vdec,
               std::shared_ptr<OSD> osd, std::atomic<bool>* stop, int slot, int port) {
    // The radio: the AR8030, reporting to its model, writing its streams.
    auto link = std::make_shared<Ar8030Link>(model, cfg, settings, codec, vdec, stop, "127.0.0.1", 50000,
                                             slot, port);
    // What reads it besides the HUD (which watched its parts when it was
    // made): the air unit's IMU samples, and the recording, a video sink -
    // each on a thread of its own.
    auto imu = std::make_unique<motion::ImuReader>(model.imu());
    DvrSink dvr_sink(codec, DvrRecorder::instance(), [osd] { return osd && osd->is_dvr_screen_enabled(); });
    link->add_video_sink(&dvr_sink);
    link->run();
    // Stopped: its threads joined (the destructor), then its readers here let go.
    link.reset();
    imu.reset();
}
