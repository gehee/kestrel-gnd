#pragma once
// Native receivers keep their single paired AP outside the vendor factory store.
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <fcntl.h>
#include <unistd.h>

namespace radio {
using Peer = std::array<uint8_t, 4>;
inline bool valid_peer(const Peer& p) {
    return p != Peer{} && p != Peer{255,255,255,255};
}
inline std::optional<Peer> load_native_peer(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return {};
    unsigned a,b,c,d; char extra;
    const int n = fscanf(f, "%2x:%2x:%2x:%2x %c", &a,&b,&c,&d,&extra);
    fclose(f);
    if (n != 4) return {};
    Peer p{uint8_t(a),uint8_t(b),uint8_t(c),uint8_t(d)};
    return valid_peer(p) ? std::optional<Peer>(p) : std::nullopt;
}
inline bool save_native_peer(const std::string& path, const Peer& peer) {
    if (path.empty() || !valid_peer(peer)) return false;
    const auto tmp = path + ".new";
    const int fd = open(tmp.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    char line[16];
    const int len = snprintf(line,sizeof(line),"%02X:%02X:%02X:%02X\n",peer[0],peer[1],peer[2],peer[3]);
    bool ok = write(fd,line,len)==len && fsync(fd)==0;
    if (close(fd)) ok=false;
    if (!ok || rename(tmp.c_str(),path.c_str())) { unlink(tmp.c_str()); return false; }
    const auto slash=path.find_last_of('/');
    const auto parent=slash==std::string::npos?".":path.substr(0,slash?slash:1);
    const int dir=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(dir<0)return false;
    ok=fsync(dir)==0; close(dir); return ok;
}
}
