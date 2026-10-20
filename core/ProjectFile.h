#pragma once

// Project files (.veproj): JSON with an OpenTimelineIO-like layout (schema-tagged objects, tracks
// with ordered children, times in flicks). Media paths are stored absolute and relative to the
// project file, so a project folder can be moved as a whole. Pure C++ (no Qt).
// SQLite with incremental autosave replaces this in a later phase (spec §11); the JSON form stays
// for golden test states and interchange.

#include "core/Json.h"
#include "core/Model.h"

#include <optional>
#include <string>

namespace ve {

inline constexpr const char* kProjectExtension = "veproj";

json::Value projectToJson(const Project& project, const std::string& projectDir = {});
std::optional<Project> projectFromJson(const json::Value& json, const std::string& projectDir, std::string* error);

json::Value timelineToJson(const Timeline& timeline);
Timeline timelineFromJson(const json::Value& json);

// Paths are UTF-8. Saving writes a temporary file and renames it over the target.
bool saveProject(const Project& project, const std::string& path, std::string* error);
std::optional<Project> loadProject(const std::string& path, std::string* error);

} // namespace ve
