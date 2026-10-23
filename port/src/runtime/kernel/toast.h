// On-screen notifications (the presenter draws them, top right): a short title line and a few
// lines of text that fade in, stay a while (or until hidden) and fade out. One at a time; a new
// one replaces it. A note can take mouse clicks: the presenter reports where it drew it and the
// game window passes clicks inside that rectangle here instead of to the game.
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
    bool clickable = false;            // takes clicks (TakeClick)
};

// `milliseconds` <= 0: stays until Hide.
void Show(const Note& note, int milliseconds);
// The note on screen gets new text (no new fade); does nothing when none is showing.
void Update(const Note& note);
void Hide();                           // fades out
bool Showing();

// The presenter: the note on screen now and its opacity (0..1); false when none. `generation`
// changes with every new text (the presenter re-renders its texture then).
bool Current(Note& note, float& opacity, uint32_t& generation);
// The presenter: where the note is drawn, in the window's client pixels (none = all zero).
void SetRect(int left, int top, int right, int bottom);

// The window thread: a click at client (x, y); true when it was on a clickable note (the click
// is then the note's: 1 left, 2 right).
bool OnClick(int x, int y, bool right);
bool Hovered(int x, int y);
// The clicks since the last call: 0 none, 1 left, 2 right (the last one wins).
int TakeClick();

} // namespace toast
