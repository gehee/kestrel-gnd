#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace radio::spectator {
struct Announcement {
    std::array<uint8_t,4> mac{};
    unsigned width=0, height=0, fps=0, recovery_ms=0;
};
// Dedicated, bounded suffix-SEI message. Unknown versions/flags are ignored.
inline bool announcement(const uint8_t* nal, size_t length, Announcement& result) {
    // The transport splitter includes the stock packet trailer in its last NAL.
    const uint8_t trailer[4]={0xee,0x29,0x55,0x9f};
    if (nal && length>=4 && !std::memcmp(nal+length-4,trailer,4)) length-=4;
    if (!nal || length < 35 || length > 52 || nal[0] != 80 || nal[1] != 1) return false;
    uint8_t rb[33]; size_t n=0; int zeros=0;
    for (size_t i=2; i<length; ++i) {
        const uint8_t b=nal[i];
        if (zeros==2 && b==3) {
            if (i+1==length || nal[i+1]>3) return false;
            zeros=0; continue;
        }
        if (zeros==2 && b<=2) return false;
        if (n==sizeof(rb)) return false;
        rb[n++]=b; zeros=b==0?zeros+1:0;
    }
    const uint8_t uuid[16]="kestrel-spec-v1";
    if (n!=33 || rb[0]!=5 || rb[1]!=30 || std::memcmp(rb+2,uuid,16) ||
        rb[18]!=1 || rb[19]!=1 || rb[32]!=128) return false;
    auto u16=[&](int p){return unsigned(rb[p]) | unsigned(rb[p+1])<<8;};
    Announcement a;
    std::memcpy(a.mac.data(),rb+20,4);
    a.width=u16(24); a.height=u16(26); a.fps=u16(28); a.recovery_ms=u16(30);
    if (!a.width || !a.height || !a.fps || !a.recovery_ms) return false;
    result=a; return true;
}
}
