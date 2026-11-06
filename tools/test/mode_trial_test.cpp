#include "display/mode_trial.hpp"
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

return 0;}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
