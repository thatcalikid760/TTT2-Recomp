// On-screen notifications (the presenter draws them, top right): a short title line and a few
// lines of text that fade in, stay a while and fade out. One at a time; a new one replaces it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace toast {

struct Note
{
    std::string title;                 // accent line
    std::vector<std::string> lines;    // UTF-8
    bool highlight = false;            // an update is available: orange accent instead of blue
};

void Show(const Note& note, int milliseconds);

// The presenter: the note on screen now and its opacity (0..1); false when none. `generation`
// changes with every new note (the presenter re-renders its texture then).
bool Current(Note& note, float& opacity, uint32_t& generation);

} // namespace toast
