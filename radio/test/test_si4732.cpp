// The receiver driver's pure checks: the opcode guard, the SSB patch format and the AM command layout.
#include <vector>

#include "radio_model.h"
#include "si4732.h"
#include "test.h"

TEST(opcode_guard) {
    for (int op = 0; op < 0x100; op++) CHECK_EQ(si4732::opcodeAllowed(static_cast<uint8_t>(op)), op < 0xF0);
}

TEST(patch_format) {
    std::vector<uint8_t> p = {0x15, 0, 1, 2, 3, 4, 5, 6, 0x16, 9, 9, 9, 9, 9, 9, 9};
    CHECK(si4732::patchValid(p.data(), p.size()));
    CHECK(!si4732::patchValid(p.data(), 12));  // not whole records
    CHECK(!si4732::patchValid(nullptr, 0));
    p[8] = 0xF5;  // a record that is not a patch download command
    CHECK(!si4732::patchValid(p.data(), p.size()));
}

TEST(am_command_antcap) {
    using Cmd = std::array<uint8_t, 6>;
    // AM_TUNE_FREQ on MW (automatic capacitor) and SW (capacitor 1).
    CHECK(si4732::amCommand(0x40, 0, 999, radio::amAntennaCap(999)) == (Cmd{0x40, 0, 0x03, 0xE7, 0, 0}));
    CHECK(si4732::amCommand(0x40, 0, 17490, radio::amAntennaCap(17490)) == (Cmd{0x40, 0, 0x44, 0x52, 0, 1}));
    // SSB_TUNE_FREQ (USB) and AM_SEEK_START carry it in the same place.
    CHECK(si4732::amCommand(0x40, 0x80, 14200, radio::amAntennaCap(14200)) == (Cmd{0x40, 0x80, 0x37, 0x78, 0, 1}));
    CHECK(si4732::amCommand(0x41, 0x0C, 0, 0x1234) == (Cmd{0x41, 0x0C, 0, 0, 0x12, 0x34}));
}
