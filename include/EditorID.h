#pragma once

#include <string>

namespace Util
{
	inline std::string GetEditorID(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		if (const char* edid = a_form->GetFormEditorID(); edid && edid[0] != '\0') {
			return edid;
		}
		return {};
	}
}
