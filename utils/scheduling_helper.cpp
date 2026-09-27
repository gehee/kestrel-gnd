
#include "scheduling_helper.hpp"
#include <vector>
#include <algorithm>
#include <string>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#include <mutex>

namespace SchedulingHelper {

enum class CpuArch {
    Unknown,
    X86_64,
    ARM_BigLittle,
    ARM_SMP,
};

struct CoreInfo {
    int id;
    int max_freq_khz;
};

static CpuArch g_arch = CpuArch::Unknown;
static std::vector<CoreInfo> g_cores;
static std::vector<int> g_little_cores;
static std::vector<int> g_big_cores;
static std::mutex g_init_mutex;
static bool g_initialized = false;

static int read_max_freq(int cpu_id) {
    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id) + "/cpufreq/cpuinfo_max_freq";
    std::ifstream f(path);
    if (f.is_open()) {
        int freq;
        f >> freq;
        return freq;
    }
    // Fallback: scaling_max_freq
    path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id) + "/cpufreq/scaling_max_freq";
    std::ifstream f2(path);
    if (f2.is_open()) {
        int freq;
        f2 >> freq;
        return freq;
    }
    return 0;
}

void init() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized) return;

    // Detect Architecture
    #if defined(__x86_64__) || defined(__i386__)
        g_arch = CpuArch::X86_64;
        printf("SCHED: Architecture: X86\n");
    #elif defined(__aarch64__) || defined(__arm__)
        g_arch = CpuArch::ARM_SMP; // Tentative
        printf("SCHED: Architecture: ARM\n");
    #else
        printf("SCHED: Architecture: Unknown\n");
    #endif

    int count = get_core_count();
    printf("SCHED: Found %d cores\n", count);

    std::map<int, std::vector<int>> freq_groups;

    for(int i=0; i<count; i++) {
        int freq = read_max_freq(i);
        g_cores.push_back({i, freq});
        if (freq > 0) {
            freq_groups[freq].push_back(i);
        }
    }

    if (g_arch != CpuArch::X86_64 && !freq_groups.empty()) {
        if (freq_groups.size() > 1) {
            g_arch = CpuArch::ARM_BigLittle;
            printf("SCHED: Detected big.LITTLE topology\n");
            
            // Assume lowest freq is LITTLE, highest is BIG
            // Some SoCs have Prime cores (Middle), but we'll group them into Big for now or split 
            
            auto it = freq_groups.begin();
            g_little_cores = it->second; // Lowest freq
            
             // Everything else is "Big" (or Mid+Big)
            for (auto it2 = std::next(it); it2 != freq_groups.end(); ++it2) {
                g_big_cores.insert(g_big_cores.end(), it2->second.begin(), it2->second.end());
            }
        } else {
             printf("SCHED: Detected SMP topology (Uniform Frequencies)\n");
             g_arch = CpuArch::ARM_SMP;
             // All cores are "Big" enough effectively, or we treat them as a single pool
             for(int i=0; i<count; i++) g_big_cores.push_back(i);
             // g_little_cores stays empty, or we could split them for load balancing
        }
    } else {
        // X86 or detection failed: Treat all as equal
        for(int i=0; i<count; i++) g_big_cores.push_back(i);
    }
    
    printf("SCHED: Little Cores: %zu, Big Cores: %zu\n", g_little_cores.size(), g_big_cores.size());
    for(int c : g_little_cores) printf("  L: %d\n", c);
    for(int c : g_big_cores) printf("  B: %d\n", c);

    g_initialized = true;
}

int get_core_for_role(ThreadRole role) {
    if (!g_initialized) init();

    if (g_cores.empty()) return -1;

    // Strategies based on Core Count and Arch
    size_t n_big = g_big_cores.size();
    size_t n_little = g_little_cores.size();
    size_t n_total = g_cores.size();

    auto get_big = [&](int idx) { return g_big_cores[idx % n_big]; };
    auto get_little = [&](int idx) { return n_little > 0 ? g_little_cores[idx % n_little] : g_big_cores[idx % n_big]; }; // Fallback to big if no little
    auto get_any = [&](int idx) { return idx % n_total; };
    
    // x86 strategy: Spread out
    if (g_arch == CpuArch::X86_64) {
        // Avoid strict pinning on x86 desktop to prevent interference with system/driver interrupts
        switch(role) {
            case ThreadRole::LinkRx:    return 1; // Pin Network Thread
            default: return -1; // Let OS schedule the rest
        }
    }

    // ARM Strategies
    if (n_total >= 8 && n_little >= 4 && n_big >= 4) {
        // RK3588 style (4L + 4B)
        switch(role) {
            case ThreadRole::Main:      return get_little(0);
            case ThreadRole::DrmEvent:  return get_little(1);
            case ThreadRole::OSD:       return get_little(2);
            case ThreadRole::LinkRx:    return get_big(0); // Needs fast reaction
            case ThreadRole::Pipeline:  return get_big(1); // Heavy
            case ThreadRole::VDec:      return get_big(2); // Heavy
            case ThreadRole::Renderer:  return get_big(3); // Heavy
            default: return -1;
        }
    } else if (n_total == 6 && n_big >= 2) {
         // RK3399 (4L + 2B) - Tight constraint
         switch(role) {
            case ThreadRole::Main:      return get_little(0);
            case ThreadRole::OSD:       return get_little(1);
            case ThreadRole::DrmEvent:  return get_little(2);
            case ThreadRole::LinkRx:    return get_little(3); // Offload to little
            case ThreadRole::Pipeline:  return get_big(0);
            case ThreadRole::VDec:      return get_big(1); // Share big
            case ThreadRole::Renderer:  return get_big(1); // Share big
            default: return -1;
        }       
    } else if (n_total == 4) {
        // Pi 4 / RK3566 (4 x Big/Mid)
        // Need to share resources carefully
        switch(role) {
            case ThreadRole::Main:      return 0;
            case ThreadRole::LinkRx:    return 1;
            case ThreadRole::Pipeline:  return 2;
            case ThreadRole::VDec:      return 3;
            case ThreadRole::Renderer:  return 3; // Share with Vdec (often Vdec is HW, thread is just waiting)
            case ThreadRole::OSD:       return 0; // Share with Main
            case ThreadRole::DrmEvent:  return 0; // Share with Main
            default: return -1;
        }
    }

    // Fallback
    return -1;
}

const char* get_role_name(ThreadRole role) {
    switch(role) {
        case ThreadRole::Main: return "Main";
        case ThreadRole::LinkRx: return "LinkRx";
        case ThreadRole::Pipeline: return "Pipeline";
        case ThreadRole::VDec: return "VDec";
        case ThreadRole::Renderer: return "Renderer";
        case ThreadRole::OSD: return "OSD";
        case ThreadRole::DrmEvent: return "DrmEvent";
        default: return "Unknown";
    }
}

int get_role_prio(ThreadRole role) {
     switch(role) {
        case ThreadRole::LinkRx:    return 60; // Still high but leaves room for top halves
        case ThreadRole::DrmEvent:  return 60; // Needs to catch vsync fast
        case ThreadRole::VDec:      return 55; // Decoder Frame feed 
        case ThreadRole::Pipeline:  return 50; // Packet processing
        case ThreadRole::Renderer:  return 50; // Render loop
        case ThreadRole::OSD:       return 30; // OSD (Can drop frames)
        case ThreadRole::Main:      return 10;
        default: return 10;
    }
}

void configure_thread(ThreadRole role) {
    if (!g_initialized) init();

    int core = get_core_for_role(role);
    int prio = get_role_prio(role);
    
    set_thread_params_max_realtime(get_role_name(role), prio);
    if (core >= 0) {
        pin_thread_to_core(core);
    }
}

}
