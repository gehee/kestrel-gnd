#include "base/damage.hpp"
#include <array>
#include <stdexcept>
#include <vector>
int main() {
    constexpr int w=20,h=12;
    std::array<base::DamageRows,3> damage{{{w,h},{w,h},{w,h}}};
    std::array<std::vector<int>,3> buffers;
    std::vector<int> current(w*h,0);
    for (int i=0;i<3;++i) { damage[i].all(); buffers[i].assign(w*h,-1); }
    // Reuse different slots, including a superseded ready buffer. Every slot
    // must catch up on changes made while it was active or not selected.
    const int slots[]={0,1,0,2,1,2,2,0};
    for (int step=0;step<8;++step) {
        base::Damage area{step*2,step,3,2};
        for(int y=area.y;y<area.y+area.height;++y) for(int x=area.x;x<area.x+area.width;++x) current[y*w+x]=step+1;
        for(auto& d:damage) d.add(area);
        int slot=slots[step];
        for(int y=0;y<h;++y) { auto [a,b]=damage[slot].rows()[y]; for(int x=a;x<b;++x) buffers[slot][y*w+x]=current[y*w+x]; }
        damage[slot].clear();
        if(buffers[slot]!=current) throw std::runtime_error("Reused UI buffer missed accumulated damage");
    }
    damage[0].add({-10,-10,12,12});
    if(damage[0].rows()[0]!=std::pair<int,int>{0,2}) throw std::runtime_error("Damage clipping failed");
}
