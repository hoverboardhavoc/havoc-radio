// Font and icon tables against the prototype's PIP, FACET and ICONS tables, parsed here independently of
// the generator, and the drawing primitives.
#include <regex>
#include <vector>

#include "util.h"

namespace {

struct ProtoGlyph {
    uint32_t cp;
    std::vector<std::string> rows;
};

uint32_t firstCp(const std::string& s) {
    const char* p = s.c_str();
    return gfx::nextCp(p);
}

std::vector<ProtoGlyph> protoPip() {
    const std::string body = jsBlock(prototype(), "const PIP =");
    std::vector<ProtoGlyph> out;
    // KEY: G('rows'), KEY is 'x', "x" or a bare letter or digit.
    const std::regex re(R"RE((?:'([^']+)'|"([^"]+)"|([A-Za-z0-9]))\s*:\s*G\('([^']*)'\))RE");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        const std::string key = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();
        ProtoGlyph g{firstCp(key), {}};
        std::stringstream ss(m[4].str());
        std::string row;
        while (ss >> row) g.rows.push_back(row);
        out.push_back(g);
    }
    return out;
}

}  // namespace

TEST(pip_matches_prototype) {
    const auto proto = protoPip();
    CHECK(proto.size() > 150);
    CHECK_EQ(static_cast<int>(proto.size()), gfx::kPipCount);
    for (const auto& g : proto) {
        const gfx::PipGlyph* p = gfx::pipGlyph(g.cp);
        if (!p) {
            t::fail(__FILE__, __LINE__, "missing glyph U+" + std::to_string(g.cp));
            continue;
        }
        CHECK_EQ(static_cast<size_t>(p->rows), g.rows.size());
        CHECK_EQ(static_cast<size_t>(p->width), g.rows[0].size());
        for (size_t j = 0; j < g.rows.size(); j++)
            for (size_t i = 0; i < g.rows[j].size(); i++)
                if (((p->bits[j] >> i) & 1) != (g.rows[j][i] == '#'))
                    t::fail(__FILE__, __LINE__, "glyph U+" + std::to_string(g.cp) + " differs");
    }
}

TEST(pip_cell_rules) {
    // Glyphs have 6 rows, or 7 with a descender, and are at most 8 wide.
    for (int i = 0; i < gfx::kPipCount; i++) {
        CHECK(gfx::kPip[i].rows == 6 || gfx::kPip[i].rows == 7);
        CHECK(gfx::kPip[i].width >= 1 && gfx::kPip[i].width <= 8);
    }
}

TEST(facet_matches_prototype) {
    const std::string body = jsBlock(prototype(), "const FACET =");
    const std::regex entry(R"RE((?:'(.)'|(\d))\s*:\s*\[(\[\[.*?\]\])\]\s*,)RE");
    const std::regex point(R"((\d+),(\d+))");
    int n = 0;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), entry); it != std::sregex_iterator(); ++it) {
        const char ch = (*it)[1].matched ? (*it)[1].str()[0] : (*it)[2].str()[0];
        // Render the prototype strokes with penLine and compare with big().
        gfx::Frame a, b;
        a.clear();
        b.clear();
        const std::string lines = (*it)[3].str();
        size_t pos = 0;
        while ((pos = lines.find("[[", pos)) != std::string::npos) {
            const size_t end = lines.find("]]", pos);
            const std::string pl = lines.substr(pos, end - pos + 2);
            std::vector<std::pair<int, int>> pts;
            for (auto p = std::sregex_iterator(pl.begin(), pl.end(), point); p != std::sregex_iterator(); ++p)
                pts.emplace_back(std::stoi((*p)[1]), std::stoi((*p)[2]));
            for (size_t i = 1; i < pts.size(); i++)
                gfx::penLine(a, 3 + pts[i - 1].first, 5 + pts[i - 1].second, 3 + pts[i].first, 5 + pts[i].second);
            pos = end + 2;
        }
        const char s[2] = {ch, 0};
        gfx::big(b, 3, 5, s);
        if (frameBits(a) != frameBits(b)) t::fail(__FILE__, __LINE__, std::string("facet glyph ") + ch);
        n++;
    }
    CHECK_EQ(n, gfx::kFacetCount);
}

TEST(icons_match_prototype) {
    const std::string body = jsBlock(prototype(), "const ICONS =");
    const char* names[] = {"presets", "settings", "volume", "muted"};
    for (int k = 0; k < 4; k++) {
        const std::regex re(std::string(names[k]) + R"(\s*:\s*\[([^\]]*)\])");
        std::smatch m;
        CHECK(std::regex_search(body, m, re));
        const std::string rows = m[1].str();
        const std::regex row("'([.#]+)'");
        int j = 0;
        gfx::Frame f;
        f.clear();
        gfx::icon(f, 0, 0, static_cast<gfx::Icon>(k));
        for (auto it = std::sregex_iterator(rows.begin(), rows.end(), row); it != std::sregex_iterator(); ++it, j++) {
            const std::string r = (*it)[1].str();
            for (size_t i = 0; i < r.size(); i++)
                if (f.get(static_cast<int>(i), j) != (r[i] == '#')) t::fail(__FILE__, __LINE__, names[k]);
        }
        CHECK_EQ(j, 8);
    }
}

TEST(text_metrics) {
    CHECK_EQ(gfx::textW(""), 0);
    CHECK_EQ(gfx::textW("A"), 4);
    CHECK_EQ(gfx::textW("AM"), 4 + 1 + 5);
    CHECK_EQ(gfx::textW("◀"), 3);
    // A character the font lacks measures as '?' but draws as a space.
    CHECK_EQ(gfx::textW("~"), gfx::textW("?"));
    gfx::Frame f;
    f.clear();
    CHECK_EQ(gfx::text(f, 0, 0, "~"), 3);
    CHECK_EQ(gfx::bigW("100.70"), 13 * 5 + 5 - 3);
}

TEST(text_draws_below_margin_row) {
    gfx::Frame f;
    f.clear();
    gfx::text(f, 0, 0, "I");
    for (int x = 0; x < 3; x++) CHECK(!f.get(x, 0));
    CHECK(f.get(0, 1) && f.get(1, 1) && f.get(2, 1));
    CHECK(f.get(1, 3));
    gfx::text(f, 0, 8, "I", 1);
    f.fill(0, 8, 8, 8, 1);
    gfx::text(f, 0, 8, "I", 0);
    CHECK(!f.get(1, 10) && f.get(0, 10));
}

TEST(frame_page_layout) {
    gfx::Frame f;
    f.clear();
    f.px(5, 0, 1);
    f.px(5, 9, 1);
    f.px(-1, 0, 1);
    f.px(200, 40, 1);
    CHECK_EQ(f.buf[5], 1);
    CHECK_EQ(f.buf[128 + 5], 2);
    int lit = 0;
    for (uint8_t b : f.buf) lit += __builtin_popcount(b);
    CHECK_EQ(lit, 2);
}
