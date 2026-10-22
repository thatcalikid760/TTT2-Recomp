// In-game updates (Options -> UPDATES): the latest release of the port's GitHub repository
// (TTT2_UPDATE_REPO, owner/name; [Update] Repo= in settings.ini overrides it), compared with this
// build's version (TTT2_VERSION). Only a strictly higher version is offered - never a downgrade, and
// a release with the same number is "already up to date". The release's ttt2_setup.exe is
// downloaded to %LOCALAPPDATA%\TTT2Recomp\update (checked against GitHub's SHA-256 when given), then
// the game quits and that installer's /update mode replaces the installed files (Windows asks for
// permission) and the game starts again.
#pragma once
#include <string>

namespace updater {

enum class State { Idle, Checking, UpToDate, Available, Failed, Downloading, Downloaded };

struct Status
{
    State state = State::Idle;
    std::string latest;         // the release's version ("0.1.8"), once checked
    std::string title;          // the release's name
    std::string error;          // Failed: why
    int percent = 0;            // Downloading
};

const char* CurrentVersion();
const std::string& Repo();
Status Get();
void Check();                   // in the background; ignored while busy
void Download();                // in the background, after a check found a newer version
// True when this exe runs from an installed copy (an update can be applied to its folder).
bool CanInstall();
// Starts the installer's update (elevated) with a watcher that restarts the game afterwards.
// On true the caller quits the game at once.
bool Install(std::string& error);

} // namespace updater
