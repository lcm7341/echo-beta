#pragma once
#include <string>

class GJBaseGameLayer;

GJBaseGameLayer* step_begin(GJBaseGameLayer* self);
void step_end(GJBaseGameLayer* self, GJBaseGameLayer* layer, bool is_half_tick);
void trace_step(GJBaseGameLayer* self);
void trace_button(char what, bool down, int extra);
void trace_event(const std::string& line);
void trace_note(const std::string& line);
void trace_flush();
bool tracing();
std::string step_pace();
void trace_plain(class PlayerObject* player);
