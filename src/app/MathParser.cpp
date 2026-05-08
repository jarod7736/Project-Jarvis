#include "MathParser.h"

#include <string.h>

namespace jarvis::app {

namespace {

// Words → numeric value lookup. Order matters only inasmuch as the
// matching loop is greedy on length within categories — but every
// word here is unambiguous as a token, so any order works.
struct WordNum {
    const char* word;
    int64_t     value;
};

constexpr WordNum kDigits[] = {
    {"zero", 0}, {"oh", 0},
    {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5},
    {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9},
    {"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13},
    {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16},
    {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19},
};
constexpr size_t kDigitsCount = sizeof(kDigits) / sizeof(kDigits[0]);

constexpr WordNum kTens[] = {
    {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50},
    {"sixty", 60}, {"seventy", 70}, {"eighty", 80}, {"ninety", 90},
};
constexpr size_t kTensCount = sizeof(kTens) / sizeof(kTens[0]);

// Operator words. Multi-word phrases ("multiplied by", "divided by")
// must be checked before their shorter prefixes to avoid mismatches.
struct OpWord {
    const char* word;     // surrounded by spaces in the matcher
    char        op;
};
constexpr OpWord kOps[] = {
    {" multiplied by ", '*'},
    {" divided by ",    '/'},
    {" times ",         '*'},
    {" plus ",          '+'},
    {" minus ",         '-'},
    {" over ",          '/'},
};
constexpr size_t kOpsCount = sizeof(kOps) / sizeof(kOps[0]);

constexpr const char* kPrefixes[] = {
    "what is ",
    "what's ",
    "whats ",
    "calculate ",
    "compute ",
    "tell me ",
    "what does ",
};
constexpr size_t kPrefixesCount = sizeof(kPrefixes) / sizeof(kPrefixes[0]);

// True if `s` starts with `pfx` at position `pos` AND the next
// character (if any) is whitespace, hyphen, or end-of-string. Used to
// avoid matching "ten" against "tennessee" or "one" against "oneself".
bool isWordBoundary(const String& s, int pos) {
    if (pos >= (int)s.length()) return true;
    char c = s.charAt(pos);
    return c == ' ' || c == '-' || c == '\0' || c == '?' || c == '.' ||
           c == ',' || c == ';' || c == ':' || c == '!';
}

bool startsWith(const String& s, int pos, const char* word) {
    int wlen = (int)strlen(word);
    if (pos + wlen > (int)s.length()) return false;
    for (int i = 0; i < wlen; ++i) {
        if (s.charAt(pos + i) != word[i]) return false;
    }
    return isWordBoundary(s, pos + wlen);
}

// Skip spaces, hyphens, and the connector word "and" (e.g. "one
// hundred and fifty"). Returns updated position.
int skipFiller(const String& s, int pos) {
    while (pos < (int)s.length()) {
        char c = s.charAt(pos);
        if (c == ' ' || c == '-' || c == ',') { ++pos; continue; }
        if (startsWith(s, pos, "and")) { pos += 3; continue; }
        break;
    }
    return pos;
}

// Parse a sequence of number-words starting at `pos` in `s`.
// Advances `pos` past every word consumed. Returns true on success
// with `out` set; false if no number word was present.
//
// Handles compound forms:
//   "twenty five"          -> 25
//   "twenty-five"          -> 25
//   "one hundred"          -> 100
//   "one hundred fifty"    -> 150
//   "one hundred and fifty"-> 150
//   "two thousand three"   -> 2003
//   "two thousand three hundred" -> 2300
bool parseNumber(const String& s, int& pos, int64_t& out) {
    int64_t total   = 0;
    int64_t current = 0;
    bool    found   = false;

    while (pos < (int)s.length()) {
        pos = skipFiller(s, pos);
        if (pos >= (int)s.length()) break;

        // "hundred" — multiply current chunk by 100 (defaulting to 1
        // if no leading digit, so "hundred" alone == 100).
        if (startsWith(s, pos, "hundred")) {
            current = (current == 0 ? 1 : current) * 100;
            pos += 7;
            found = true;
            continue;
        }
        // "thousand" — promote current chunk into total, reset chunk.
        if (startsWith(s, pos, "thousand")) {
            total += (current == 0 ? 1 : current) * 1000;
            current = 0;
            pos += 8;
            found = true;
            continue;
        }
        // Tens (greedy: try before digits to catch "twenty-five" path).
        bool matched = false;
        for (size_t i = 0; i < kTensCount; ++i) {
            if (startsWith(s, pos, kTens[i].word)) {
                current += kTens[i].value;
                pos += (int)strlen(kTens[i].word);
                found = true;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        // Digits (incl. teens).
        for (size_t i = 0; i < kDigitsCount; ++i) {
            if (startsWith(s, pos, kDigits[i].word)) {
                current += kDigits[i].value;
                pos += (int)strlen(kDigits[i].word);
                found = true;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        // No more number words — stop. Caller will see whatever's
        // left in `s` past `pos`.
        break;
    }

    if (!found) return false;
    out = total + current;
    return true;
}

// Strip any leading "what is" / "what's" / etc. prefix from `s` so
// parseNumber sees the bare number. Lowercased input expected.
String stripQueryPrefix(const String& s) {
    String t = s;
    t.trim();
    for (size_t i = 0; i < kPrefixesCount; ++i) {
        if (t.startsWith(kPrefixes[i])) {
            t = t.substring(strlen(kPrefixes[i]));
            t.trim();
            break;
        }
    }
    return t;
}

// Strip trailing punctuation that ASR sometimes attaches.
String stripTrailingPunct(const String& s) {
    String t = s;
    while (t.length() > 0) {
        char c = t.charAt(t.length() - 1);
        if (c == '?' || c == '.' || c == '!' || c == ',' || c == ' ') {
            t = t.substring(0, t.length() - 1);
            continue;
        }
        break;
    }
    return t;
}

// Find the first operator word (longest-match priority via the order
// of kOps). Returns true with `opStart` = first char of the operator
// phrase, `opLen` = length of the phrase (including its surrounding
// spaces), and `op` = the resulting operator character.
bool findOperator(const String& s, int& opStart, int& opLen, char& op) {
    int best = -1;
    for (size_t i = 0; i < kOpsCount; ++i) {
        int idx = s.indexOf(kOps[i].word);
        if (idx < 0) continue;
        if (best < 0 || idx < best) {
            best    = idx;
            opStart = idx;
            opLen   = (int)strlen(kOps[i].word);
            op      = kOps[i].op;
        }
    }
    return best >= 0;
}

}  // namespace

bool looksLikeMath(const String& lc) {
    for (size_t i = 0; i < kOpsCount; ++i) {
        if (lc.indexOf(kOps[i].word) >= 0) return true;
    }
    return false;
}

MathResult parseAndEvaluate(const String& transcript) {
    MathResult r{false, 0, String()};

    String lc = transcript;
    lc.toLowerCase();
    lc = stripQueryPrefix(lc);
    lc = stripTrailingPunct(lc);

    int  opStart = -1;
    int  opLen   = 0;
    char op      = '\0';
    if (!findOperator(lc, opStart, opLen, op)) {
        // No operator → not math-shaped. Caller (IntentRouter) will
        // fall through to its next branch.
        return r;
    }

    String left  = lc.substring(0, opStart);
    String right = lc.substring(opStart + opLen);
    left.trim();
    right.trim();

    int64_t a = 0, b = 0;
    int     pos = 0;
    if (!parseNumber(left, pos, a)) {
        r.error = String("I didn't catch the first number.");
        return r;
    }
    pos = 0;
    if (!parseNumber(right, pos, b)) {
        r.error = String("I didn't catch the second number.");
        return r;
    }

    int64_t result = 0;
    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/':
            if (b == 0) {
                r.error = String("I can't divide by zero.");
                return r;
            }
            result = a / b;
            break;
        default:
            r.error = String("I didn't catch that math problem.");
            return r;
    }

    r.ok    = true;
    r.value = result;
    return r;
}

}  // namespace jarvis::app
