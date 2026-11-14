#pragma once
// Native receivers keep their paired air units outside the vendor factory
// store: one "AA:BB:CC:DD" a line, the one last linked first (a file from
// before holds the one line it always had). The receiver looks for each in
// turn until one links (radio/ar8030_link.cpp).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace radio {
using Peer = std::array<uint8_t, 4>;
using Peers = std::vector<Peer>;
inline constexpr size_t kMaxPeers = 8;

inline bool valid_peer(const Peer& p) {
    return p != Peer{} && p != Peer{255,255,255,255};
}
inline std::optional<Peer> parse_peer(const char* text) {
    unsigned a,b,c,d; char extra;
    if (sscanf(text, "%2x:%2x:%2x:%2x %c", &a,&b,&c,&d,&extra) != 4) return {};
    Peer p{uint8_t(a),uint8_t(b),uint8_t(c),uint8_t(d)};
    return valid_peer(p) ? std::optional<Peer>(p) : std::nullopt;
}
// p first, then the others in their order, at most kMaxPeers.
inline Peers with_first(const Peers& list, const Peer& p) {
    Peers out{p};
    for (const auto& q : list)
        if (q != p && out.size() < kMaxPeers) out.push_back(q);
    return out;
}
inline Peers load_native_peers(const std::string& path) {
    Peers out;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;
    char line[64];
    while (fgets(line, sizeof(line), f) && out.size() < kMaxPeers) {
        const auto p = parse_peer(line);
        if (p && std::find(out.begin(), out.end(), *p) == out.end()) out.push_back(*p);
    }
    fclose(f);
    return out;
}
inline bool save_native_peers(const std::string& path, const Peers& peers) {
    if (path.empty() || peers.empty()) return false;
    for (const auto& p : peers) if (!valid_peer(p)) return false;
    const auto tmp = path + ".new";
    const int fd = open(tmp.c_str(), O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    std::string text;
    for (size_t i = 0; i < peers.size() && i < kMaxPeers; i++) {
        char line[16];
        snprintf(line,sizeof(line),"%02X:%02X:%02X:%02X\n",peers[i][0],peers[i][1],peers[i][2],peers[i][3]);
        text += line;
    }
    bool ok = write(fd,text.data(),text.size())==ssize_t(text.size()) && fsync(fd)==0;
    if (close(fd)) ok=false;
    if (!ok || rename(tmp.c_str(),path.c_str())) { unlink(tmp.c_str()); return false; }
    const auto slash=path.find_last_of('/');
    const auto parent=slash==std::string::npos?".":path.substr(0,slash?slash:1);
    const int dir=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(dir<0)return false;
    ok=fsync(dir)==0; close(dir); return ok;
}
// The one last linked.
inline std::optional<Peer> load_native_peer(const std::string& path) {
    const auto list = load_native_peers(path);
    return list.empty() ? std::nullopt : std::optional<Peer>(list.front());
}
// Adds (or moves) peer to the front; the others stay.
inline bool save_native_peer(const std::string& path, const Peer& peer) {
    if (!valid_peer(peer)) return false;
    return save_native_peers(path, with_first(load_native_peers(path), peer));
}
// The air units a receiver.conf has settings for (sky_AABBCCDD_*): units it
// was paired with and linked to before it kept more than one.
inline Peers peers_in_settings(const std::string& all_text) {
    Peers out;
    size_t at = 0;
    while ((at = all_text.find("sky_", at)) != std::string::npos) {
        const bool line_start = at == 0 || all_text[at - 1] == '\n';
        if (line_start && at + 13 <= all_text.size() && all_text[at + 12] == '_') {
            const std::string hex = all_text.substr(at + 4, 8);
            const std::string mac = hex.substr(0,2)+":"+hex.substr(2,2)+":"+hex.substr(4,2)+":"+hex.substr(6,2);
            const auto p = parse_peer(mac.c_str());
            if (p && std::find(out.begin(), out.end(), *p) == out.end()) out.push_back(*p);
        }
        at += 4;
    }
    return out;
}
}
