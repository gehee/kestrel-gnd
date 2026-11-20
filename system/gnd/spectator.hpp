#pragma once
namespace gnd {
// Manual receive-only tuning. The short stream ID is a filter, not authentication.
struct Spectator {
    int mode = 0; // 0 pilot, 1 spectator
    int frequency_mhz = 5839;
    int bandwidth_mhz = 40;
    int stream_id = 0;
};
inline bool operator==(const Spectator& a, const Spectator& b) {
    return a.mode==b.mode && a.frequency_mhz==b.frequency_mhz &&
        a.bandwidth_mhz==b.bandwidth_mhz && a.stream_id==b.stream_id;
}
inline bool operator!=(const Spectator& a, const Spectator& b) { return !(a==b); }
inline bool valid(const Spectator& s) {
    return (s.mode==0 || s.mode==1) && s.frequency_mhz>=4915 && s.frequency_mhz<=6400 &&
        (s.bandwidth_mhz==5 || s.bandwidth_mhz==10 || s.bandwidth_mhz==20 || s.bandwidth_mhz==40) && s.stream_id>=0 && s.stream_id<=255;
}
} // namespace gnd
