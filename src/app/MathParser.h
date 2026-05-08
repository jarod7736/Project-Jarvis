#pragma once

// MathParser — turns spoken arithmetic into a numeric result.
//
// ASR returns spoken numbers as words ("twelve plus seventeen"), so we
// can't just hand them to a normal expression evaluator. This module
// takes a lowercased transcript like "what is twelve plus seventeen"
// and produces an int64_t answer if it looks like a single binary
// arithmetic problem.
//
// Scope (v1, intentionally narrow):
//   - One operator per query: a + b, a - b, a * b, a / b
//   - Operators by word: plus / minus / times / multiplied by /
//     divided by / over
//   - Numbers up to a few thousand, expressed as English words with
//     optional "and" connectors and optional hyphens between tens and
//     ones ("twenty-five" or "twenty five")
//   - Integer division (no fractional results)
//   - Optional leading "what is" / "what's" / "calculate" / etc.
//
// Out of scope: chained ops, parens, fractions, exponents, square
// roots. The IntentRouter routes those phrasings to local_llm /
// claude where a real model can answer them.

#include <Arduino.h>
#include <stdint.h>

namespace jarvis::app {

struct MathResult {
    bool    ok;
    int64_t value;
    // Failure reason when !ok. Empty for ok results. The router uses
    // this to compose a user-facing error string ("I can't divide by
    // zero" etc.).
    String  error;
};

// Parse `transcript` (lowercased, trimmed-ish — internal handling is
// permissive about whitespace and punctuation) and return the result
// of the implied binary operation. ok=false means the input didn't
// match the expected shape; the caller should fall through to its
// next intent option.
MathResult parseAndEvaluate(const String& transcript);

// Rough predicate used by the IntentRouter's keyword classifier to
// decide whether a "what is X" phrase is math-shaped before it gets
// routed to the LLM path. True iff the input contains an operator
// word (plus / minus / times / divided by / over / multiplied by).
bool looksLikeMath(const String& lowercaseTranscript);

}  // namespace jarvis::app
