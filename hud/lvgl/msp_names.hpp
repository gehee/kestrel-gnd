#pragma once
#include <cstdint>
namespace kestrel_ui {
inline const char* msp_name(uint16_t function) {
    switch (function) {
    case 1: return "API_VERSION";
    case 2: return "FC_VARIANT";
    case 3: return "FC_VERSION";
    case 101: return "STATUS";
    case 110: return "ANALOG";
    case 150: return "STATUS_EX";
    case 182: return "DISPLAYPORT";
    default: return "OTHER";
    }
}
}
