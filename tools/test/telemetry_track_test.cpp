#include "dvr/telemetry_track.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
int main(){try{
    char dir[]="telemetry-test-XXXXXX";check(mkdtemp(dir),"temporary directory");
    struct Cleanup {std::string p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{dir};
    std::string video=std::string(dir)+"/clip.mp4";
    {
        dvr::TelemetryWriter writer;writer.begin(video);
        auto first=std::make_shared<media::TelemetrySample>();first->values={{"battery_v",16.4},{"flight.valid",1},{"motion.dx",0}};
        first->text["mode"]="ACRO";first->grid[100]=512;first->hud={0,255,32};writer.append(0,first);
        auto second=std::make_shared<media::TelemetrySample>(*first);second->values["motion.dx"]=0.1;second->grid[101]=513;writer.append(100000,second);
        auto third=std::make_shared<media::TelemetrySample>(*second);third->values["flight.valid"]=0;third->values.erase("battery_v");writer.append(2000000,third);
        writer.append(2100000,third);writer.finish();writer.drain();check(writer.error().empty(),"writer succeeds");
    }
    {
        dvr::TelemetryReader reader(video);check(reader.available(),"valid track header");
        auto first=reader.at(50000);check(first && first->us==0 && first->sample.values.at("battery_v")==16.4,"first snapshot and hold");
        auto second=reader.at(100000);check(second && second->sample.values.at("motion.dx")==0.1,"timestamped update");
        check(second->sample.grid[100]==512 && second->sample.grid[101]==513 && second->sample.hud==std::vector<uint8_t>({0,255,32}),"HUD and OSD survive delta");
        auto third=reader.at(2100000);check(third && !third->sample.values.count("battery_v") && third->sample.values.at("flight.valid")==0,"invalid readings and removed keys");
        auto rewound=reader.at(0);check(rewound && rewound->sample.values.at("flight.valid")==1,"backward lookup restores snapshot");
        check(!reader.at(6000000),"missing telemetry tail expires");
    }
    {
        std::ofstream out(video+".telemetry.jsonl",std::ios::app);out<<"{\"truncated\":";
    }
    {dvr::TelemetryReader reader(video);check(bool(reader.at(2000000)),"incomplete final line ignored");}
    {dvr::TelemetryReader absent(std::string(dir)+"/old.mp4");check(!absent.available() && absent.error().empty(),"legacy video has no invented telemetry");}
    {
        const auto bad=std::string(dir)+"/bad.mp4";std::ofstream out(bad+".telemetry.jsonl");out<<"{\"format\":\"other\"}\n";out.close();
        dvr::TelemetryReader reader(bad);check(!reader.available() && !reader.error().empty(),"unsupported header rejected");
    }
    {
        dvr::TelemetryWriter writer;writer.begin(std::string(dir)+"/missing/clip.mp4");writer.drain();check(!writer.error().empty(),"sidecar storage error surfaced");
    }
    puts("PASS: telemetry snapshots/deltas, timestamps, rewind, validity, truncated tail and storage errors");return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
