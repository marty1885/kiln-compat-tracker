#pragma once
#include <functional>
#include <string>
namespace kiln {
    inline std::string log_dir; // empty = logging disabled
    inline std::string github_token;
    inline std::function<void()> poll_kiln_head; // set in main.cpp after event loop init
}
