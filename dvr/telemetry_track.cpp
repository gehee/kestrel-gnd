#include "telemetry_track.hpp"
#include "third_party/nlohmann/json.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace dvr {
namespace {
using Json=nlohmann::json;
constexpr size_t max_line=32768;
Json parse(const std::string& s) {
    return Json::parse(s,[](int depth,Json::parse_event_t,Json&) {
        if(depth>8)throw std::runtime_error("Telemetry nesting too deep");return true;
    });
}
std::string hex(const std::vector<uint8_t>& bytes) {
    std::string s;s.reserve(bytes.size()*2);for(auto b:bytes){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;
}
std::vector<uint8_t> unhex(const std::string& s) {
    if(s.size()>16384 || s.size()%2)throw std::runtime_error("Invalid recorded HUD size");
    auto digit=[](char c)->unsigned {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("Invalid recorded HUD encoding");};
    std::vector<uint8_t> out;out.reserve(s.size()/2);for(size_t i=0;i<s.size();i+=2)out.push_back((digit(s[i])<<4)|digit(s[i+1]));return out;
}
Json encode(const media::TelemetrySample& s) {
    return Json{{"values",s.values},{"text",s.text},{"grid",s.grid},{"hud_v1",hex(s.hud)}};
}
void apply(media::TelemetrySample& s,const Json& j,bool full) {
    if(!j.is_object())throw std::runtime_error("Invalid telemetry state");
    if(full)s={};
    auto values=j.find("values");
    if(values!=j.end()) {
        if(!values->is_object() || values->size()>256)throw std::runtime_error("Invalid telemetry readings");
        for(auto it=values->begin();it!=values->end();++it) {
            if(it.key().size()>80)throw std::runtime_error("Invalid telemetry field");
            if(it->is_null())s.values.erase(it.key());
            else {double n=it->get<double>();if(!std::isfinite(n) || std::abs(n)>1e15)throw std::runtime_error("Invalid telemetry number");s.values[it.key()]=n;}
        }
        if(s.values.size()>256)throw std::runtime_error("Too many telemetry fields");
    }
    auto texts=j.find("text");
    if(texts!=j.end()) {
        if(!texts->is_object() || texts->size()>32)throw std::runtime_error("Invalid telemetry text");
        for(auto it=texts->begin();it!=texts->end();++it) {
            if(it.key().size()>80)throw std::runtime_error("Invalid telemetry text field");
            if(it->is_null())s.text.erase(it.key());
            else {auto t=it->get<std::string>();if(t.size()>256)throw std::runtime_error("Telemetry text too long");s.text[it.key()]=t;}
        }
        if(s.text.size()>32)throw std::runtime_error("Too many telemetry text fields");
    }
    if(j.contains("grid")) {
        const auto& g=j.at("grid");if(!g.is_array() || g.size()!=s.grid.size())throw std::runtime_error("Invalid OSD canvas");
        for(size_t i=0;i<s.grid.size();++i){const int n=g[i].get<int>();if(n<0 || n>65535)throw std::runtime_error("Invalid OSD glyph");s.grid[i]=n;}
    }
    if(j.contains("grid_updates")) {
        const auto& updates=j.at("grid_updates");
        if(!updates.is_array() || updates.size()>s.grid.size())throw std::runtime_error("Invalid OSD updates");
        for(const auto& entry:updates) {
            if(!entry.is_array() || entry.size()!=2)throw std::runtime_error("Invalid OSD update");
            const int i=entry[0].get<int>(),glyph=entry[1].get<int>();
            if(i<0 || size_t(i)>=s.grid.size() || glyph<0 || glyph>65535)throw std::runtime_error("Invalid OSD update bounds");
            s.grid[i]=glyph;
        }
    }
    if(j.contains("hud_v1"))s.hud=unhex(j.at("hud_v1").get<std::string>());
}
bool line(FILE* f,std::string& s) {
    s.clear();int c;
    while((c=fgetc(f))!=EOF) {if(c=='\n')return true;if(s.size()>=max_line)throw std::runtime_error("Telemetry entry too large");s+=char(c);}
    if(ferror(f))throw std::runtime_error("Cannot read telemetry");
    // A power loss may leave the last line incomplete. Earlier complete
    // snapshots remain readable; never apply a partial update.
    return false;
}
void put(FILE* f,const Json& j) {const auto s=j.dump();if(s.size()>max_line || fwrite(s.data(),1,s.size(),f)!=s.size() || fputc('\n',f)==EOF)throw std::runtime_error("Cannot write telemetry");}
}
TelemetryWriter::TelemetryWriter():worker_([this]{run();}) {}
TelemetryWriter::~TelemetryWriter(){finish();{std::lock_guard<std::mutex> l(mutex_);quit_=true;}wake_.notify_one();worker_.join();}
void TelemetryWriter::begin(const std::string& file) {std::lock_guard<std::mutex> l(mutex_);disabled_=false;error_.clear();jobs_.push_back({Job::Begin,file});wake_.notify_one();}
void TelemetryWriter::append(uint64_t us,std::shared_ptr<const media::TelemetrySample> sample) {
    if(!sample)return;std::lock_guard<std::mutex> l(mutex_);if(disabled_)return;
    if(jobs_.size()>=128){disabled_=true;error_="Telemetry storage cannot keep up";jobs_.push_back({Job::End,{}});}
    else jobs_.push_back({Job::Sample,{},us,std::move(sample)});wake_.notify_one();
}
void TelemetryWriter::finish(){std::lock_guard<std::mutex> l(mutex_);jobs_.push_back({Job::End,{}});wake_.notify_one();}
std::string TelemetryWriter::error() const {std::lock_guard<std::mutex> l(mutex_);return error_;}
bool TelemetryWriter::pending() const {std::lock_guard<std::mutex> l(mutex_);return busy_ || !jobs_.empty();}
void TelemetryWriter::drain(){std::unique_lock<std::mutex> l(mutex_);drained_.wait(l,[&]{return jobs_.empty() && !busy_;});}
void TelemetryWriter::run() {
    pthread_setname_np(pthread_self(),"dvr-telem");sched_param p{};pthread_setschedparam(pthread_self(),SCHED_OTHER,&p);
    FILE* f=nullptr;std::string path;uint64_t last_full=0,last_flush=0,last_us=0;bool have=false;Json previous;std::shared_ptr<const media::TelemetrySample> previous_sample;
    auto close=[&]{if(f){bool failed=fflush(f)!=0;if(fsync(fileno(f)))failed=true;if(fclose(f))failed=true;f=nullptr;
        if(!have)unlink(path.c_str());if(failed)throw std::runtime_error("Cannot finish telemetry");}};
    for(;;) {
        Job job;{std::unique_lock<std::mutex> l(mutex_);wake_.wait(l,[&]{return quit_ || !jobs_.empty();});if(jobs_.empty() && quit_)break;
            job=std::move(jobs_.front());jobs_.pop_front();busy_=true;}
        try {
            if(job.kind==Job::Begin){close();{std::lock_guard<std::mutex> l(mutex_);disabled_=false;error_.clear();}path=job.path+".telemetry.jsonl";
                int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0644);
                if(fd<0)throw std::runtime_error("Cannot create telemetry sidecar");
                f=fdopen(fd,"w");if(!f){::close(fd);throw std::runtime_error("Cannot open telemetry writer");}
                setvbuf(f,nullptr,_IOFBF,65536);have=false;last_full=last_flush=last_us=0;previous={};previous_sample.reset();
                put(f,Json{{"format","kestrel.telemetry"},{"version",1},{"timebase","mp4_microseconds"},{"hud_codec",1}});
            } else if(job.kind==Job::End)close();
            else if(f) {
                if(have && job.us<last_us)throw std::runtime_error("Telemetry timestamps moved backwards");
                const bool full=!have || job.us-last_full>=2000000;
                const bool same=previous_sample && previous_sample->values==job.sample->values && previous_sample->text==job.sample->text &&
                    previous_sample->grid==job.sample->grid && previous_sample->hud==job.sample->hud;
                if(full || !same) {
                const auto state=encode(*job.sample);
                Json update=Json::object();
                if(full)update=state;
                else {
                    for(const char* key:{"values","text"}) {
                        Json changed=Json::object();
                        for(auto it=state[key].begin();it!=state[key].end();++it)
                            if(!previous[key].contains(it.key()) || previous[key][it.key()]!=it.value())changed[it.key()]=it.value();
                        for(auto it=previous[key].begin();it!=previous[key].end();++it)if(!state[key].contains(it.key()))changed[it.key()]=nullptr;
                        if(!changed.empty())update[key]=std::move(changed);
                    }
                    if(state["grid"]!=previous["grid"]) {
                        Json cells=Json::array();
                        for(size_t i=0;i<job.sample->grid.size();++i)if(state["grid"][i]!=previous["grid"][i])cells.push_back(Json::array({i,job.sample->grid[i]}));
                        update["grid_updates"]=std::move(cells);
                    }
                    if(state["hud_v1"]!=previous["hud_v1"])update["hud_v1"]=state["hud_v1"];
                }
                if(!update.empty()){put(f,Json{{"t_us",job.us},{"type",full?"snapshot":"update"},{"data",std::move(update)}});previous=state;}
                previous_sample=job.sample;
                }
                have=true;last_us=job.us;if(full)last_full=job.us;
                if(job.us-last_flush>=1000000){if(fflush(f))throw std::runtime_error("Cannot flush telemetry");last_flush=job.us;}
            }
        }catch(const std::exception& e){if(f){fclose(f);f=nullptr;}std::lock_guard<std::mutex> l(mutex_);error_=e.what();disabled_=true;fprintf(stderr,"telemetry: %s\n",e.what());}
        {std::lock_guard<std::mutex> l(mutex_);busy_=false;if(jobs_.empty())drained_.notify_all();}
    }
    try{close();}catch(...){}
}
TelemetryReader::TelemetryReader(const std::string& video) {
    int fd=open((video+".telemetry.jsonl").c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){if(errno!=ENOENT)error_="Cannot open telemetry";return;}
    struct stat s{};if(fstat(fd,&s) || !S_ISREG(s.st_mode) || s.st_size>256*1024*1024){::close(fd);error_="Invalid telemetry file";return;}
    file_=fdopen(fd,"r");if(!file_){::close(fd);error_="Cannot read telemetry";return;}
    try{std::string text;if(!line(file_,text))throw std::runtime_error("Missing telemetry header");const auto j=parse(text);
        if(j.at("format")!="kestrel.telemetry" || j.at("version")!=1 || j.at("timebase")!="mp4_microseconds" || j.at("hud_codec")!=1)
            throw std::runtime_error("Unsupported telemetry format");
        data_offset_=ftello(file_);next();
    }catch(const std::exception& e){error_=e.what();}
}
TelemetryReader::~TelemetryReader(){if(file_)fclose(file_);}
bool TelemetryReader::next() {
    const auto offset=ftello(file_);std::string text;if(!line(file_,text)){eof_=true;next_.reset();return false;}
    const auto j=parse(text);if(!j.at("t_us").is_number_unsigned())throw std::runtime_error("Invalid telemetry timestamp");
    const uint64_t us=j.at("t_us").get<uint64_t>();if(us>86400000000ull || (have_line_ && us<last_line_us_))throw std::runtime_error("Invalid telemetry order");
    const auto type=j.at("type").get<std::string>();const bool full=type=="snapshot";
    if((!have_line_ && !full) || (!full && type!="update"))throw std::runtime_error("Telemetry requires a snapshot");
    next_=std::make_shared<media::TimedTelemetry>();next_->us=us;if(current_)next_->sample=current_->sample;
    apply(next_->sample,j.at("data"),full);last_line_us_=us;have_line_=true;
    if(full && (index_.empty() || index_.back().first<us))index_.push_back({us,offset});return true;
}
std::shared_ptr<const media::TimedTelemetry> TelemetryReader::at(uint64_t us) {
    if(!available())return {};
    try {
        if(current_ && us<current_->us) {
            auto it=std::upper_bound(index_.begin(),index_.end(),us,[](uint64_t t,const auto& p){return t<p.first;});
            const auto offset=it==index_.begin()?data_offset_:std::prev(it)->second;
            if(fseeko(file_,offset,SEEK_SET))throw std::runtime_error("Cannot seek telemetry");
            current_.reset();next_.reset();have_line_=false;eof_=false;next();
        }
        while(next_ && next_->us<=us){current_=next_;next_.reset();if(!eof_)next();}
        // Checkpoints occur at least every two seconds even when nothing changes.
        // A missing tail must not present old readings indefinitely as valid.
        if(current_ && us-current_->us<=2500000)return current_;
        return {};
    }catch(const std::exception& e){error_=e.what();return {};}
}
}
