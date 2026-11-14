#include "radio/native_pairing.hpp"
#include "base/settings.hpp"
#include <filesystem>
#include <stdexcept>
int main() {
    auto check=[](bool ok){if(!ok)throw std::runtime_error("native pairing persistence failed");};
    char dir[]="/tmp/kestrel-pairing-XXXXXX";
    check(mkdtemp(dir)); const std::string path=std::string(dir)+"/peer";
    check(!radio::load_native_peer(path));
    const radio::Peer first{0xe5,0x12,0xa8,0xea},second{0x01,0x02,0x03,0x04};
    check(radio::save_native_peer(path,first)); check(radio::load_native_peer(path)==first);
    check(radio::save_native_peer(path,second)); check(radio::load_native_peer(path)==second);
    check(!radio::save_native_peer(path,{})); check(radio::load_native_peer(path)==second);
    check(!radio::save_native_peer(std::string(dir)+"/missing/peer",first));
    // Every unit paired is kept, the last linked first; the old one-line file
    // is a list of one.
    check((radio::load_native_peers(path)==radio::Peers{second,first}));
    check(radio::save_native_peer(path,first));
    check((radio::load_native_peers(path)==radio::Peers{first,second}));
    for (uint8_t i=1;i<=10;i++) check(radio::save_native_peer(path,radio::Peer{0x10,0,0,i}));
    check(radio::load_native_peers(path).size()==radio::kMaxPeers);
    check(radio::load_native_peer(path)==(radio::Peer{0x10,0,0,10}));
    { FILE* f=fopen(path.c_str(),"w"); check(f); fputs("E5:D1:C7:8E\nnot a mac\nE6:A4:73:9A\nE5:D1:C7:8E\n",f); fclose(f); }
    check((radio::load_native_peers(path)==radio::Peers{{0xE5,0xD1,0xC7,0x8E},{0xE6,0xA4,0x73,0x9A}}));
    // Units a receiver.conf has settings for, once each, in file order.
    check((radio::peers_in_settings("ar8030_last_peer: 1\nsky_E5D1C78E_air: 491\nsky_E5D1C78E_ev: 0\n"
                                    "sky_E6A4739A_air: 491\nxsky_01020304_a: 1\nsky_FFFFFFFF_air: 1\n")
           ==radio::Peers{{0xE5,0xD1,0xC7,0x8E},{0xE6,0xA4,0x73,0x9A}}));
    check(radio::save_native_peers(path,{second}));
    for(const auto* bad:{"FF:FF:FF:FF", "00:00:00:00", "01:02:03", "01:02:03:04junk"}) {
        FILE* f=fopen(path.c_str(),"w"); check(f); fputs(bad,f); fclose(f);
        check(!radio::load_native_peer(path));
    }
    const std::string conf=std::string(dir)+"/receiver.conf";
    const Setting<int> mode{"screen_height",1080};
    {
        Settings settings; settings.load(conf); settings.set(mode,720); settings.flush();
        Settings restored; restored.load(conf); check(restored.get(mode)==720);
        // A failed replacement must leave the last complete configuration intact.
        std::filesystem::create_directory(conf+".tmp");
        settings.set(mode,1080); settings.flush();
        Settings intact; intact.load(conf); check(intact.get(mode)==720);
        std::filesystem::remove(conf+".tmp");
    }
    std::filesystem::remove_all(dir);
}
