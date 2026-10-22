#pragma once

namespace Menu
{
	void InstallHooks();
	bool IsOpen();
	void SetOpen(bool a_open);
	void Toggle();
	void Render();
	void ForceUnlockControls();
}
