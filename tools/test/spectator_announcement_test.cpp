#include "radio/spectator_announcement.hpp"
#include <cassert>
#include <vector>
int main() {
    uint8_t rb[33]={5,30};
    std::memcpy(rb+2,"kestrel-spec-v1",15);
    rb[18]=1; rb[19]=1; rb[20]=0;rb[21]=0;rb[22]=1;rb[23]=2;
    rb[24]=128;rb[25]=7;rb[26]=56;rb[27]=4;rb[28]=60;rb[30]=208;rb[31]=7;rb[32]=128;
    std::vector<uint8_t> nal{80,1}; int zeros=0;
    for(auto b:rb) {if(zeros==2 && b<=3){nal.push_back(3);zeros=0;}nal.push_back(b);zeros=b==0?zeros+1:0;}
    radio::spectator::Announcement a;
    assert(radio::spectator::announcement(nal.data(),nal.size(),a));
    assert(a.width==1920 && a.height==1080 && a.fps==60 && a.recovery_ms==2000 && a.mac[2]==1);
    for(size_t n=0;n<nal.size();++n) assert(!radio::spectator::announcement(nal.data(),n,a));
    for(size_t i=2;i<18;++i) {auto bad=nal;bad[i]^=128;assert(!radio::spectator::announcement(bad.data(),bad.size(),a));}
    auto framed=nal;framed.insert(framed.end(),{0xee,0x29,0x55,0x9f});
    assert(radio::spectator::announcement(framed.data(),framed.size(),a));
    auto bad=nal;bad.push_back(0);assert(!radio::spectator::announcement(bad.data(),bad.size(),a));
    bad=nal;bad[0]=78;assert(!radio::spectator::announcement(bad.data(),bad.size(),a));
}
