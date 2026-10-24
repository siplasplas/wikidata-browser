#pragma once
// The search key of a name: lower case (Unicode case folding), without
// diacritics (NFD, combining marks removed), letters that do not decompose
// mapped by hand (ł -> l, ø -> o, đ -> d, ß -> ss by folding). Names are sorted
// by it (wdnames) and searched by it (wdbrowse), so both must use this one
// function. UTF-8 in and out; ICU.
#include <string>
#include <string_view>

#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>

inline std::string foldKey(std::string_view name) {
    static thread_local const icu::Normalizer2 *nfd = [] {
        UErrorCode status = U_ZERO_ERROR;
        return icu::Normalizer2::getNFDInstance(status);
    }();
    UErrorCode status = U_ZERO_ERROR;
    auto text = icu::UnicodeString::fromUTF8(icu::StringPiece(name.data(), static_cast<int32_t>(name.size())));
    text.foldCase();
    const auto decomposed = nfd->normalize(text, status);
    if (U_FAILURE(status))
        return std::string(name);
    icu::UnicodeString out;
    for (int32_t i = 0; i < decomposed.length();) {
        const UChar32 c = decomposed.char32At(i);
        i += U16_LENGTH(c);
        if (u_charType(c) == U_NON_SPACING_MARK)
            continue;
        switch (c) {
        case 0x0142: out.append(UChar32('l')); break; // ł
        case 0x00F8: out.append(UChar32('o')); break; // ø
        case 0x0111: out.append(UChar32('d')); break; // đ
        case 0x0127: out.append(UChar32('h')); break; // ħ
        case 0x0131: out.append(UChar32('i')); break; // ı
        case 0x00E6: out.append(icu::UnicodeString(u"ae")); break; // æ
        case 0x0153: out.append(icu::UnicodeString(u"oe")); break; // œ
        case 0x00FE: out.append(icu::UnicodeString(u"th")); break; // þ
        default: out.append(c);
        }
    }
    std::string result;
    out.toUTF8String(result);
    return result;
}
