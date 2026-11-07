#pragma once
// system(cmd), but the shell gets none of kestrel's descriptors. system()
// handed it every one not marked CLOEXEC, and what the command leaves running
// (hostapd and udhcpd, the radio daemon, a restart's background shell) kept
// them: the decoder's buffers, the screen's DRM file. Same return value as
// system(); the shell starts with no signal blocked.
int run_shell(const char* cmd);
