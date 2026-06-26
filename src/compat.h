#pragma once
/*
 * Compatibility shims for MSVC vs MinGW-w64 differences.
 * Include this ONCE in main.cpp (or any TU that needs these functions).
 */

#include <wchar.h>
#include <stdio.h>
#include <string.h>

// ── wcsicmp ─────────────────────────────────────────────────────────────────
// MSVC spells it _wcsicmp; MinGW provides both names, but _wcsicmp is safer.
#ifndef _wcsicmp
#ifdef _MSC_VER
    // already defined in <wchar.h> as _wcsicmp — nothing to do
#else
    #define _wcsicmp wcsicmp
#endif
#endif

// ── ReadWideLine ─────────────────────────────────────────────────────────────
// Portable replacement for _getws_s (MSVC-only) and getws (deprecated/removed).
// Reads a wide-character line from stdin, strips the trailing newline.
// Returns buf on success, nullptr on EOF.
static inline wchar_t* ReadWideLine(wchar_t* buf, int maxChars)
{
    if (!fgetws(buf, maxChars, stdin))
        return nullptr;
    // Strip trailing \n (and \r\n on Windows text mode)
    int len = (int)wcslen(buf);
    while (len > 0 && (buf[len-1] == L'\n' || buf[len-1] == L'\r'))
        buf[--len] = L'\0';
    return buf;
}

// ── ScanWideLine ─────────────────────────────────────────────────────────────
// swscanf_s (MSVC) requires explicit buffer-size arguments for %s.
// swscanf  (MinGW / C99) does not. Wrap the two-string variant we need.
static inline int ScanWideTwo(const wchar_t* src,
                               wchar_t* a, int aLen,
                               wchar_t* b, int bLen)
{
#ifdef _MSC_VER
    return swscanf_s(src, L"%*[ ]%[^ ]%*[ ]%[^\n]", a, aLen, b, bLen);
#else
    (void)aLen; (void)bLen;
    return swscanf(src, L"%*[ ]%[^ ]%*[ ]%[^\n]", a, b);
#endif
}
