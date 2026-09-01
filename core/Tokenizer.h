#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <string>
#include <string_view>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Tokenizer - text to search terms.
//
// This exists as its own unit for one reason: the write path and the query path MUST tokenize
// identically. The moment indexing and searching disagree about what a term is, terms silently
// stop matching and the only symptom is queries that return nothing for text you can see with your
// own eyes. Everything that touches terms calls TOK::Tokenize and nothing else.
//
// Rules, and the reasoning behind each:
//
//   - ASCII case-folded. Anything >= 0x80 is treated as an ordinary word byte and passed through
//     untouched, so UTF-8 words survive intact as opaque byte sequences and match themselves.
//     They will not case-fold. That is a deliberate limit, not an oversight - real Unicode folding
//     needs a table nobody here wants to vendor, and the corpus is overwhelmingly ASCII.
//
//   - Digits stay attached to letters, so "50ft" is one term rather than "15" and "mcg". This
//     matters on the actual corpus: doses, versions and sizes are the distinctive parts of a jot.
//
//   - Apostrophes inside a word are kept ("user's"), but a leading or trailing one is dropped.
//
//   - Underscore and hyphen SPLIT. Tags normalize to hyphenated form, and slugs like
//     "user-preferences" should be findable by searching "preferences".
//
// Stopwords are removed from the BODY only, never from the summary and never from a query whose
// every term is a stopword - dropping every term turns a specific query into a match-everything,
// which is worse than a slow one.
//////////////////////////////////////////////////////////////////////////////////////////////////

namespace TOK
{
    constexpr size_t kMaxTermLen = 64;   // longer runs are truncated, not dropped

    // Appends terms to outTerms (does not clear it, so several fields can accumulate).
    // Returns the number of terms appended.
    size_t Tokenize(std::string_view sText, std::vector<std::string>& outTerms);

    // Same split, but keeps stopwords. Used for the summary field and for queries.
    size_t TokenizeKeepStopwords(std::string_view sText, std::vector<std::string>& outTerms);

    bool IsStopword(std::string_view sTerm);

    // ASCII lowercase + trim. Used for tag and slug normalization, not for indexing.
    std::string Fold(std::string_view sText);
}
