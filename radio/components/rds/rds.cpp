#include "rds.h"

#include <cstdio>
#include <cstring>

namespace rds {

namespace {

// The RDS character table (IEC 62106, annex E) from 0x20 to 0xFE. 0xFF has no character.
const char* const kTable[0xFF - 0x20] = {
    " ", "!", "\"", "#", "¤", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
    "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "\\", "]", "―", "_",
    "‖", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "{", "|", "}", "¯", "",
    "á", "à", "é", "è", "í", "ì", "ó", "ò", "ú", "ù", "Ñ", "Ç", "Ş", "ß", "¡", "Ĳ",
    "â", "ä", "ê", "ë", "î", "ï", "ô", "ö", "û", "ü", "ñ", "ç", "ş", "ğ", "ı", "ĳ",
    "ª", "α", "©", "‰", "Ğ", "ě", "ň", "ő", "π", "€", "£", "$", "←", "↑", "→", "↓",
    "º", "¹", "²", "³", "±", "İ", "ń", "ű", "µ", "¿", "÷", "°", "¼", "½", "¾", "§",
    "Á", "À", "É", "È", "Í", "Ì", "Ó", "Ò", "Ú", "Ù", "Ř", "Č", "Š", "Ž", "Ð", "Ŀ",
    "Â", "Ä", "Ê", "Ë", "Î", "Ï", "Ô", "Ö", "Û", "Ü", "ř", "č", "š", "ž", "đ", "ŀ",
    "Ã", "Å", "Æ", "Œ", "Ŵ", "Ý", "Õ", "Ø", "Þ", "Ŋ", "Ŕ", "Ć", "Ś", "Ź", "Ŧ", "ð",
    "ã", "å", "æ", "œ", "ŵ", "ý", "õ", "ø", "þ", "ŋ", "ŕ", "ć", "ś", "ź", "ŧ",
};

// Codes whose character the Pip font lacks, one bit per code from 0x00: shown as "?" on the radio.
constexpr uint32_t kNoGlyph[8] = {
    0x00000000,  // control codes
    0x00000010,  // ¤
    0x78000000,  // [ \ ] ―
    0x68000001,  // ‖ { } ¯
    0x80008000,  // Ĳ ĳ
    0xFD1FF70F,  // ª α © ‰ π € £ ← ↑ → ↓ º ¹ ² ³ ± µ ÷ ° ¼ ½ ¾ §
    0x80008000,  // Ŀ ŀ
    0x42004200,  // Ŋ Ŧ ŋ ŧ
};

bool noGlyph(uint8_t c) { return kNoGlyph[c >> 5] >> (c & 31) & 1; }

// An AF code as a frequency: FM 87.5 MHz plus code x 0.1 MHz; after code 250, LW from 153 kHz (codes 1 to 15) and
// MW from 531 kHz (16 to 135) in 9 kHz steps.
int32_t afFreq(int32_t code, bool lf) {
    if (!lf) return 87500000 + code * 100000;
    return (code <= 15 ? 153 + (code - 1) * 9 : 531 + (code - 16) * 9) * 1000;
}

// Appends the text at positions 0.. up to the first one not accepted (or a carriage return), drops control codes
// and trailing spaces.
size_t build(const uint8_t* chars, uint64_t acc, int len, bool stopAtCr, bool display, char* out, size_t n) {
    if (!n) return 0;
    size_t used = 0, keep = 0;
    for (int i = 0; i < len && (acc >> i & 1); i++) {
        if (stopAtCr && chars[i] == 0x0D) break;
        const char* s = charText(chars[i], display);
        const size_t k = std::strlen(s);
        if (used + k >= n) break;
        std::memcpy(out + used, s, k);
        used += k;
        if (k && !(k == 1 && *s == ' ')) keep = used;
    }
    out[keep] = 0;
    return keep;
}

}  // namespace

const char* charText(uint8_t code, bool display) {
    if (code < 0x20 || code == 0x7F) return "";
    if (code == 0xFF || (display && noGlyph(code))) return "?";
    return kTable[code - 0x20];
}

bool Decoder::Hold::see(uint16_t v, uint8_t need) {
    if (count && cand == v) {
        if (count < 255) count++;
    } else {
        cand = v;
        count = 1;
    }
    if (count < need) return false;
    const bool changed = !held || value != v;
    value = v;
    held = true;
    return changed;
}

void Decoder::reset() {
    sync_ = false;
    pi_ = pty_ = tp_ = ta_ = Hold{};
    psAcc_ = psSeen_ = 0;
    rtAb_ = -1;
    clearRt();
    afLf_ = false;
    afLfMask_ = 0;
    afCount_ = 0;
}

void Decoder::clearRt() {
    rtAcc_ = rtSeen_ = 0;
    rtSeq_++;
}

void Decoder::text(uint8_t* chars, uint8_t* cand, uint64_t& acc, uint64_t& seen, int pos, uint8_t code) {
    const uint64_t bit = uint64_t{1} << pos;
    if ((seen & bit) && cand[pos] == code) {
        chars[pos] = code;
        acc |= bit;
    }
    cand[pos] = code;
    seen |= bit;
}

void Decoder::af(uint8_t code) {
    const bool lf = afLf_;
    afLf_ = code == 250;
    // 224 to 249 count the frequencies that follow, 205 is a filler, the rest are unused.
    if (afLf_ || !(code >= 1 && code <= (lf ? 135 : 204)) || afCount_ >= kMaxAf) return;
    const int32_t hz = afFreq(code, lf);
    int i = 0;
    while (i < afCount_ && afHz(i) < hz) i++;
    if (i < afCount_ && afHz(i) == hz) return;
    // Kept in ascending order of frequency.
    const uint32_t below = (1u << i) - 1;
    afLfMask_ = (afLfMask_ & below) | (afLfMask_ & ~below) << 1 | (lf ? 1u << i : 0);
    std::memmove(afCode_ + i + 1, afCode_ + i, afCount_ - i);
    afCode_[i] = code;
    afCount_++;
}

int32_t Decoder::afHz(int i) const { return afFreq(afCode_[i], afLfMask_ >> i & 1); }

void Decoder::group(const Group& g) {
    const uint16_t a = g.blk[0], b = g.blk[1], c = g.blk[2], d = g.blk[3];
    const bool typed = g.level(1) == 0;  // the group type only from a block B without errors
    const bool versionB = typed && (b & 0x0800);
    const bool piA = g.level(0) == 0, piC = versionB && g.level(2) == 0;
    if (piA || piC) {
        const bool wasHeld = pi_.held;
        // One PI per group; a B-version group carries it twice.
        if (pi_.see(piA ? a : c, 2) && wasHeld) {
            const Hold pi = pi_;
            const bool sync = sync_;
            reset();
            pi_ = pi;
            sync_ = sync;
        }
    }
    if (!typed) return;
    const int type = b >> 12;
    pty_.see((b >> 5) & 31, 3);
    tp_.see((b >> 10) & 1, 3);
    if (type == 0) {
        ta_.see((b >> 4) & 1, 3);
        if (g.level(3) <= 1) {
            const int pos = (b & 3) * 2;
            text(ps_, psCand_, psAcc_, psSeen_, pos, static_cast<uint8_t>(d >> 8));
            text(ps_, psCand_, psAcc_, psSeen_, pos + 1, static_cast<uint8_t>(d));
        }
        if (!versionB && g.level(2) == 0) {
            af(static_cast<uint8_t>(c >> 8));
            af(static_cast<uint8_t>(c));
        }
        afLf_ = false;
    } else if (type == 2) {
        const int8_t ab = static_cast<int8_t>((b >> 4) & 1);
        if (rtAb_ >= 0 && ab != rtAb_) clearRt();
        rtAb_ = ab;
        const int seg = b & 15;
        if (versionB) {
            if (g.level(3) <= 1) {
                text(rt_, rtCand_, rtAcc_, rtSeen_, seg * 2, static_cast<uint8_t>(d >> 8));
                text(rt_, rtCand_, rtAcc_, rtSeen_, seg * 2 + 1, static_cast<uint8_t>(d));
            }
        } else {
            if (g.level(2) <= 1) {
                text(rt_, rtCand_, rtAcc_, rtSeen_, seg * 4, static_cast<uint8_t>(c >> 8));
                text(rt_, rtCand_, rtAcc_, rtSeen_, seg * 4 + 1, static_cast<uint8_t>(c));
            }
            if (g.level(3) <= 1) {
                text(rt_, rtCand_, rtAcc_, rtSeen_, seg * 4 + 2, static_cast<uint8_t>(d >> 8));
                text(rt_, rtCand_, rtAcc_, rtSeen_, seg * 4 + 3, static_cast<uint8_t>(d));
            }
        }
    }
}

size_t Decoder::ps(char* out, size_t n, bool display) const { return build(ps_, psAcc_, kPsLen, false, display, out, n); }

size_t Decoder::rt(char* out, size_t n, bool display) const { return build(rt_, rtAcc_, kRtLen, true, display, out, n); }

void Ring::push(uint32_t t, const Group& g) {
    r_[head_] = {t, g};
    head_ = static_cast<uint8_t>((head_ + 1) % kLen);
    if (count_ < kLen) count_++;
}

void Ring::format(const Raw& r, char* out, size_t n) {
    snprintf(out, n, "%lu %04X %04X %04X %04X %02X", static_cast<unsigned long>(r.t), r.g.blk[0], r.g.blk[1],
             r.g.blk[2], r.g.blk[3], r.g.ble);
}

}  // namespace rds
