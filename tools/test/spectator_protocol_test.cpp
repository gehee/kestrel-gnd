#include "radio/spectator_protocol.hpp"
#include <cassert>
#include <vector>
static uint32_t code;
static int calls;
static std::vector<uint8_t> payload;
static bool supported=true,held=true,tx=false;
static int request(uint32_t id,const void* in,uint32_t n,void* out,uint32_t,uint32_t* got,int timeout) {
    ++calls;code=id;payload.clear();
    if(n) payload.assign((const uint8_t*)in,(const uint8_t*)in+n);
    assert(timeout>0);
    if(id==0x0c5a0037) {
        if(!supported) return -95;
        uint8_t caps[28]={'L','S','T','1',2,0,0,0,32,0,0,0};
        caps[16]=tx; std::memcpy(out,caps,28);*got=28;
    } else if(id==0x0c5a0038) {
        uint32_t state[8]={1,5839000,4,3,2,0,0,40};std::memcpy(out,state,sizeof state);*got=sizeof state;
    } else if(id==0x0c5a0018) {std::memset(out,0,4);((uint8_t*)out)[0]=held?4:0;*got=4;}
    return 0;
}
int main() {
    assert(radio::spectator::available(request));
    supported=false;assert(!radio::spectator::available(request));
    assert(!radio::spectator::available(nullptr));
    gnd::Spectator config{1,5839,40,246};
    assert(!radio::spectator::start(request,config));
    assert(code==0x0c5a0016 && payload.size()==28);
    uint32_t frequency=payload[0]|payload[1]<<8|payload[2]<<16|payload[3]<<24;
    assert(frequency==5839000 && payload[8]==246 && payload[12]==78);
    assert(!std::memcmp(payload.data()+16,"LST2",4));
    assert(payload[20]==5 && payload[21]==4 && payload[22]==5 && payload[23]==1);
    assert(payload[24]==0xc4 && payload[25]==9 && payload[4]==0);
    for(auto bad:{gnd::Spectator{1,100,40,1},gnd::Spectator{1,5839,30,1},gnd::Spectator{1,5839,40,256},gnd::Spectator{0,5839,40,1}}) {
        const int before=calls;
        assert(radio::spectator::start(request,bad)==-22 && calls==before);
    }
    supported=true;
    assert(radio::spectator::tx_held(request));held=false;assert(!radio::spectator::tx_held(request));
    held=true;tx=true;assert(!radio::spectator::tx_held(request));
    radio::spectator::FollowState state;
    assert(radio::spectator::state(request,state) && state.khz==5839000 && state.bandwidth==4 && state.hops==3 && state.widths==2);
    for(int bw:{5,10,20,40}) {
        config.bandwidth_mhz=bw;assert(!radio::spectator::start(request,config));
        assert(payload[20]==(bw==40?5:bw==20?4:bw==10?3:2));
        assert(payload[24]==(bw==40?0xc4:0x50));
    }
    assert(!radio::spectator::stop(request) && code==0x0b000001 && payload.empty());
}
