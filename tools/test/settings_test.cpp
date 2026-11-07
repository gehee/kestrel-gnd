// Host test of the settings file (base/settings.hpp): the typed settings read
// their defaults until set, a value goes to the file in the text it always
// had ("true", "42", "1.500000"), comes back from it, and a value the file
// holds in some other form reads as the default. The file is replaced whole
// (a .tmp renamed over it, a link followed to its target), and a write that
// fails is tried again by the next save.
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "base/settings.hpp"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static const Setting<int> kInt{"an_int", 7};
static const Setting<bool> kBool{"a_bool", true};
static const Setting<float> kFloat{"a_float", 0.25f};
static const Setting<std::string> kText{"a_text", "dflt"};

static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int main() {
    char path[] = "/tmp/settings_test_XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) { std::printf("FAIL: no temp file\n"); return 1; }
    close(fd);

    {
        Settings s;
        s.load(path);   // empty: every setting reads its default
        CHECK(s.get(kInt) == 7 && s.get(kBool) && s.get(kFloat) == 0.25f && s.get(kText) == "dflt");
        CHECK(s.text(kInt).empty());
        s.set(kInt, 42);
        s.set(kBool, false);
        s.set(kFloat, 1.5f);
        s.set(kText, "hello");
        s.set_text(Setting<float>{"a_period", 0.0f}, "16666.667");
        CHECK(s.get(kInt) == 42 && !s.get(kBool) && s.get(kFloat) == 1.5f && s.get(kText) == "hello");
        s.flush();
    }
    const std::string file = slurp(path);
    CHECK(file.find("an_int: 42\n") != std::string::npos);
    CHECK(file.find("a_bool: false\n") != std::string::npos);
    CHECK(file.find("a_float: 1.500000\n") != std::string::npos);
    CHECK(file.find("a_text: hello\n") != std::string::npos);
    CHECK(file.find("a_period: 16666.667\n") != std::string::npos);

    {
        Settings s;
        s.load(path);
        CHECK(s.get(kInt) == 42 && !s.get(kBool) && s.get(kFloat) == 1.5f && s.get(kText) == "hello");
        CHECK(s.get(Setting<float>{"a_period", 0.0f}) > 16666.66f);
        s.remove(kText);
        CHECK(s.get(kText) == "dflt");
        // Another form than the setting's: its default (an old bool where a
        // number is read now, as hud_reactivity's migration expects).
        s.set_text(kInt, "true");
        CHECK(s.get(kInt) == 7 && s.text(kInt) == "true");
        s.set_text(kBool, "1");
        CHECK(s.get(kBool));
        s.set_text(kBool, "maybe");
        CHECK(s.get(kBool));   // the default
        s.flush();
    }
    CHECK(slurp(path).find("a_text") == std::string::npos);
    CHECK(access((std::string(path) + ".tmp").c_str(), F_OK) != 0);   // none left behind
    unlink(path);

    char dir[] = "/tmp/settings_test_dir_XXXXXX";
    if (!mkdtemp(dir)) { std::printf("FAIL: no temp dir\n"); return 1; }
    const std::string d = dir;

    // A link: its target is written, the link stays a link - also one whose
    // target is not made yet.
    {
        CHECK(symlink("real.yaml", (d + "/link.yaml").c_str()) == 0);
        Settings s;
        s.load(d + "/link.yaml");
        s.set(kInt, 5);
        s.flush();
        struct stat st;
        CHECK(lstat((d + "/link.yaml").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
        CHECK(slurp(d + "/real.yaml").find("an_int: 5\n") != std::string::npos);
        s.set(kInt, 6);
        s.flush();
        CHECK(lstat((d + "/link.yaml").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
        CHECK(slurp(d + "/real.yaml").find("an_int: 6\n") != std::string::npos);
        CHECK(access((d + "/real.yaml.tmp").c_str(), F_OK) != 0);
    }
    unlink((d + "/link.yaml").c_str());
    unlink((d + "/real.yaml").c_str());

    // A write that cannot be done (its folder is not there - so even as
    // root) stays unsaved, and the next flush writes it once it can.
    {
        const std::string sub = d + "/sub";
        Settings s;
        s.load(sub + "/k.yaml");
        s.set(kInt, 9);
        s.flush();
        CHECK(access((sub + "/k.yaml").c_str(), F_OK) != 0);
        CHECK(mkdir(sub.c_str(), 0755) == 0);
        s.flush();
        CHECK(slurp(sub + "/k.yaml").find("an_int: 9\n") != std::string::npos);
        CHECK(access((sub + "/k.yaml.tmp").c_str(), F_OK) != 0);
        unlink((sub + "/k.yaml").c_str());
        rmdir(sub.c_str());
    }
    rmdir(dir);
    if (fails) return 1;
    std::printf("settings_test: ok\n");
    return 0;
}
