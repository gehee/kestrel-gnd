#pragma once
// Open8030 LST1 protocol over ar_libre's existing bounded request API.
// No normal radio initialization is issued by these receive-only operations.
#include <cstdint>
#include <cstring>
#include "system/gnd/spectator.hpp"
namespace radio::spectator {
using Request = int (*)(uint32_t,const void*,uint32_t,void*,uint32_t,uint32_t*,int);
inline bool available(Request request) {
    if (!request) return false;
    uint8_t reply[28]{}; uint32_t got=0;
    return request(0x0c5a0037,nullptr,0,reply,sizeof reply,&got,2000)==0 && got==sizeof reply &&
        !std::memcmp(reply,"LST1",4) && reply[4]==2 && (reply[8]&0x20);
}
inline int start(Request request,const gnd::Spectator& config) {
    if (!request || !gnd::valid(config) || config.mode!=1) return -22;
    uint8_t args[28]{};
    const uint32_t khz=uint32_t(config.frequency_mhz)*1000;
    for(unsigned i=0;i<4;++i) args[i]=khz>>(8*i);
    args[8]=uint8_t(config.stream_id); args[12]=78;
    std::memcpy(args+16,"LST2",4);
    args[20]=config.bandwidth_mhz==40?5:config.bandwidth_mhz==20?4:config.bandwidth_mhz==10?3:2; args[21]=4; args[22]=5; args[23]=1;
    args[24]=config.bandwidth_mhz==40?0xc4:0x50; args[25]=config.bandwidth_mhz==40?0x09:0x12;
    uint8_t reply[4096]; uint32_t got=0;
    return request(0x0c5a0016,args,sizeof args,reply,sizeof reply,&got,20000);
}
struct FollowState { uint32_t active=0, khz=0, bandwidth=0, hops=0, widths=0, rejected=0; };
inline bool state(Request request, FollowState& s) {
    uint8_t data[32]{}; uint32_t got=0;
    if (!request || request(0x0c5a0038,nullptr,0,data,sizeof data,&got,2000) || got!=sizeof data) return false;
    auto u32=[&](unsigned p) {return uint32_t(data[p])|uint32_t(data[p+1])<<8|uint32_t(data[p+2])<<16|uint32_t(data[p+3])<<24;};
    s={u32(0),u32(4),u32(8),u32(12),u32(16),u32(20)};
    return s.active==1 && s.khz>=4915000 && s.khz<=6400000 && s.bandwidth>=2 && s.bandwidth<=5;
}
inline int stop(Request request) {
    return request ? request(0x0b000001,nullptr,0,nullptr,0,nullptr,5000) : -95;
}
inline bool hold_bit(Request request) {
    if (!request) return false;
    const uint8_t query[4]={0,8,4,0}; // BB offset 0x800, four bytes
    uint8_t reply[4]{}; uint32_t got=0;
    if (request(0x0c5a0018,query,sizeof query,reply,sizeof reply,&got,2000)!=0 ||
        got!=4 || !(reply[0]&4)) return false;
    return true;
}
inline bool tx_held(Request request) {
    if (!hold_bit(request)) return false;
    uint32_t got=0;
    uint8_t counters[28]{};
    if (request(0x0c5a0037,nullptr,0,counters,sizeof counters,&got,2000)!=0 || got!=sizeof counters) return false;
    for(unsigned i=12;i<sizeof counters;++i) if(counters[i]) return false;
    return true;
}
}
