#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace RenderPassFinder
{
	struct Memory
	{
		void* context{ nullptr };
		bool (*read)(void* a_context, std::uintptr_t a_address, void* a_out, std::size_t a_size){ nullptr };
		bool (*functionBounds)(void* a_context, std::uintptr_t a_address, std::uintptr_t& a_begin, std::uintptr_t& a_end){ nullptr };
	};

	struct Result
	{
		std::uintptr_t accumulatorPass{ 0 };
		std::uintptr_t passRangeDraw{ 0 };
		std::uintptr_t perFrameUpdate{ 0 };
	};

	namespace detail
	{
		constexpr std::size_t kMaxFunctionBytes = 0x4000;
		constexpr std::size_t kCallAfterArgs = 24;
		constexpr std::size_t kArgsBeforeCall = 16;

		struct Function
		{
			std::uintptr_t            begin{ 0 };
			std::vector<std::uint8_t> bytes;
		};

		inline bool IsFunctionStart(const Memory& a_memory, std::uintptr_t a_address)
		{
			std::uintptr_t begin = 0;
			std::uintptr_t end = 0;
			return a_memory.functionBounds(a_memory.context, a_address, begin, end) && begin == a_address;
		}

		inline std::optional<Function> ReadFunction(const Memory& a_memory, std::uintptr_t a_start)
		{
			std::uintptr_t begin = 0;
			std::uintptr_t end = 0;
			if (!a_memory.functionBounds(a_memory.context, a_start, begin, end) || begin != a_start || end <= begin) {
				return std::nullopt;
			}
			Function function{ begin, std::vector<std::uint8_t>((std::min)(static_cast<std::size_t>(end - begin), kMaxFunctionBytes)) };
			if (!a_memory.read(a_memory.context, begin, function.bytes.data(), function.bytes.size())) {
				return std::nullopt;
			}
			return function;
		}

		inline bool Matches(const Function& a_function, std::size_t a_at, std::initializer_list<std::uint8_t> a_pattern)
		{
			return a_at + a_pattern.size() <= a_function.bytes.size() &&
			       std::equal(a_pattern.begin(), a_pattern.end(), a_function.bytes.begin() + static_cast<std::ptrdiff_t>(a_at));
		}

		inline std::optional<std::uintptr_t> BranchTarget(const Memory& a_memory, const Function& a_function, std::size_t a_at, std::uint8_t a_opcode)
		{
			if (a_at + 5 > a_function.bytes.size() || a_function.bytes[a_at] != a_opcode) {
				return std::nullopt;
			}
			std::int32_t rel = 0;
			std::memcpy(&rel, &a_function.bytes[a_at + 1], sizeof(rel));
			const std::uintptr_t target = a_function.begin + a_at + 5 + static_cast<std::intptr_t>(rel);
			if (!IsFunctionStart(a_memory, target)) {
				return std::nullopt;
			}
			return target;
		}

		inline std::optional<std::uintptr_t> FirstCall(const Memory& a_memory, const Function& a_function, std::size_t a_from, std::size_t a_window)
		{
			const std::size_t end = (std::min)(a_from + a_window, a_function.bytes.size());
			for (std::size_t at = a_from; at < end; ++at) {
				if (const auto target = BranchTarget(a_memory, a_function, at, 0xE8)) {
					return target;
				}
			}
			return std::nullopt;
		}

		inline std::set<std::uintptr_t> CallsAfter(const Memory& a_memory, const Function& a_function, std::initializer_list<std::uint8_t> a_pattern)
		{
			std::set<std::uintptr_t> targets;
			for (std::size_t at = 0; at < a_function.bytes.size(); ++at) {
				if (Matches(a_function, at, a_pattern)) {
					if (const auto target = FirstCall(a_memory, a_function, at + a_pattern.size(), kCallAfterArgs)) {
						targets.insert(*target);
					}
				}
			}
			return targets;
		}

		inline std::set<std::uintptr_t> TailJumpTargets(const Memory& a_memory, std::uintptr_t a_start)
		{
			std::set<std::uintptr_t> reached;
			std::vector<std::uintptr_t> frontier{ a_start };
			for (int depth = 0; depth < 2; ++depth) {
				std::vector<std::uintptr_t> next;
				for (const auto start : frontier) {
					const auto function = ReadFunction(a_memory, start);
					if (!function) {
						continue;
					}
					for (std::size_t at = 0; at < function->bytes.size(); ++at) {
						if (const auto target = BranchTarget(a_memory, *function, at, 0xE9); target && reached.insert(*target).second) {
							next.push_back(*target);
						}
					}
				}
				frontier = std::move(next);
			}
			return reached;
		}
	}

	inline std::optional<Result> Find(const Memory& a_memory, std::uintptr_t a_postResolveDepth, std::uintptr_t a_stateCameraUpdate, const char** a_failure = nullptr)
	{
		using namespace detail;
		auto fail = [&](const char* a_reason) -> std::optional<Result> {
			if (a_failure) {
				*a_failure = a_reason;
			}
			return std::nullopt;
		};

		const auto postResolve = ReadFunction(a_memory, a_postResolveDepth);
		if (!postResolve) {
			return fail("FinishAccumulatingPostResolveDepth is not a function in the game's code");
		}
		Result result;
		if (const auto pass = FirstCall(a_memory, *postResolve, 0, 0x40)) {
			result.accumulatorPass = *pass;
		} else {
			return fail("FinishAccumulatingPostResolveDepth does not start with a call to the accumulator pass");
		}
		const auto pass = ReadFunction(a_memory, result.accumulatorPass);
		if (!pass) {
			return fail("the accumulator pass could not be read");
		}

		std::set<std::uintptr_t> waterCalls;
		for (std::size_t at = 0; at < pass->bytes.size(); ++at) {
			if (Matches(*pass, at, { 0xBA, 0x71, 0x00, 0x00, 0x5C })) {
				for (std::size_t r8 = at + 5; r8 < (std::min)(at + 5 + 12, pass->bytes.size()); ++r8) {
					if (Matches(*pass, r8, { 0x41, 0xB8, 0x71, 0x60, 0x00, 0x5C })) {
						if (const auto target = FirstCall(a_memory, *pass, r8 + 6, kCallAfterArgs)) {
							waterCalls.insert(*target);
						}
					}
				}
			}
		}
		const auto groupCalls = CallsAfter(a_memory, *pass, { 0xC7, 0x44, 0x24, 0x20, 0x0A, 0x00, 0x00, 0x00 });
		if (waterCalls.size() != 1 || groupCalls.size() != 1 || *waterCalls.begin() != *groupCalls.begin()) {
			return fail("the water pass and group 0xA calls do not name one pass-range draw");
		}
		result.passRangeDraw = *waterCalls.begin();

		std::map<std::uintptr_t, std::set<std::uintptr_t>> stateArgs;
		std::map<std::uintptr_t, int>                      callCount;
		for (std::size_t at = kArgsBeforeCall; at < pass->bytes.size(); ++at) {
			const auto target = BranchTarget(a_memory, *pass, at, 0xE8);
			if (!target || *target == result.passRangeDraw) {
				continue;
			}
			std::optional<std::uintptr_t> state;
			bool                          flagsInEdx = false;
			for (std::size_t back = at - kArgsBeforeCall; back < at; ++back) {
				if (back + 7 <= at && Matches(*pass, back, { 0x48, 0x8D, 0x0D })) {
					std::int32_t disp = 0;
					std::memcpy(&disp, &pass->bytes[back + 3], sizeof(disp));
					state = pass->begin + back + 7 + static_cast<std::intptr_t>(disp);
				}
				const std::uint8_t modrm = back + 1 < at ? pass->bytes[back + 1] : 0;
				if (pass->bytes[back] == 0x8B && (modrm & 0xF8) == 0xD0) {
					flagsInEdx = true;
				}
			}
			if (state && flagsInEdx) {
				stateArgs[*target].insert(*state);
				++callCount[*target];
			}
		}
		const auto reachedFromCamera = TailJumpTargets(a_memory, a_stateCameraUpdate);
		std::vector<std::uintptr_t> perFrame;
		for (const auto& [target, count] : callCount) {
			if (count >= 2 && stateArgs[target].size() == 1 && reachedFromCamera.contains(target)) {
				perFrame.push_back(target);
			}
		}
		if (perFrame.size() != 1) {
			return fail("no single per-frame update is called by the accumulator pass and reached from SetCameraData");
		}
		result.perFrameUpdate = perFrame.front();

		if (result.perFrameUpdate == result.accumulatorPass || result.passRangeDraw == result.accumulatorPass) {
			return fail("the functions found are not distinct");
		}
		return result;
	}
}
