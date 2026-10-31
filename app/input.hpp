#pragma once
// The keys, into the OSD: the VRX Pro's front buttons and its BIND button
// (ADC channels, polled), and keyboards (the terminal, evdev devices). Each
// is a thread of its own (pthread entries, the OSD their argument), until
// signal_stop.

class OSD;
namespace gnd { class Radio; }

// What BIND opens the pairing window of; before the threads start.
void input_set_radio(gnd::Radio* r);
void* vrx_button_thread_func(void* osd);
void* kb_thread_func(void* osd);
