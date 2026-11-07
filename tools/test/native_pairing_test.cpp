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
    for(const auto* bad:{"FF:FF:FF:FF", "00:00:00:00", "01:02:03", "01:02:03:04junk"}) {
        FILE* f=fopen(path.c_str(),"w"); check(f); fputs(bad,f); fclose(f);
        check(!radio::load_native_peer(path));
    }
    const std::string conf=std::string(dir)+"/receiver.conf";
    const Setting<int> mode{"screen_height",1080};
    {
        Settings settings; settings.load(conf,true); settings.set(mode,720); settings.flush();
        Settings restored; restored.load(conf,true); check(restored.get(mode)==720);
        // A failed replacement must leave the last complete configuration intact.
        std::filesystem::create_directory(conf+".new");
        settings.set(mode,1080); settings.flush();
        Settings intact; intact.load(conf,true); check(intact.get(mode)==720);
        std::filesystem::remove(conf+".new");
    }
    std::filesystem::remove_all(dir);
}
