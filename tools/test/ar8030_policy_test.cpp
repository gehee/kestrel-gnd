#include "radio/ar8030_policy.hpp"
#include <cstdio>
#include <unistd.h>

int main() {
    char path[] = "/tmp/kestrel-policy-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    {
        Settings saved;
        saved.load(path);
        saved.set(radio::setting::ar8030_max_bw_mhz, 20);
        saved.set(radio::setting::ar8030_max_mcs, 8);
        saved.set(radio::setting::ar8030_steady, 1);
        saved.set(radio::setting::ar8030_chan_mhz, 5839);
        saved.flush();
    }
    Settings reloaded;
    reloaded.load(path);
    unlink(path);
    Ar8030Config cfg;
    ar8030_load_policy(cfg, reloaded);
    if (cfg.max_bw_mhz != 20 || cfg.max_mcs != 8 || cfg.steady != 1 ||
        cfg.chan_fixed_mhz != 5839 || cfg.chan_hop != 0) return 2;
    Settings defaults;
    ar8030_load_policy(cfg, defaults, 20);
    if (cfg.max_bw_mhz != 20) return 5;
    ar8030_load_policy(cfg, defaults);
    if (cfg.max_bw_mhz != 40) return 6;
    Settings automatic;
    automatic.set(radio::setting::ar8030_max_bw_mhz, 0);
    ar8030_load_policy(cfg, automatic);
    if (cfg.max_bw_mhz != 0 || cfg.max_mcs != -1 || cfg.steady != 0 ||
        cfg.chan_fixed_mhz != 0) return 3;
    automatic.set(radio::setting::ar8030_max_bw_mhz, -10);
    automatic.set(radio::setting::ar8030_max_mcs, 99);
    automatic.set(radio::setting::ar8030_chan_mhz, 100);
    ar8030_load_policy(cfg, automatic);
    if (cfg.max_bw_mhz != 40 || cfg.max_mcs != -1 || cfg.chan_fixed_mhz != 0) return 4;
    std::puts("Persisted radio policy restored and validated");
}
