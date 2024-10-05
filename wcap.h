#pragma once

#ifndef UNICODE
#	define UNICODE
#endif

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

// ReSharper disable CppClangTidyClangDiagnosticReservedMacroIdentifier
#define _CRT_SECURE_NO_DEPRECATE

// Do not define these or you'll get snprintf errors
// #define _NO_CRT_STDIO_INLINE
// #define _CRT_SECURE_NO_WARNINGS
// #define _CRT_NONSTDC_NO_DEPRECATE

#pragma warning(push)
#pragma warning(disable: 4255)
#pragma warning(disable: 4456)
#pragma warning(disable: 4668)
#pragma warning(disable: 4459)
#pragma warning(disable: 4777)
#pragma warning(disable: 4459)
#pragma warning(disable: 4820)
#include <initguid.h>
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <intrin.h>
#pragma warning(pop)

#define WCAP_TITLE L"wcap"
#define WCAP_URL   L"https://github.com/mmozeiko/wcap"

#define STR(x) #x
#define WCAP_GENERATE_TITLE(version_info) "wcap, " __DATE__ " [" version_info "]"
#if defined(WCAP_GIT_INFO)
#	define WCAP_CONFIG_TITLE WCAP_GENERATE_TITLE(STR(WCAP_GIT_INFO))
#else
#	define WCAP_CONFIG_TITLE WCAP_GENERATE_TITLE("")
#endif

#ifdef _DEBUG
#define Assert(Cond) do { if (!(Cond)) __debugbreak(); } while (0)
#else
#define Assert(Cond) (void)(Cond)
#endif
#define HR(hr) do { HRESULT _hr = (hr); Assert(SUCCEEDED(_hr)); } while (0)

// calculates ceil(X * Num / Den)
#define MUL_DIV_ROUND_UP(X, Num, Den) (((X) * (Num) - 1) / (Den) + 1)

// calculates ceil(X / Y)
#define DIV_ROUND_UP(X, Y) ( ((X) + (Y) - 1) / (Y) )

// MF works with 100nsec units
#define MF_UNITS_PER_SECOND 10000000ULL

#include <stdio.h>
#define StrFormat(Buffer, ...) _snwprintf(Buffer, _countof(Buffer), __VA_ARGS__)

// These are all warnings to disable globally
#pragma warning(disable: 4211)
#pragma warning(disable: 4255)
#pragma warning(disable: 4668)
#pragma warning(disable: 4777)
#pragma warning(disable: 4820)
#pragma warning(disable: 4456)
#pragma warning(disable: 4710)
#pragma warning(disable: 4711)
#pragma warning(disable: 5045)
#pragma warning(disable: 4057)
#pragma warning(disable: 4100)
#pragma warning(disable: 4189)
#pragma warning(disable: 4242)
#pragma warning(disable: 4244)
#pragma warning(disable: 4389)
#pragma warning(disable: 4514)
#pragma warning(disable: 4245)
#pragma warning(disable: 4127)
