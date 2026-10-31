
#ifndef FPVUE_SCHEDULINGHELPER_H
#define FPVUE_SCHEDULINGHELPER_H

#include <pthread.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <mutex>

namespace SchedulingHelper {

// Thread Roles for automatic scheduling
enum class ThreadRole {
    Main,
    LinkRx,
    Pipeline,
    VDec,
    Renderer,
    OSD,
    DrmEvent
};

void init();
void configure_thread(ThreadRole role);

// Legacy helpers 
static void set_thread_params_max_realtime(const std::string& tag, const int priority = 90) {
    pthread_t target = pthread_self();
    pthread_setname_np(target, tag.c_str());
    int policy = SCHED_FIFO;
    struct sched_param param{};
    param.sched_priority = priority;
    auto result = pthread_setschedparam(target, policy, &param);
    if (result != 0) {
        std::cerr << "Cannot setThreadParamsMaxRealtime " << result << std::endl;
    } else {
        std::cout << "Changed prio for " << tag << " to SCHED_FIFO:" << param.sched_priority << std::endl;
    }
}

static int get_core_count() {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

static void pin_thread_to_core(int core_id) {
    int actual_cores = get_core_count();
    int target_core = core_id % actual_cores;

    pthread_t thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(target_core, &cpuset);

    int result = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        fprintf(stderr, "Error setting thread affinity to core %d (requested %d): %d\n", target_core, core_id, result);
    }
}

// Fixed core IDs (Deprecated mapping)
static constexpr int CORE_MAIN=0;
static constexpr int CORE_BIG_2=2;
static constexpr int CORE_BIG_3=3; // Used in renderer currently if checking legacy
static constexpr int CORE_VDEC_FRAME_READER=2; // Used in vdec

}  // namespace SchedulingHelper

#endif //FPVUE_SCHEDULINGHELPER_H
