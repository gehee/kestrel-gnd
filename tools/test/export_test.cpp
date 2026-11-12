#include "dvr/export_job.hpp"
#include "dvr/export_source.hpp"
#include "recording_fixture.hpp"
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
namespace fs = std::filesystem;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> static void wait(F condition) {
    for (int i = 0; i < 3000; ++i) {
        if (condition())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("export test timeout");
}
int main(int argc, char **argv) {
    // A child with the real worker's protocol; deliberately touch only OUTPUT.
    if (argc == 4) {
        FILE *f = fopen(argv[2], "wb");
        if (!f)
            return 1;
        const std::string mode = argv[3];
        fputs("test-only finalized video contents", f);
        fclose(f);
        puts("EXPORT PROGRESS 40");
        fflush(stdout);
        if (mode == "cancel")
            for (;;)
                pause();
        if (mode == "failure") {
            puts("EXPORT ERROR injected encoder error");
            return 1;
        }
        if (mode == "crash")
            return 2;
        puts("EXPORT DONE");
        return 0;
    }
    try {
        char temp[] = "export-test-XXXXXX";
        check(mkdtemp(temp), "test directory");
        struct Cleanup {
            std::string p;
            ~Cleanup() { fs::remove_all(p); }
        } cleanup{temp};
        const std::string directory = fs::absolute(temp), input = directory + "/clip.mp4";
        FILE *f = fopen(input.c_str(), "wb");
        check(f, "fixture file");
        auto *mux = MP4E_open(0, 0, f, [](int64_t off, const void *data, size_t size, void *c) -> int {
            FILE *file = static_cast<FILE *>(c);
            return fseeko(file, off, SEEK_SET) || fwrite(data, 1, size, file) != size;
        });
        mp4_h26x_writer_t writer{};
        check(mp4_h26x_write_init(&writer, mux, 64, 64, 1) == 0, "fixture track");
        for (const auto &p : recording_ps)
            check(mp4_h26x_write_nal(&writer, p.data(), p.size(), 1500) == 0, "fixture parameters");
        for (unsigned i = 0; i < recording_frames.size(); ++i) {
            const auto &p = recording_frames[i];
            check(mp4_h26x_write_nal(&writer, p.data(), p.size(), 1500 + i * 3) == 0, "fixture sample");
        }
        check(MP4E_close(mux) == 0, "fixture finalize");
        mp4_h26x_write_close(&writer);
        fclose(f);
        const auto original = fs::file_size(input);
        {
            dvr::ExportSource source(input);
            check(source.count == 8 && source.width == 64 && source.parameters.size() == 3,
                  "source metadata");
            uint64_t ticks = 0;
            for (unsigned i = 0; i < 8; ++i) {
                auto s = source.sample(i);
                check(s.bytes == recording_frames[i], "Annex B access unit preserved");
                check(s.ticks == 1500 + i * 3 && s.us == ticks * 1000000 / 90000,
                      "variable frame timing preserved");
                ticks += s.ticks;
            }
            check(source.duration_us == ticks * 1000000 / 90000, "duration preserved");
        }
        {
            std::ifstream in(input, std::ios::binary);
            std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            const std::string tag="stts";
            auto at=std::search(bytes.begin(),bytes.end(),tag.begin(),tag.end());
            check(at!=bytes.end() && bytes.end()-at>=16,"fixture timing box");
            // Remove one timestamp while leaving eight indexed picture samples.
            std::fill(at+12,at+16,0);
            const auto path=directory+"/short-timing.mp4";
            {std::ofstream out(path,std::ios::binary);out.write(bytes.data(),bytes.size());}
            bool rejected=false;
            try{dvr::ExportSource bad(path);}catch(const std::exception&){rejected=true;}
            check(rejected,"short MP4 timestamp table rejected before indexing");
        }
        fs::create_symlink(input, directory + "/symlink.mp4");
        bool rejected = false;
        try {
            dvr::ExportSource bad(directory + "/symlink.mp4");
        } catch (...) {
            rejected = true;
        }
        check(rejected, "symlink source rejected");
        {
            std::ofstream bad(directory + "/bad.mp4", std::ios::binary);
            const char bytes[] = {0, 0, 0, 16, 'c', 't', 't', 's', 0, 0, 0, 0, 0, 0, 0, 0};
            bad.write(bytes, 16);
        }
        rejected = false;
        try {
            dvr::ExportSource bad(directory + "/bad.mp4");
        } catch (...) {
            rejected = true;
        }
        check(rejected, "composition offsets rejected");
        std::atomic<bool> suspended = false;
        std::atomic<unsigned> held = 0, resumed = 0;
        dvr::ExportJob job(directory, fs::read_symlink("/proc/self/exe"),
                           {[&](bool on) {
                                suspended = on;
                                if (on)
                                    ++held;
                                else
                                    ++resumed;
                            },
                            [&] { return suspended.load(); }});
        rejected = false;
        try {
            job.start("../clip.mp4", {"success"});
        } catch (...) {
            rejected = true;
        }
        check(rejected, "unsafe name rejected");
        job.start("clip.mp4", {"success"});
        wait([&] { return !job.state().busy; });
        auto first = job.state();
        check(!first.output.empty() && fs::exists(first.output) && first.percent == 100,
              "successful result published");
        check(!suspended && held == 1 && resumed == 1, "media restored after success");
        job.start("clip.mp4", {"success"});
        wait([&] { return !job.state().busy; });
        check(job.state().output != first.output && fs::exists(first.output),
              "repeat export does not overwrite");
        for (const std::string mode : {"cancel", "failure", "crash"}) {
            job.start("clip.mp4", {mode});
            if (mode == "cancel") {
                wait([&] { return job.state().percent == 40; });
                job.cancel();
            }
            wait([&] { return !job.state().busy; });
            check(job.state().output.empty() && !suspended,
                  "failed export restores media without publishing");
        }
        for (const auto &entry : fs::directory_iterator(directory + "/exports"))
            check(entry.path().filename().string()[0] != '.', "partial output cleaned");
        check(fs::file_size(input) == original && held == resumed,
              "original preserved and all resource leases released");
        {
            // What a worker that outlived a crashed kestrel-gnd leaves behind.
            std::ofstream(directory + "/exports/.hud-Stale1") << "partial";
            std::ofstream(directory + "/exports/.hud-notours") << "kept";
            dvr::ExportJob restarted(directory, fs::read_symlink("/proc/self/exe"),
                                     {[](bool) {}, [] { return true; }});
            check(!fs::exists(directory + "/exports/.hud-Stale1") && fs::exists(directory + "/exports/.hud-notours"),
                  "stale partial removed at start, other files kept");
            fs::remove(directory + "/exports/.hud-notours");
        }
        {
            dvr::ExportJob shutdown(directory, fs::read_symlink("/proc/self/exe"),
                                    {[&](bool on) { suspended = on; }, [&] { return suspended.load(); }});
            shutdown.start("clip.mp4", {"cancel"});
            wait([&] { return shutdown.state().percent == 40; });
        }
        check(!suspended, "destructor cancels and joins worker before returning media");
        puts("PASS export timing, bounds, process lifecycle, cancellation, publication and original "
             "preservation");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
