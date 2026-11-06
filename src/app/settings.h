// User settings: FarCryInstincts.ini beside the executable ("key = value" lines, '#' or ';' comments, [sections]
// ignored), each overridden by an environment variable FCI_<KEY> (upper case). Read once, on first use.
#pragma once

#include <string>

namespace settings {

// the value of key, or fallback when neither the environment nor the ini file sets it
int Int(const char* key, int fallback);
float Float(const char* key, float fallback);
std::string String(const char* key, const char* fallback);
// ---- PC comfort features: 1/0, true/false, yes/no, on/off ----
bool Bool(const char* key, bool fallback);

// ---- device presets: defaults of the device the game runs on (the Steam Deck: GTAO, shadow_scale 4), used where
// neither the environment, the file nor the menu sets a key ----
// the device's default for key, else fallback (the settings menu shows and resets to it)
std::string Default(const char* key, const char* fallback);
// "Steam Deck OLED", "Steam Deck LCD", "Steam Deck" or "" (none detected)
const std::string& Device();
// ---- end device presets ----

// ---- settings menu (app/menu.cpp) ----
// a new value for key: from now on it wins over the environment and the file (the getters above return it);
// Save writes it into FarCryInstincts.ini
void Set(const char* key, const std::string& value);
// the value key had when the program started (environment, else the file), fallback when it had none: rows that
// only take effect after a restart compare against it
std::string Boot(const char* key, const char* fallback);
// FCI_<KEY> is set: the environment decided the value at start (a saved change applies at the next start only
// when the variable is gone)
bool FromEnvironment(const char* key);
// writes the values Set changed into FarCryInstincts.ini: a key's existing line keeps its place and its comment,
// a new key goes at the end, every other line (comments, unknown keys, sections) stays as it was; false on failure
bool Save();
// ---- end settings menu ----

}  // namespace settings
