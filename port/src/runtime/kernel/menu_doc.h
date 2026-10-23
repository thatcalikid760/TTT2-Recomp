// Run-time edits of TTT2's Options / main-menu BX01 document (the PC rows and the EXIT item;
// see port/MENU_PLAN.md). Pure byte-level work: no guest access, testable on a dumped document.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace menu_doc {

struct PcLists
{
    std::vector<std::string> modes;          // DISPLAY MODE values, index = display_settings::Mode
    std::vector<std::string> resolutions;    // RESOLUTION values, index = resolution index
};

// The edited document, or an empty string when `doc` is not the Options / main-menu document or
// an edit's anchor is missing (`why` says which); the game then keeps its own document.
// `note` reports an optional edit that was left out (the rest still applies).
std::string ApplyPcEdits(const uint8_t* doc, size_t size, const PcLists& lists, std::string* why,
                         std::string* note = nullptr);

} // namespace menu_doc
