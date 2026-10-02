# librga headers

The im2d API headers of Rockchip's RGA library, from
https://github.com/airockchip/librga at commit 72da645 (1.10.0_[5]), unchanged.
Licensed under the Apache License 2.0 (COPYING).

The library itself is not built here: the goggle's stock firmware ships
`librga.so.2` (1.10.0_[9]), which fpvOS installs from the vendor runtime
(fpvos-vendor-libs). Its headers are not part of that, so these are kept
beside the code that uses them (rga_compositor.cpp). The library takes
them without complaint; 1.10.0_[9] is just a later build of the same API.
