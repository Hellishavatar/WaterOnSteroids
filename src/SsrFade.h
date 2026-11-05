#pragma once

#include "PCH.h"

namespace SsrFade
{
	void NewFrame();
	void NoteWater(float a_distance, float a_height);
	void Apply(float a_waterHeight, float a_falloff);
}
