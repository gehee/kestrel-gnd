// The zone the fpvOS Flasher leaves for the goggle at a flash: taken only when
// none is set, saved, and its file removed either way.
#include "region/settings.hpp"
#include "region/zones.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static void check(bool ok, const char* what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
    printf("ok: %s\n", what);
}

static void put(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "w");
    check(f && fputs(text, f) >= 0 && fclose(f) == 0, "write the location file");
}

int main() {
    char dir[] = "zones-location-test-XXXXXX";
    check(mkdtemp(dir), "Create private test directory");
    const std::string loc = std::string(dir) + "/location";
    {
        Settings settings; settings.load(std::string(dir) + "/s1");
        put(loc, "TZ='CET-1CEST,M3.5.0,M10.5.0/3'\nZONE='Europe/Paris'\nCOUNTRY='FR'\n");
        zones::load(settings, loc.c_str());
        check(zones::selected() == zones::find("Europe/Paris"), "Not set: the flash's zone is taken");
        check(settings.get(region::setting::time_zone) == "Europe/Paris", "and saved");
        check(access(loc.c_str(), F_OK) != 0, "and its file removed");
        check(std::string(getenv("TZ")) == "CET-1CEST,M3.5.0,M10.5.0/3", "and applied");
        settings.flush();
    }
    {
        Settings settings; settings.load(std::string(dir) + "/s2");
        settings.set(region::setting::time_zone, std::string("America/Toronto"));
        put(loc, "TZ='CET-1CEST,M3.5.0,M10.5.0/3'\nZONE='Europe/Paris'\nCOUNTRY='FR'\n");
        zones::load(settings, loc.c_str());
        check(zones::selected() == zones::find("America/Toronto"), "Set: the pilot's zone stays");
        check(access(loc.c_str(), F_OK) != 0, "and the file is removed all the same");
        settings.flush();
    }
    {
        Settings settings; settings.load(std::string(dir) + "/s3");
        put(loc, "TZ='<+0530>-5:30'\nZONE='Mars/Olympus'\nCOUNTRY=''\n");
        zones::load(settings, loc.c_str());
        check(zones::selected() == -1 && settings.get(region::setting::time_zone).empty(), "A zone tzdata does not know is not taken");
        put(loc, "ZONE='Asia/Calcutta'\n");
        zones::load(settings, loc.c_str());
        check(zones::selected() == zones::find("Asia/Kolkata"), "An old name finds its zone");
        settings.flush();
    }
    {
        Settings settings; settings.load(std::string(dir) + "/s4");
        zones::load(settings, (std::string(dir) + "/none").c_str());
        check(zones::selected() == -1, "No file: still not set");
        settings.flush();
    }
    std::string cmd = std::string("rm -rf ") + dir;
    return system(cmd.c_str());
}
