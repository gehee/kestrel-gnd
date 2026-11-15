#pragma once
// Max Bandwidth: how wide the video link may go, in MHz, as saved
// (ar8030_max_bw_mhz) and sent. 0 is AUTO: the radio's own config, nothing
// sent. 5 and 10 only with Open8030 on the goggle (taken live, SET 22);
// stock's radio config caps the width at 20 or 40 (bb_only.sh picks it).
// One table for the legacy menu, the native menu and the startup loader.
static const int kArMaxBwVals[]     = { 0, 5, 10, 20, 40 };
static const char* kArMaxBwLabels[] = { "AUTO", "5 MHz", "10 MHz", "20 MHz", "40 MHz" };
static const int kArMaxBwCount = 5;
// mhz's place in the table; a value it has not reads as 40 MHz.
static inline int ar_maxbw_index(int mhz) {
    for (int i = 0; i < kArMaxBwCount; i++) if (kArMaxBwVals[i] == mhz) return i;
    return kArMaxBwCount - 1;
}
static inline bool ar_maxbw_valid(int mhz) {
    for (int i = 0; i < kArMaxBwCount; i++) if (kArMaxBwVals[i] == mhz) return true;
    return false;
}
