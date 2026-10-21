// Local player profiles (the console's signed-in profile, PROFILE_PLAN.md): a list in
// <save root>/profiles.ini, each with a name (the gamertag the game shows) and an offline XUID
// that names its save folder. The existing single-profile save ("TTT2", B13EBABEBABEBABE) is
// profile 1. Nobody is signed in at boot unless TTT2_AUTO_SIGNIN=1 (scripted runs).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace profiles {

struct Profile { std::string name; uint64_t xuid = 0; };

constexpr size_t kMaxProfiles = 8;
constexpr size_t kMaxName = 15;           // an Xbox gamertag's length

std::vector<Profile> List();
bool SignedIn();
int ActiveIndex();                        // -1 = nobody signed in
Profile Active();                         // empty when signed out
uint64_t ActiveXuid();                    // 0 when signed out
int LastIndex();                          // the profile used last (the picker's default)
bool SignIn(int index);                   // remembers it as the last profile
void SignOut();
// A new profile (not signed in); returns its index, or -1 when the list is full or the name
// is not valid (1-15 characters: letters, digits, spaces; not all spaces; unique).
int Create(const std::string& name);
bool ValidName(const std::string& name);
std::string Normalize(const std::string& name);   // trims, collapses spaces

// Bumped on every sign-in change (the XAM layer posts XN_SYS_SIGNINCHANGED).
uint32_t Generation();

} // namespace profiles
