#include "display/mode_trial.hpp"
#include "display/ascent_kms_mode.hpp"
#include <stdexcept>
#include <cstdio>
static void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {try {
        display::ModeTrial trial(1080);
        trial.select(720); trial.restart=false; trial.started(100);
        check(trial.pending && trial.current==720 && trial.confirmed==1080, "Trial does not replace saved mode");
        check(!trial.tick(15000099), "Mode remains visible throughout confirmation interval");
        check(trial.tick(15000100) && trial.restart && trial.current==1080 && !trial.pending, "Unconfirmed mode reverts");
        trial.restart=false; trial.select(720); trial.started(200); trial.keep();
        check(!trial.tick(99999999) && trial.confirmed==720, "Confirmed mode survives timeout");
        trial.select(1080); trial.revert();
        check(trial.current==720 && !trial.pending, "Failed trial restores most recently confirmed mode");
        bool bad_mode=false;try {trial.select(800);}catch(const std::invalid_argument&) {bad_mode=true;}
        check(bad_mode && trial.current==720, "Invalid mode cannot change state");
        trial.restart=false;
        check(!trial.recover(display::SessionResult::Failed) && trial.current==720 && !trial.restart,
              "A decoder or radio failure must not change a confirmed HDMI mode");
        trial.select(1080);trial.restart=false;
        check(!trial.recover(display::SessionResult::Failed) && trial.current==1080 && trial.pending,
              "A non-display failure is not proof that a trial mode is invalid");
        check(trial.recover(display::SessionResult::DisplayFailed) && trial.current==720 && !trial.pending,
              "A failed display trial restores the confirmed mode");
        trial.restart=false;
        check(trial.recover(display::SessionResult::DisplayFailed) && trial.current==1080,
              "A failed confirmed720 display can recover at1080");
        trial.restart=false;
        check(!trial.recover(display::SessionResult::DisplayFailed) && !trial.restart,
              "Failed1080 display recovery cannot loop");

        for (const auto mode : display::ascent_modes) {
            check(mode.valid() && display::parse_ascent_mode(mode.name()) == mode,
                  "Every supported CLI mode round trips");
            check(display::AscentMode::from_id(mode.id()) == mode,
                  "Menu IDs preserve both resolution and refresh rate");
        }
        for (const auto mode : display::ascent_modes) {
            const auto timing=display::ascent_kms_mode(mode);
            check(timing.clock*1000==timing.htotal*timing.vtotal*mode.hz,
                  "CEA timings produce the requested nominal refresh rate");
            auto advertised=timing;
            advertised.flags|=DRM_MODE_FLAG_PIC_AR_16_9;
            check(display::same_ascent_timing(advertised,timing),
                  "Atomic EDID aspect metadata must not hide supported modes");
            advertised.flags|=DRM_MODE_FLAG_INTERLACE;
            check(!display::same_ascent_timing(advertised,timing),"Interlaced timings must not match");
            advertised=timing; advertised.clock-=100;
            check(!display::same_ascent_timing(advertised,timing),"Fractional/other clocks must not match");
        }
        check(!display::AscentMode{1080,120}.valid() && !display::AscentMode{720,59}.valid(),
              "Unsupported refresh rates are rejected");
        bool bad_text=false;
        try { display::parse_ascent_mode("1280x720@120junk"); }
        catch (const std::invalid_argument&) { bad_text=true; }
        check(bad_text,"CLI mode parser rejects trailing characters");
        display::ModeTrial fast({720,120});
        fast.select({720,100}); fast.started(1000);
        check(fast.pending && fast.current.hz==100 && fast.confirmed.hz==120,
              "Refresh-only changes require confirmation");
        fast.tick(15001000);
        check(fast.current==display::AscentMode{720,120},"Timeout restores the previous refresh rate");
        fast.select({720,50}); fast.keep();
        check(fast.confirmed==display::AscentMode{720,50},"KEEP preserves the selected refresh rate");
        fast.select({1080,50}); fast.recover(display::SessionResult::DisplayFailed);
        check(fast.current==display::AscentMode{720,50},"Failed trial restores both mode dimensions");
        fast.recover(display::SessionResult::DisplayFailed);
        check(fast.current==display::AscentMode{1080,60},"Confirmed-mode recovery uses safe 1080p60");
        display::ModeTrial fifty({1080,50});
        check(fifty.recover(display::SessionResult::DisplayFailed) && fifty.current==display::AscentMode{1080,60},
              "1080p50 failure also falls back to 1080p60");

return 0;}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
