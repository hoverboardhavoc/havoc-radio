// Helpers shared by the host tests.
#pragma once
#include <fstream>
#include <sstream>
#include <string>

#include "gfx.h"
#include "test.h"

inline std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::string prototype() { return readFile(t::root() + "/radio/ui/prototype.html"); }

// Text of the JavaScript object or array literal that starts at the first `open` after `start`.
inline std::string jsBlock(const std::string& src, const std::string& start, char open = '{') {
    const char close = open == '{' ? '}' : ']';
    size_t i = src.find(start);
    if (i == std::string::npos) return "";
    i = src.find(open, i);
    int depth = 0;
    for (size_t j = i; j < src.size(); j++) {
        if (src[j] == open) depth++;
        if (src[j] == close && --depth == 0) return src.substr(i + 1, j - i - 1);
    }
    return "";
}

// Frame as 32 rows of 128 '0'/'1' characters, the PBM P1 body.
inline std::string frameBits(const gfx::Frame& f) {
    std::string s;
    for (int y = 0; y < gfx::H; y++)
        for (int x = 0; x < gfx::W; x++) s += f.get(x, y) ? '1' : '0';
    return s;
}
