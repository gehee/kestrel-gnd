#pragma once
// Stop the app the ordinary way and start it again in place: for a setting
// read only at start, such as the screen mode. How is app/main.cpp's, which
// sets it before anything can ask; until then asking does nothing.
void kestrel_request_restart();
void set_restart_handler(void (*fn)());
