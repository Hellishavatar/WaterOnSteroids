#pragma once

#include "PCH.h"

#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShaderMaterial.h>
#include <RE/B/BSWaterShaderMaterial.h>

namespace WaterWatch
{
	[[nodiscard]] bool IsEnabled() noexcept;
	void SetEnabled(bool a_enabled);

	void OnFrame();

	void Mark(std::string_view a_label);
	void MarkAfterFrames(std::string_view a_label, std::uint32_t a_frames);

	void NoteSetupMaterial(const RE::BSShaderMaterial* a_material);
	void NoteDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_isLod, bool a_rebuiltMaterial);

	class Scope
	{
	public:
		explicit Scope(const char* a_name);
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		const char* name{ nullptr };
		bool        active{ false };
	};
}
