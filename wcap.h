#pragma once

#define UNICODE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

// ReSharper disable CppClangTidyClangDiagnosticReservedMacroIdentifier
#define _CRT_SECURE_NO_DEPRECATE

// push warning
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
#include <intrin.h>
#pragma warning(pop)

#define WCAP_TITLE L"wcap"
#define WCAP_URL   L"https://github.com/mmozeiko/wcap"

#ifdef _DEBUG
#define Assert(Cond) do { if (!(Cond)) __debugbreak(); } while (0)
#else
#define Assert(Cond) (void)(Cond)
#endif
#define HR(hr) do { HRESULT _hr = (hr); Assert(SUCCEEDED(_hr)); } while (0)

// calculates ceil(X * Num / Den)
#define MUL_DIV_ROUND_UP(X, Num, Den) (((X) * (Num) - 1) / (Den) + 1)

// MF works with 100nsec units
#define MF_UNITS_PER_SECOND 10000000ULL

#include <stdio.h>
#define StrFormat(Buffer, ...) _snwprintf(Buffer, _countof(Buffer), __VA_ARGS__)

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
