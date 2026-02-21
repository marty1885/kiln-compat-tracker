#pragma once

#include <cstdint>
#include <string>

namespace kiln {

// Resource tiers — maps to DB enum
enum class ResourceTier { Small, Medium, Large };

inline std::string resource_tier_to_string(ResourceTier t) {
    switch (t) {
        case ResourceTier::Small: return "small";
        case ResourceTier::Medium: return "medium";
        case ResourceTier::Large: return "large";
    }
    return "small";
}

inline ResourceTier resource_tier_from_string(const std::string& s) {
    if (s == "medium") return ResourceTier::Medium;
    if (s == "large") return ResourceTier::Large;
    return ResourceTier::Small;
}

// Dependency levels — maps to DB enum
enum class DepLevel { Base, Moderate, Full };

inline std::string dep_level_to_string(DepLevel l) {
    switch (l) {
        case DepLevel::Base: return "base";
        case DepLevel::Moderate: return "moderate";
        case DepLevel::Full: return "full";
    }
    return "base";
}

inline DepLevel dep_level_from_string(const std::string& s) {
    if (s == "moderate") return DepLevel::Moderate;
    if (s == "full") return DepLevel::Full;
    return DepLevel::Base;
}

// Build status
enum class BuildStatus { Pass, Fail, Timeout, Error };

inline std::string build_status_to_string(BuildStatus s) {
    switch (s) {
        case BuildStatus::Pass: return "pass";
        case BuildStatus::Fail: return "fail";
        case BuildStatus::Timeout: return "timeout";
        case BuildStatus::Error: return "error";
    }
    return "error";
}

inline BuildStatus build_status_from_string(const std::string& s) {
    if (s == "pass") return BuildStatus::Pass;
    if (s == "fail") return BuildStatus::Fail;
    if (s == "timeout") return BuildStatus::Timeout;
    return BuildStatus::Error;
}

} // namespace kiln
