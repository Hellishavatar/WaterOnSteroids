#include "TypeMaterial.h"

#include <RE/B/BSWaterShaderMaterial.h>
#include <RE/N/NiSourceTexture.h>
#include <RE/T/TESWaterForm.h>
#include <RE/T/TESWaterObject.h>
#include <RE/T/TESWaterSystem.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace
{
	struct WaterNormals
	{
		std::uint8_t                   pad00[0x18];
		RE::TESWaterForm*              form{ nullptr };
		std::uint64_t                  pad20{ 0 };
		RE::BSWaterShaderMaterial*     material{ nullptr };
		RE::NiPointer<RE::NiSourceTexture> textures[4];
	};
	static_assert(offsetof(WaterNormals, form) == 0x18);
	static_assert(offsetof(WaterNormals, material) == 0x28);
	static_assert(offsetof(WaterNormals, textures) == 0x30);

	constexpr std::size_t kTextureReloadFlag = 0x23C;
	constexpr auto        kUpdateEvery = std::chrono::milliseconds(5);

	struct Entry
	{
		WaterNormals                          normals{};
		std::chrono::steady_clock::time_point updated{};
	};

	std::mutex                                                      entriesLock;
	std::unordered_map<RE::TESWaterForm*, std::unique_ptr<Entry>> entries;
	std::atomic<std::uint32_t>                                      built{ 0 };

	bool ReloadPending(const RE::TESWaterForm* a_type)
	{
		return *(reinterpret_cast<const std::uint8_t*>(a_type) + kTextureReloadFlag) != 0;
	}

	bool TypeLoaded(const RE::TESWaterForm* a_type)
	{
		auto* system = RE::TESWaterSystem::GetSingleton();
		if (!system) {
			return false;
		}
		RE::BSSpinLockGuard guard(system->lock);
		for (const auto& water : system->waterObjects) {
			if (water && water->waterType == a_type) {
				return true;
			}
		}
		return false;
	}

	bool MayRunUpdate(const RE::TESWaterForm* a_type)
	{
		return !ReloadPending(a_type) || !TypeLoaded(a_type);
	}

	void Update(Entry& a_entry, std::chrono::steady_clock::time_point a_now)
	{
		using Update_t = void (*)(WaterNormals*);
		static REL::Relocation<Update_t> update{ RELOCATION_ID(31380, 32170) };
		a_entry.updated = a_now;
		update(&a_entry.normals);
	}

	std::unique_ptr<Entry> Build(RE::TESWaterForm* a_type)
	{
		using Construct_t = RE::BSWaterShaderMaterial* (*)(RE::BSWaterShaderMaterial*);
		using Fill_t = void (*)(RE::TESWaterForm*, RE::BSWaterShaderMaterial*);
		static REL::Relocation<Construct_t> construct{ RELOCATION_ID(100783, 107567) };
		static REL::Relocation<Fill_t>      fill{ RELOCATION_ID(19945, 20373) };

		if (!MayRunUpdate(a_type)) {
			return nullptr;
		}
		auto* memory = RE::malloc(sizeof(RE::BSWaterShaderMaterial));
		if (!memory) {
			return nullptr;
		}
		auto* material = construct(static_cast<RE::BSWaterShaderMaterial*>(memory));
		fill(a_type, material);

		auto entry = std::make_unique<Entry>();
		entry->normals.form = a_type;
		entry->normals.material = material;
		*(reinterpret_cast<std::uint8_t*>(a_type) + kTextureReloadFlag) = 1;
		Update(*entry, std::chrono::steady_clock::now());

		if (built++ < 20) {
			logger::info("Water type material: built {:08X} '{}' from its record (wave textures {}, {}, {}).",
				a_type->GetFormID(), a_type->GetFormEditorID(),
				material->normalTexture1 ? "loaded" : "missing", material->normalTexture2 ? "loaded" : "missing", material->normalTexture3 ? "loaded" : "missing");
		}
		return entry;
	}
}

RE::BSWaterShaderMaterial* TypeMaterial::For(RE::TESWaterForm* a_type)
{
	if (!a_type) {
		return nullptr;
	}
	const auto       now = std::chrono::steady_clock::now();
	std::scoped_lock lock(entriesLock);
	const auto       it = entries.find(a_type);
	if (it == entries.end()) {
		auto entry = Build(a_type);
		if (!entry) {
			return nullptr;
		}
		return entries.emplace(a_type, std::move(entry)).first->second->normals.material;
	}
	if (now - it->second->updated >= kUpdateEvery && MayRunUpdate(a_type)) {
		Update(*it->second, now);
	}
	return it->second->normals.material;
}
