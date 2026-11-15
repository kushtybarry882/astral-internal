#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ConfigIO {

std::filesystem::path ConfigDirectory();
std::string ConfigPath();
std::vector<std::string> ListProfiles();

bool SaveProfile(const std::string& name, std::string* actualName, std::string* error);
bool LoadProfile(const std::string& name, std::string* error);
bool DeleteProfile(const std::string& name, std::string* error);
void AutoLoadDefault();

} // namespace ConfigIO
