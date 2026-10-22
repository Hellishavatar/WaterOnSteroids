#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include <detours/detours.h>

#include "EditorID.h"

using namespace std::literals;
namespace logger = SKSE::log;

namespace Detour
{
	inline bool Attach(void** a_target, void* a_replacement)
	{
		if (!a_target || !*a_target || DetourTransactionBegin() != NO_ERROR) {
			return false;
		}
		if (DetourUpdateThread(::GetCurrentThread()) == NO_ERROR && DetourAttach(a_target, a_replacement) == NO_ERROR) {
			return DetourTransactionCommit() == NO_ERROR;
		}
		DetourTransactionAbort();
		return false;
	}

	template <class Hook>
	bool Function(std::uintptr_t a_address)
	{
		Hook::func = a_address;
		return Attach(reinterpret_cast<void**>(&Hook::func), reinterpret_cast<void*>(&Hook::thunk));
	}

	template <class Hook>
	bool Virtual(const void* a_object, std::size_t a_slot)
	{
		const auto* table = a_object ? *static_cast<const std::uintptr_t* const*>(a_object) : nullptr;
		return table && Function<Hook>(table[a_slot]);
	}
}
