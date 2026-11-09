// A small test framework: TEST registers a function, CHECK records a failure and continues.
#pragma once
#include <cstdio>
#include <string>

namespace t {
struct Case {
    const char* name;
    void (*fn)();
    Case* next;
};
Case*& head();
int& failures();
struct Reg {
    Reg(Case* c) {
        c->next = head();
        head() = c;
    }
};
void fail(const char* file, int line, const std::string& what);
std::string root();  // repository root
}  // namespace t

#define TEST(name)                                     \
    static void test_##name();                         \
    static t::Case case_##name{#name, test_##name, nullptr}; \
    static t::Reg reg_##name(&case_##name);            \
    static void test_##name()

#define CHECK(cond)                                         \
    do {                                                    \
        if (!(cond)) t::fail(__FILE__, __LINE__, #cond);    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va_ = (a);                                                                \
        const auto vb_ = (b);                                                                \
        if (!(va_ == vb_))                                                                   \
            t::fail(__FILE__, __LINE__, std::string(#a " == " #b " (") + std::to_string(va_) + \
                                            " vs " + std::to_string(vb_) + ")");             \
    } while (0)

#define CHECK_STR(a, b)                                                                              \
    do {                                                                                             \
        const std::string va_ = (a), vb_ = (b);                                                      \
        if (va_ != vb_) t::fail(__FILE__, __LINE__, std::string(#a " == \"") + vb_ + "\" (got \"" + va_ + "\")"); \
    } while (0)
