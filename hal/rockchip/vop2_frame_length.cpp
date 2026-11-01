#include "hal/rockchip/vop2_frame_length.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace vop2 {

bool on_this_soc(const char *compat) {
    return strstr(compat, "rockchip,rk3568") || strstr(compat, "rockchip,rk3566");
}

}  // namespace vop2

namespace hal {

namespace {

class Vop2FrameLength : public FrameLength {
public:
    Vop2FrameLength(void *map, volatile uint32_t *reg) : map_(map), reg_(reg) {}
    ~Vop2FrameLength() override { munmap(map_, 0x1000); }

    bool take_mode(int vtotal, int sync_lines, Seen *seen) override {
        const uint32_t raw = *reg_, v = raw & ~vop2::kImd;
        if (seen) {
            seen->value = v;
            seen->total = vop2::vtotal_of(v);
            seen->held = (raw & vop2::kImd) != 0;
        }
        if (!vop2::is_mode(v, vtotal, sync_lines)) return false;
        orig_ = vop2::mode_value(v, vtotal);
        return true;
    }
    void set_total(int total) override { *reg_ = vop2::total_value(orig_, total); }
    void restore() override { *reg_ = orig_; }

private:
    void *map_;
    volatile uint32_t *reg_;
    uint32_t orig_ = 0;   // the register as the mode has it, immediate bit clear
};

}  // namespace

std::unique_ptr<FrameLength> map_vop2_frame_length(int crtc_index) {
    char compat[512] = {};
    if (FILE *f = fopen("/proc/device-tree/compatible", "r")) {
        const size_t n = fread(compat, 1, sizeof(compat) - 1, f);
        fclose(f);
        for (size_t i = 0; i < n; i++) if (!compat[i]) compat[i] = ' ';
    }
    if (!vop2::on_this_soc(compat)) {
        printf("display lock: not an RK3568 - Refresh Sync is not available\n");
        return nullptr;
    }
    const int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("display lock: /dev/mem");
        return nullptr;
    }
    void *m = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, vop2::kBase);
    close(fd);
    if (m == MAP_FAILED) {
        perror("display lock: mmap");
        return nullptr;
    }
    auto *reg = (volatile uint32_t *)((uint8_t *)m + vop2::kVtotalReg + 0x100 * crtc_index);
    return std::unique_ptr<FrameLength>(new Vop2FrameLength(m, reg));
}

}  // namespace hal
