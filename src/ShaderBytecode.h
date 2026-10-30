#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace ShaderBytecode
{
	namespace detail
	{
		constexpr std::uint32_t kOpMad = 0x32;
		constexpr std::uint32_t kOpMul = 0x38;
		constexpr std::uint32_t kOpCustomData = 0x35;
		constexpr std::uint32_t kOperandTemp = 0;
		constexpr std::uint32_t kOperandImmediate32 = 4;
		constexpr std::uint32_t kOperandConstantBuffer = 8;
		constexpr std::uint32_t kImmediateOne = 0x3F800000;
		constexpr std::uint32_t kScalarImmediateToken = 0x00004001;

		struct Operand
		{
			std::size_t   pos{ 0 };
			std::size_t   end{ 0 };
			std::uint32_t token{ 0 };
			std::uint32_t type{ 0 };
			bool          extended{ false };
			std::uint32_t index[2]{};
			std::uint32_t dims{ 0 };
			std::uint32_t immediate{ 0 };

			[[nodiscard]] std::uint32_t Components() const { return token & 3; }
			[[nodiscard]] std::uint32_t Selection() const { return (token >> 2) & 3; }

			[[nodiscard]] std::uint32_t ReadMask() const
			{
				if (Components() != 2) {
					return 0;
				}
				if (Selection() == 2) {
					return 1u << ((token >> 4) & 3);
				}
				if (Selection() == 1) {
					std::uint32_t mask = 0;
					for (int i = 0; i < 4; ++i) {
						mask |= 1u << ((token >> (4 + 2 * i)) & 3);
					}
					return mask;
				}
				return 0;
			}

			[[nodiscard]] std::uint32_t WriteMask() const { return Components() == 2 && Selection() == 0 ? (token >> 4) & 0xF : 0; }
		};

		inline bool ReadOperand(const std::uint32_t* a_tokens, std::size_t a_pos, std::size_t a_limit, Operand& a_out)
		{
			if (a_pos >= a_limit) {
				return false;
			}
			a_out = {};
			a_out.pos = a_pos;
			a_out.token = a_tokens[a_pos];
			a_out.type = (a_out.token >> 12) & 0xFF;
			a_out.dims = (a_out.token >> 20) & 3;
			a_out.extended = (a_out.token >> 31) != 0;
			std::size_t p = a_pos + 1;
			if (a_out.extended) {
				++p;
			}
			if (a_out.type == kOperandImmediate32) {
				const std::size_t count = a_out.Components() == 2 ? 4 : 1;
				if (p + count > a_limit) {
					return false;
				}
				a_out.immediate = a_tokens[p];
				p += count;
			}
			for (std::uint32_t d = 0; d < a_out.dims; ++d) {
				const std::uint32_t representation = (a_out.token >> (22 + 3 * d)) & 7;
				if (representation != 0 || p >= a_limit || d >= 2) {
					return false;
				}
				a_out.index[d] = a_tokens[p++];
			}
			a_out.end = p;
			return p <= a_limit;
		}

		struct Instruction
		{
			std::uint32_t        opcode{ 0 };
			bool                 saturate{ false };
			std::size_t          pos{ 0 };
			std::size_t          length{ 0 };
			std::vector<Operand> operands;
		};

		inline bool ReadInstructions(const std::uint32_t* a_tokens, std::size_t a_count, std::vector<Instruction>& a_out)
		{
			std::size_t pos = 2;
			while (pos < a_count) {
				const std::uint32_t token = a_tokens[pos];
				Instruction         inst;
				inst.opcode = token & 0x7FF;
				inst.saturate = (token & (1u << 13)) != 0;
				inst.pos = pos;
				inst.length = inst.opcode == kOpCustomData ? (pos + 1 < a_count ? a_tokens[pos + 1] : 0) : (token >> 24) & 0x7F;
				if (inst.length == 0 || pos + inst.length > a_count) {
					return false;
				}
				if (inst.opcode != kOpCustomData) {
					std::size_t p = pos + 1;
					if (token >> 31) {
						while (p < pos + inst.length && (a_tokens[p] >> 31)) {
							++p;
						}
						++p;
					}
					Operand operand;
					while (p < pos + inst.length && ReadOperand(a_tokens, p, pos + inst.length, operand)) {
						inst.operands.push_back(operand);
						p = operand.end;
					}
				}
				const std::size_t length = inst.length;
				a_out.push_back(std::move(inst));
				pos += length;
			}
			return true;
		}

		inline bool IsFlag(const Operand& a_operand)
		{
			return a_operand.type == kOperandConstantBuffer && a_operand.dims == 2 && a_operand.index[0] == 12 && a_operand.index[1] == 42 &&
			       a_operand.ReadMask() == 0b1000;
		}

		inline bool IsReflectionStrength(const Operand& a_operand)
		{
			return a_operand.type == kOperandConstantBuffer && a_operand.dims == 2 && a_operand.index[0] == 1 && a_operand.index[1] == 12 &&
			       a_operand.ReadMask() == 0b0001;
		}

		inline std::optional<std::vector<std::size_t>> FindSites(const std::vector<Instruction>& a_program)
		{
			for (std::size_t k = 0; k < a_program.size(); ++k) {
				const auto& weight = a_program[k];
				if (weight.opcode != kOpMad || !weight.saturate || weight.operands.size() != 4 || !IsFlag(weight.operands[3])) {
					continue;
				}
				const auto& dst = weight.operands[0];
				const auto  mask = dst.WriteMask();
				if (dst.type != kOperandTemp || dst.dims != 1 || mask == 0 || (mask & (mask - 1)) != 0) {
					return std::nullopt;
				}
				const auto           reg = dst.index[0];
				std::vector<std::size_t> sites;
				for (std::size_t i = k + 1; i < a_program.size(); ++i) {
					const auto& inst = a_program[i];
					if (inst.operands.empty()) {
						continue;
					}
					for (std::size_t s = 1; s < inst.operands.size(); ++s) {
						const auto& src = inst.operands[s];
						if (src.type != kOperandTemp || src.dims != 1 || src.index[0] != reg || (src.ReadMask() & mask) == 0) {
							continue;
						}
						if (src.ReadMask() != mask) {
							return std::nullopt;
						}
						bool reflections = false;
						if (inst.opcode == kOpMul) {
							for (const auto& other : inst.operands) {
								reflections = reflections || IsReflectionStrength(other);
							}
						}
						if (reflections) {
							continue;
						}
						if (src.extended || src.end - src.pos != 2) {
							return std::nullopt;
						}
						sites.push_back(src.pos);
					}
					const auto& out = inst.operands[0];
					if (out.type == kOperandTemp && out.dims == 1 && out.index[0] == reg && (out.WriteMask() & mask) != 0) {
						break;
					}
				}
				if (sites.empty()) {
					return std::nullopt;
				}
				return sites;
			}
			return std::nullopt;
		}

		inline void Md5Block(std::uint32_t (&a_state)[4], const std::uint8_t* a_block)
		{
			static constexpr std::uint32_t shift[64] = {
				7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
				5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
				4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
				6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
			};
			static const auto table = [] {
				std::array<std::uint32_t, 64> k{};
				for (int i = 0; i < 64; ++i) {
					k[i] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(std::abs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
				}
				return k;
			}();
			std::uint32_t m[16];
			std::memcpy(m, a_block, sizeof(m));
			std::uint32_t a = a_state[0], b = a_state[1], c = a_state[2], d = a_state[3];
			for (std::uint32_t i = 0; i < 64; ++i) {
				std::uint32_t f, g;
				if (i < 16) {
					f = (b & c) | (~b & d);
					g = i;
				} else if (i < 32) {
					f = (d & b) | (~d & c);
					g = (5 * i + 1) % 16;
				} else if (i < 48) {
					f = b ^ c ^ d;
					g = (3 * i + 5) % 16;
				} else {
					f = c ^ (b | ~d);
					g = (7 * i) % 16;
				}
				f += a + table[i] + m[g];
				a = d;
				d = c;
				c = b;
				b += (f << shift[i]) | (f >> (32 - shift[i]));
			}
			a_state[0] += a;
			a_state[1] += b;
			a_state[2] += c;
			a_state[3] += d;
		}

		inline void WriteChecksum(std::vector<std::uint8_t>& a_container)
		{
			const std::uint8_t* data = a_container.data() + 20;
			const std::size_t   size = a_container.size() - 20;
			const std::uint32_t bits = static_cast<std::uint32_t>(size * 8);
			std::uint32_t       state[4]{ 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476 };
			const std::size_t   full = size - size % 64;
			for (std::size_t i = 0; i < full; i += 64) {
				Md5Block(state, data + i);
			}
			const std::size_t   rest = size - full;
			const std::uint32_t tail = (bits >> 2) | 1;
			std::uint8_t        block[64]{};
			if (rest < 56) {
				std::memcpy(block, &bits, 4);
				std::memcpy(block + 4, data + full, rest);
				block[4 + rest] = 0x80;
				std::memcpy(block + 60, &tail, 4);
				Md5Block(state, block);
			} else {
				std::memcpy(block, data + full, rest);
				block[rest] = 0x80;
				Md5Block(state, block);
				std::memset(block, 0, sizeof(block));
				std::memcpy(block, &bits, 4);
				std::memcpy(block + 60, &tail, 4);
				Md5Block(state, block);
			}
			std::memcpy(a_container.data() + 4, state, sizeof(state));
		}
	}

	inline std::optional<std::vector<std::uint8_t>> PatchRealisticWater(const void* a_bytecode, std::size_t a_length)
	{
		if (!a_bytecode || a_length < 32) {
			return std::nullopt;
		}
		const auto* bytes = static_cast<const std::uint8_t*>(a_bytecode);
		if (std::memcmp(bytes, "DXBC", 4) != 0) {
			return std::nullopt;
		}
		std::uint32_t total = 0;
		std::uint32_t chunks = 0;
		std::memcpy(&total, bytes + 24, 4);
		std::memcpy(&chunks, bytes + 28, 4);
		if (total != a_length || chunks > 32 || 32 + 4 * static_cast<std::size_t>(chunks) > a_length) {
			return std::nullopt;
		}
		std::size_t programOffset = 0;
		std::size_t programSize = 0;
		for (std::uint32_t i = 0; i < chunks; ++i) {
			std::uint32_t offset = 0;
			std::memcpy(&offset, bytes + 32 + 4 * i, 4);
			if (offset + 8 > a_length) {
				return std::nullopt;
			}
			std::uint32_t size = 0;
			std::memcpy(&size, bytes + offset + 4, 4);
			if ((std::memcmp(bytes + offset, "SHEX", 4) == 0 || std::memcmp(bytes + offset, "SHDR", 4) == 0) && offset + 8 + size <= a_length) {
				programOffset = offset + 8;
				programSize = size;
			}
		}
		if (programSize < 8 || programSize % 4 != 0) {
			return std::nullopt;
		}
		std::vector<std::uint32_t> tokens(programSize / 4);
		std::memcpy(tokens.data(), bytes + programOffset, programSize);
		if ((tokens[0] >> 16) != 0) {
			return std::nullopt;
		}
		std::vector<detail::Instruction> program;
		if (!detail::ReadInstructions(tokens.data(), tokens.size(), program)) {
			return std::nullopt;
		}
		const auto sites = detail::FindSites(program);
		if (!sites) {
			return std::nullopt;
		}
		std::vector<std::uint8_t> out(bytes, bytes + a_length);
		for (const auto site : *sites) {
			const std::uint32_t replacement[2]{ detail::kScalarImmediateToken, detail::kImmediateOne };
			std::memcpy(out.data() + programOffset + 4 * site, replacement, sizeof(replacement));
		}
		detail::WriteChecksum(out);
		return out;
	}
}
