#include "Tokenizer.h"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace TOK
{
    namespace
    {
        // Small and deliberately conservative. Every word here is one that carries no selectivity
        // in a corpus of short personal notes and technical memories. Words that look like
        // stopwords but are not: "no", "not", "off", "on", "up", "down" - all of them change the
        // meaning of a jot ("no second fix", "hold the experiment"), so they stay indexed.
        const std::unordered_set<std::string_view>& Stopwords()
        {
            static const std::unordered_set<std::string_view> kSet = {
                "a", "an", "and", "are", "as", "at", "be", "been", "but", "by", "for", "from",
                "had", "has", "have", "he", "her", "his", "i", "if", "in", "into", "is", "it",
                "its", "of", "or", "she", "so", "than", "that", "the", "their", "them", "then",
                "there", "these", "they", "this", "to", "was", "were", "will", "with", "you"
            };
            return kSet;
        }

        inline bool IsWordByte(unsigned char c)
        {
            // High bytes are UTF-8 continuation or lead bytes - treated as word content so
            // multibyte words are not shredded into fragments.
            return (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || (c >= 0x80)
                || c == '\'';
        }

        inline char FoldByte(unsigned char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        }

        size_t TokenizeImpl(std::string_view sText, std::vector<std::string>& outTerms, bool bDropStopwords)
        {
            size_t nAppended = 0;
            const size_t nLen = sText.size();
            size_t i = 0;

            while (i < nLen)
            {
                while (i < nLen && !IsWordByte(static_cast<unsigned char>(sText[i])))
                    ++i;

                const size_t nStart = i;
                while (i < nLen && IsWordByte(static_cast<unsigned char>(sText[i])))
                    ++i;

                if (i == nStart)
                    continue;

                std::string_view sRaw = sText.substr(nStart, i - nStart);

                // A leading or trailing apostrophe is punctuation, not part of the word. Quoted
                // text ('like this') would otherwise produce terms that never match the same word
                // written bare.
                while (!sRaw.empty() && sRaw.front() == '\'')
                    sRaw.remove_prefix(1);
                while (!sRaw.empty() && sRaw.back() == '\'')
                    sRaw.remove_suffix(1);

                if (sRaw.empty())
                    continue;

                if (sRaw.size() > kMaxTermLen)
                    sRaw = sRaw.substr(0, kMaxTermLen);

                std::string sTerm;
                sTerm.reserve(sRaw.size());
                for (char c : sRaw)
                    sTerm.push_back(FoldByte(static_cast<unsigned char>(c)));

                if (bDropStopwords && Stopwords().count(sTerm) != 0)
                    continue;

                outTerms.emplace_back(std::move(sTerm));
                ++nAppended;
            }

            return nAppended;
        }
    }

    size_t Tokenize(std::string_view sText, std::vector<std::string>& outTerms)
    {
        return TokenizeImpl(sText, outTerms, true);
    }

    size_t TokenizeKeepStopwords(std::string_view sText, std::vector<std::string>& outTerms)
    {
        return TokenizeImpl(sText, outTerms, false);
    }

    bool IsStopword(std::string_view sTerm)
    {
        return Stopwords().count(sTerm) != 0;
    }

    std::string Fold(std::string_view sText)
    {
        size_t nBegin = 0;
        size_t nEnd   = sText.size();
        while (nBegin < nEnd && static_cast<unsigned char>(sText[nBegin]) <= ' ')
            ++nBegin;
        while (nEnd > nBegin && static_cast<unsigned char>(sText[nEnd - 1]) <= ' ')
            --nEnd;

        std::string sOut;
        sOut.reserve(nEnd - nBegin);
        for (size_t i = nBegin; i < nEnd; ++i)
            sOut.push_back(FoldByte(static_cast<unsigned char>(sText[i])));
        return sOut;
    }
}
