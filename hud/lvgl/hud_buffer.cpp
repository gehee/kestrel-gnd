#include "hud_buffer.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace kestrel_ui {
HudBuffer::HudBuffer(const hud_ipc::Layout& layout, int supplied_fd) : layout_(layout), fd_(supplied_fd) {
    try {
        layout_.validate(); size_ = layout_.bytes() * 2;
        if (fd_ < 0) {
            fd_ = int(syscall(SYS_memfd_create, "kestrel-hud", MFD_CLOEXEC | MFD_ALLOW_SEALING));
            const bool sealable = fd_ >= 0;
            if (fd_ < 0) {
                const char* temp = std::getenv("TMPDIR");
                std::string name = std::string(temp && *temp ? temp : "/tmp") + "/kestrel-hud-XXXXXX";
                fd_ = mkstemp(name.data()); if (fd_ >= 0) { unlink(name.c_str()); fcntl(fd_, F_SETFD, FD_CLOEXEC); }
            }
            if (fd_ < 0 || ftruncate(fd_, off_t(size_))) throw std::runtime_error("Cannot allocate HUD shared buffer");
            if (sealable && fcntl(fd_, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
                throw std::runtime_error("Cannot seal HUD buffer size");
        }
        struct stat st{};
        if (fstat(fd_, &st) || !S_ISREG(st.st_mode) || st.st_size != off_t(size_)) throw std::runtime_error("HUD shared-buffer size/type mismatch");
        void* map = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (map == MAP_FAILED) throw std::runtime_error("Cannot map HUD shared buffer");
        mapping_ = static_cast<uint8_t*>(map);
    } catch (...) { if (fd_ >= 0) close(fd_); fd_ = -1; throw; }
}
HudBuffer::~HudBuffer() { if (mapping_) munmap(mapping_, size_); if (fd_ >= 0) close(fd_); }
uint8_t* HudBuffer::region(uint32_t slot, size_t index) const {
    if (slot > 1 || index >= layout_.regions.size()) throw std::out_of_range("HUD slot/region");
    return mapping_ + slot * layout_.bytes() + layout_.offset(index);
}
} // namespace kestrel_ui
