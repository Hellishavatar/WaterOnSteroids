#include "Menu.h"
#include "PCH.h"
#include "Settings.h"
#include "WaterPatcher.h"
#include "WaterShader.h"
#include "WaterWatch.h"
#include "Version.h"
#include <RE/S/SendHUDMessage.h>
#include <cmath>

#include <d3d11.h>
#include <dxgi.h>
#include <array>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Menu
{
	namespace
	{
		void ShowHUDNotification(const char* a_message)
		{
			if (!a_message) {
				return;
			}
			std::string msg = a_message;
			SKSE::GetTaskInterface()->AddTask([msg]() {
				RE::SendHUDMessage::ShowHUDMessage(msg.c_str(), nullptr, true);
			});
		}

		std::atomic<bool> s_open{ false };
		bool s_initialized{ false };
		HWND s_hWnd{ nullptr };
		WNDPROC s_originalWndProc{ nullptr };

		ID3D11Device* s_device{ nullptr };
		ID3D11DeviceContext* s_context{ nullptr };
		ID3D11RenderTargetView* s_mainRenderTargetView{ nullptr };

		std::atomic<bool> s_toggleKeyPressed{ false };
		bool s_sampledInitialWater{ false };

		constexpr std::array s_menuLockedControls{
			RE::ControlMap::UEFlag::kLooking,
			RE::ControlMap::UEFlag::kMovement,
			RE::ControlMap::UEFlag::kFighting
		};
		std::array<bool, s_menuLockedControls.size()> s_controlsLockedByMenu{};

		void LockControls()
		{
			auto controlMap = RE::ControlMap::GetSingleton();
			if (!controlMap) {
				return;
			}
			for (std::size_t i = 0; i < s_menuLockedControls.size(); ++i) {
				s_controlsLockedByMenu[i] = controlMap->AreControlsEnabled(s_menuLockedControls[i]);
				if (s_controlsLockedByMenu[i]) {
					controlMap->ToggleControls(s_menuLockedControls[i], false, false);
				}
			}
		}

		void UnlockControls()
		{
			auto controlMap = RE::ControlMap::GetSingleton();
			for (std::size_t i = 0; i < s_menuLockedControls.size(); ++i) {
				if (s_controlsLockedByMenu[i] && controlMap) {
					controlMap->ToggleControls(s_menuLockedControls[i], true, false);
				}
				s_controlsLockedByMenu[i] = false;
			}
		}

		void SetupImGuiStyle()
		{
			ImGuiStyle& style = ImGui::GetStyle();
			style.WindowRounding = 8.0f;
			style.FrameRounding = 5.0f;
			style.PopupRounding = 6.0f;
			style.GrabRounding = 4.0f;
			style.ScrollbarRounding = 6.0f;
			style.WindowBorderSize = 0.0f;
			style.ChildBorderSize = 0.0f;
			style.PopupBorderSize = 0.0f;
			style.FrameBorderSize = 0.0f;
			style.TabBorderSize = 0.0f;
			style.ItemSpacing = ImVec2(8.0f, 4.0f);
			style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
			style.WindowPadding = ImVec2(10.0f, 8.0f);
			style.FramePadding = ImVec2(6.0f, 3.0f);
			style.CellPadding = ImVec2(4.0f, 2.0f);
			style.SeparatorTextPadding = ImVec2(14.0f, 3.0f);

			float winAlpha = Settings::GetSingleton().windowAlpha;
			float childAlpha = (std::clamp)(winAlpha * 0.70f, 0.15f, 0.90f);
			float frameAlpha = (std::clamp)(0.35f + winAlpha * 0.50f, 0.45f, 1.0f);
			float titleAlpha = (std::clamp)(winAlpha + 0.10f, 0.35f, 1.0f);

			ImVec4* colors = style.Colors;
			colors[ImGuiCol_Text] = ImVec4(0.80f, 0.80f, 0.78f, 1.00f);
			colors[ImGuiCol_TextDisabled] = ImVec4(0.46f, 0.46f, 0.45f, 1.00f);
			colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.02f, 0.02f, winAlpha);
			colors[ImGuiCol_ChildBg] = ImVec4(0.04f, 0.04f, 0.04f, childAlpha);
			colors[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.04f, 0.04f, (std::max)(0.88f, winAlpha));
			colors[ImGuiCol_Border] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.08f, 0.08f, frameAlpha);
			colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.12f, 0.12f, (std::min)(1.0f, frameAlpha + 0.20f));
			colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.16f, 0.16f, (std::min)(1.0f, frameAlpha + 0.30f));
			colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, titleAlpha);
			colors[ImGuiCol_TitleBgActive] = ImVec4(0.02f, 0.02f, 0.02f, titleAlpha);
			colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, winAlpha * 0.75f);
			colors[ImGuiCol_MenuBarBg] = ImVec4(0.03f, 0.03f, 0.03f, winAlpha);
			colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, winAlpha * 0.60f);
			colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.18f, 0.18f, 0.18f, 0.80f);
			colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.25f, 0.25f, 0.90f);
			colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.78f, 0.65f, 0.36f, 1.00f);
			colors[ImGuiCol_CheckMark] = ImVec4(0.78f, 0.65f, 0.36f, 1.00f);
			colors[ImGuiCol_SliderGrab] = ImVec4(0.76f, 0.63f, 0.34f, 1.00f);
			colors[ImGuiCol_SliderGrabActive] = ImVec4(0.86f, 0.75f, 0.50f, 1.00f);
			colors[ImGuiCol_Button] = ImVec4(0.09f, 0.09f, 0.09f, frameAlpha);
			colors[ImGuiCol_ButtonHovered] = ImVec4(0.14f, 0.14f, 0.14f, (std::min)(1.0f, frameAlpha + 0.25f));
			colors[ImGuiCol_ButtonActive] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
			colors[ImGuiCol_Header] = ImVec4(0.09f, 0.09f, 0.09f, 0.80f);
			colors[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.14f, 0.14f, 0.90f);
			colors[ImGuiCol_HeaderActive] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
			colors[ImGuiCol_Separator] = ImVec4(0.15f, 0.15f, 0.15f, 0.80f);
			colors[ImGuiCol_SeparatorHovered] = ImVec4(0.22f, 0.22f, 0.22f, 0.90f);
			colors[ImGuiCol_SeparatorActive] = ImVec4(0.78f, 0.65f, 0.36f, 1.00f);
			colors[ImGuiCol_ResizeGrip] = ImVec4(0.18f, 0.18f, 0.18f, 0.50f);
			colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.25f, 0.25f, 0.25f, 0.75f);
			colors[ImGuiCol_ResizeGripActive] = ImVec4(0.78f, 0.65f, 0.36f, 0.90f);
		}

		void OnResize()
		{
			if (s_mainRenderTargetView) {
				s_mainRenderTargetView->Release();
				s_mainRenderTargetView = nullptr;
			}
			if (s_initialized) {
				ImGui_ImplDX11_InvalidateDeviceObjects();
			}
		}

		ID3D11RenderTargetView* GetMenuRenderTarget(IDXGISwapChain* a_swapChain)
		{
			if (auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				if (auto rtv = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].RTV) {
					return reinterpret_cast<ID3D11RenderTargetView*>(rtv);
				}
			}

			if (!s_mainRenderTargetView) {
				ID3D11Texture2D* backBuffer = nullptr;
				if (SUCCEEDED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
					s_device->CreateRenderTargetView(backBuffer, nullptr, &s_mainRenderTargetView);
					backBuffer->Release();
				}
			}
			return s_mainRenderTargetView;
		}

		LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
		{
			if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
				uint32_t toggleKey = Settings::GetFastSnapshot().toggleKey;
				if (wParam == toggleKey) {
					if (!(lParam & (1 << 30))) {
						if (!s_toggleKeyPressed.exchange(true)) {
							Menu::Toggle();
						}
					}
					return 0;
				}
			} else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
				uint32_t toggleKey = Settings::GetFastSnapshot().toggleKey;
				if (wParam == toggleKey) {
					s_toggleKeyPressed.store(false);
					return 0;
				}
			}

			if (s_initialized && Menu::IsOpen()) {
				ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);

				if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && (wParam == VK_ESCAPE || wParam == VK_TAB)) {
					Menu::SetOpen(false);
					return 0;
				}

				switch (uMsg) {
				case WM_KEYDOWN:
				case WM_KEYUP:
				case WM_CHAR:
				case WM_SYSKEYDOWN:
				case WM_SYSKEYUP:
				case WM_LBUTTONDOWN:
				case WM_LBUTTONUP:
				case WM_RBUTTONDOWN:
				case WM_RBUTTONUP:
				case WM_MBUTTONDOWN:
				case WM_MBUTTONUP:
				case WM_MOUSEWHEEL:
				case WM_MOUSEHWHEEL:
					return 0;
				}
			}

			return CallWindowProcW(s_originalWndProc, hWnd, uMsg, wParam, lParam);
		}

		void RenderFrame(IDXGISwapChain* a_swapChain)
		{
			if (!s_initialized) {
				ID3D11Device* device = nullptr;
				if (SUCCEEDED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
					ID3D11DeviceContext* context = nullptr;
					device->GetImmediateContext(&context);

					DXGI_SWAP_CHAIN_DESC desc{};
					a_swapChain->GetDesc(&desc);
					s_hWnd = desc.OutputWindow;

					ImGui::CreateContext();
					ImGuiIO& io = ImGui::GetIO();
					io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
					io.IniFilename = nullptr;

					ImGui_ImplWin32_Init(s_hWnd);
					ImGui_ImplDX11_Init(device, context);

					SetupImGuiStyle();

					s_device = device;
					s_context = context;
					s_initialized = true;

					if (s_hWnd && !s_originalWndProc) {
						s_originalWndProc = reinterpret_cast<WNDPROC>(
							SetWindowLongPtrW(s_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
						if (!s_originalWndProc) {
							logger::warn("Failed to subclass the game window (error {}). The toggle key still works, but Esc/Tab and keyboard navigation inside the menu will not.", ::GetLastError());
						}
					}
					logger::info("ImGui initialized successfully with DirectX 11.");
				} else {
					return;
				}
			}

			if (!s_open.load()) {
				return;
			}

			ID3D11RenderTargetView* targetRTV = GetMenuRenderTarget(a_swapChain);
			if (!targetRTV) {
				return;
			}

			DXGI_SWAP_CHAIN_DESC desc{};
			a_swapChain->GetDesc(&desc);

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();

			ImGuiIO& io = ImGui::GetIO();
			const float displayW = static_cast<float>(desc.BufferDesc.Width);
			const float displayH = static_cast<float>(desc.BufferDesc.Height);
			io.DisplaySize = ImVec2(displayW, displayH);

			HWND targetHwnd = s_hWnd ? s_hWnd : desc.OutputWindow;
			RECT clientRect{};
			if (targetHwnd && ::GetClientRect(targetHwnd, &clientRect) && clientRect.right > 0 && clientRect.bottom > 0) {
				float scaleX = displayW / static_cast<float>(clientRect.right);
				float scaleY = displayH / static_cast<float>(clientRect.bottom);

				POINT pt{};
				if (::GetCursorPos(&pt) && ::ScreenToClient(targetHwnd, &pt)) {
					io.AddMousePosEvent(static_cast<float>(pt.x) * scaleX, static_cast<float>(pt.y) * scaleY);
				}
			} else {
				POINT pt{};
				if (::GetCursorPos(&pt)) {
					io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
				}
			}

			io.AddMouseButtonEvent(0, (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
			io.AddMouseButtonEvent(1, (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
			io.AddMouseButtonEvent(2, (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);

			ImGui::NewFrame();

			const float displayWidth = io.DisplaySize.x;
			const float displayHeight = io.DisplaySize.y;
			float targetWidth = (std::clamp)(displayWidth * 0.32f, 580.0f, 640.0f);
			ImGui::SetNextWindowSize(ImVec2(targetWidth, 0.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(displayWidth, (std::max)(200.0f, displayHeight - 40.0f)));
			float posX = 40.0f;
			ImGui::SetNextWindowPos(ImVec2(posX, displayHeight * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.0f, 0.5f));
			ImGui::SetNextWindowBgAlpha(Settings::GetSingleton().windowAlpha);

			std::string keyName = Settings::GetKeyName(Settings::GetSingleton().toggleKey);
			std::string winTitle = std::format("{} v{}.{}.{} ({} to close)",
				Plugin::NAME, Plugin::VERSION.major(), Plugin::VERSION.minor(), Plugin::VERSION.patch(), keyName);

			if (ImGui::Begin("###MainWindow", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGuiWindow* win = ImGui::GetCurrentWindow();
				ImRect titleBarRect = win->TitleBarRect();
				const ImGuiStyle& style = ImGui::GetStyle();
				const ImU32 titleAccent = ImGui::GetColorU32(ImVec4{ 0.78f, 0.65f, 0.36f, 1.0f });

				ImVec2 titleSize = ImGui::CalcTextSize(winTitle.c_str());
				ImVec2 titlePos(
					titleBarRect.Min.x + style.FramePadding.x,
					titleBarRect.Min.y + (titleBarRect.GetHeight() - titleSize.y) * 0.5f);

				win->DrawList->PushClipRect(titleBarRect.Min, titleBarRect.Max, false);
				win->DrawList->AddText(titlePos, titleAccent, winTitle.c_str());
				win->DrawList->PopClipRect();

				Render();
			}
			ImGui::End();

			ImGui::Render();

			ID3D11RenderTargetView* prevRTV = nullptr;
			ID3D11DepthStencilView* prevDSV = nullptr;
			s_context->OMGetRenderTargets(1, &prevRTV, &prevDSV);

			D3D11_VIEWPORT prevViewport{};
			UINT numViewports = 1;
			s_context->RSGetViewports(&numViewports, &prevViewport);

			s_context->OMSetRenderTargets(1, &targetRTV, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

			s_context->OMSetRenderTargets(1, &prevRTV, prevDSV);
			if (prevRTV) prevRTV->Release();
			if (prevDSV) prevDSV->Release();
			if (numViewports > 0) {
				s_context->RSSetViewports(1, &prevViewport);
			}
		}

		struct Hook_IDXGISwapChain_Present
		{
			static HRESULT WINAPI thunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
			{
				WaterWatch::OnFrame();
				RenderFrame(a_swapChain);
				return func(a_swapChain, a_syncInterval, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Hook_IDXGISwapChain_ResizeBuffers
		{
			static HRESULT WINAPI thunk(
				IDXGISwapChain* a_swapChain,
				UINT a_bufferCount,
				UINT a_width,
				UINT a_height,
				DXGI_FORMAT a_newFormat,
				UINT a_swapChainFlags)
			{
				OnResize();
				return func(a_swapChain, a_bufferCount, a_width, a_height, a_newFormat, a_swapChainFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool IsGameMenuOpen()
		{
			auto ui = RE::UI::GetSingleton();
			if (!ui) return false;

			if (ui->GameIsPaused() || ui->IsModalMenuOpen() || ui->IsApplicationMenuOpen() || ui->IsItemMenuOpen()) {
				return true;
			}

			static constexpr std::array<std::string_view, 21> s_checkMenus = {
				"Journal Menu",
				"InventoryMenu",
				"MagicMenu",
				"MapMenu",
				"TweenMenu",
				"StatsMenu",
				"ContainerMenu",
				"Dialogue Menu",
				"BarterMenu",
				"Crafting Menu",
				"FavoritesMenu",
				"Console",
				"Lockpicking Menu",
				"Sleep/Wait Menu",
				"CustomMenu",
				"Book Menu",
				"LevelUp Menu",
				"Training Menu",
				"RaceSex Menu",
				"GiftMenu",
				"Loading Menu"
			};

			for (const auto& name : s_checkMenus) {
				if (ui->IsMenuOpen(name)) {
					return true;
				}
			}

			return false;
		}

		class MenuOpenCloseHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuOpenCloseHandler* GetSingleton()
			{
				static MenuOpenCloseHandler instance;
				return &instance;
			}

			virtual RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent* a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_eventSource) override
			{
				(void)a_eventSource;
				if (!a_event) return RE::BSEventNotifyControl::kContinue;

				if (a_event->opening && Menu::IsOpen()) {
					if (IsGameMenuOpen()) {
						Menu::SetOpen(false);
					}
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class InputEventHandler : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputEventHandler* GetSingleton()
			{
				static InputEventHandler instance;
				return &instance;
			}

			virtual RE::BSEventNotifyControl ProcessEvent(
				RE::InputEvent* const* a_event,
				RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override
			{
				(void)a_eventSource;
				if (!a_event) return RE::BSEventNotifyControl::kContinue;

				for (auto event = *a_event; event; event = event->next) {
					if (event->eventType == RE::INPUT_EVENT_TYPE::kButton) {
						auto button = event->AsButtonEvent();
						if (!button) continue;

						if (button->GetDevice() == RE::INPUT_DEVICE::kMouse && s_initialized && Menu::IsOpen()) {
							uint32_t id = button->GetIDCode();
							if (id <= 2) {
								ImGui::GetIO().AddMouseButtonEvent(id, button->IsPressed());
							} else if (id == 8) {
								ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
							} else if (id == 9) {
								ImGui::GetIO().AddMouseWheelEvent(0.0f, -1.0f);
							}
						}

						if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
							auto scanCode = button->GetIDCode();
							static uint32_t s_cachedTargetVk = 0;
							static uint32_t s_cachedTargetScan = 0;

							uint32_t targetVk = Settings::GetFastSnapshot().toggleKey;
							if (targetVk != s_cachedTargetVk) {
								s_cachedTargetVk = targetVk;
								s_cachedTargetScan = MapVirtualKeyA(targetVk, MAPVK_VK_TO_VSC);
							}

							bool isToggleKey = (s_cachedTargetScan != 0 && scanCode == s_cachedTargetScan);
							if (!isToggleKey) {
								uint32_t vk = MapVirtualKeyA(scanCode, MAPVK_VSC_TO_VK_EX);
								isToggleKey = (vk == targetVk);
							}

							if (isToggleKey) {
								if (button->IsDown()) {
									if (!s_toggleKeyPressed.exchange(true)) {
										Menu::Toggle();
									}
								} else if (button->IsUp() || !button->IsPressed()) {
									s_toggleKeyPressed.store(false);
								}
							}
						}
					}
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	bool IsOpen()
	{
		return s_open.load();
	}

	void SetOpen(bool a_open)
	{
		if (a_open && IsGameMenuOpen()) {
			return;
		}

		if (s_open.load() == a_open) {
			return;
		}
		s_open.store(a_open);

		if (a_open) {
			LockControls();
		} else {
			UnlockControls();
		}

		auto menuCursor = RE::MenuCursor::GetSingleton();
		if (menuCursor) {
			menuCursor->SetCursorVisibility(a_open);
		}

		if (a_open) {
			::ClipCursor(nullptr);
		}

		if (s_initialized) {
			ImGui::GetIO().MouseDrawCursor = a_open;
		}

	}

	void ForceUnlockControls()
	{
		s_open.store(false);
		UnlockControls();

		auto menuCursor = RE::MenuCursor::GetSingleton();
		if (menuCursor) {
			menuCursor->SetCursorVisibility(false);
		}

		::ClipCursor(nullptr);

		if (s_initialized) {
			ImGui::GetIO().MouseDrawCursor = false;
		}
	}

	void Toggle()
	{
		if (!IsOpen() && IsGameMenuOpen()) {
			return;
		}

		SetOpen(!IsOpen());
	}

	void InstallHooks()
	{
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			logger::error("Failed to get BSGraphics::Renderer singleton for ImGui hooks.");
			return;
		}

		auto& runtimeData = renderer->GetRuntimeData();
		auto swapChain = reinterpret_cast<IDXGISwapChain*>(runtimeData.renderWindows[0].swapChain);
		s_hWnd = reinterpret_cast<HWND>(runtimeData.renderWindows[0].hWnd);

		if (!swapChain) {
			logger::error("Failed to get IDXGISwapChain from Renderer.");
			return;
		}

		DXGI_SWAP_CHAIN_DESC desc{};
		if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
			s_hWnd = desc.OutputWindow;
		}

		logger::info("Detouring IDXGISwapChain::Present (vtable slot 8)...");
		if (!Detour::Virtual<Hook_IDXGISwapChain_Present>(swapChain, 8)) {
			logger::error("Failed to hook IDXGISwapChain::Present (another mod may have patched it). The in-game menu is disabled; water settings still load from the INI.");
			return;
		}

		logger::info("Detouring IDXGISwapChain::ResizeBuffers (vtable slot 13)...");
		if (!Detour::Virtual<Hook_IDXGISwapChain_ResizeBuffers>(swapChain, 13)) {
			logger::warn("Failed to hook IDXGISwapChain::ResizeBuffers. The menu may stop drawing correctly if the game resolution changes while running.");
		}

		if (s_hWnd) {
			logger::info("Subclassing game window (HWND {:p})...", (void*)s_hWnd);
			s_originalWndProc = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrW(s_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
			if (!s_originalWndProc) {
				logger::warn("Failed to subclass the game window (error {}); retrying when the menu first draws.", ::GetLastError());
			}
		} else {
			logger::warn("Game window HWND not found; WndProc subclassing skipped.");
		}

		auto inputManager = RE::BSInputDeviceManager::GetSingleton();
		if (inputManager) {
			logger::info("Registering InputEventHandler sink with BSInputDeviceManager...");
			inputManager->AddEventSink(InputEventHandler::GetSingleton());
		}

		auto ui = RE::UI::GetSingleton();
		if (ui) {
			logger::info("Registering MenuOpenCloseHandler sink with UI...");
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuOpenCloseHandler::GetSingleton());
		}

		logger::info("ImGui hooks installed. Press {} to toggle the in-game menu.", Settings::GetKeyName(Settings::GetSingleton().toggleKey));
	}

	static bool RenderRGBScaleSliders(
		const char* a_label,
		float a_values[3],
		float a_min = 0.0f,
		float a_max = Settings::kMaxScale,
		const char* a_tooltip = nullptr,
		bool* a_rightClicked = nullptr)
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (window->SkipItems) return false;

		ImGuiContext& g = *GImGui;
		const ImGuiStyle& style = g.Style;
		bool value_changed = false;

		float square_sz = ImGui::GetFrameHeight();
		float w_extra = square_sz + style.ItemInnerSpacing.x;
		float w_regular = ImGui::CalcItemWidth();
		g.NextItemData.ClearFlags();

		ImGui::BeginGroup();
		ImGui::PushID(a_label);

		ImGui::PushMultiItemsWidths(3, w_regular - w_extra);

		const char* formats[3] = { "R: %.2fx", "G: %.2fx", "B: %.2fx" };

		for (int i = 0; i < 3; i++) {
			ImGui::PushID(i);
			if (i > 0) {
				ImGui::SameLine(0, style.ItemInnerSpacing.x);
			}
			value_changed |= ImGui::SliderFloat("", &a_values[i], a_min, a_max, formats[i], ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
			a_values[i] = std::clamp(a_values[i], a_min, a_max);

			ImGui::PopID();
			ImGui::PopItemWidth();
		}

		ImGui::SameLine(0, style.ItemInnerSpacing.x);
		ImVec4 tintPreview(
			std::clamp(a_values[0] / 2.0f, 0.0f, 1.0f),
			std::clamp(a_values[1] / 2.0f, 0.0f, 1.0f),
			std::clamp(a_values[2] / 2.0f, 0.0f, 1.0f),
			1.0f
		);
		ImGui::ColorButton("##tintPreview", tintPreview, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop, ImVec2(square_sz, square_sz));
		if (a_rightClicked && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			*a_rightClicked = true;
		}

		ImGui::PopID();

		const char* label_end = ImGui::FindRenderedTextEnd(a_label);
		if (a_label != label_end) {
			ImGui::SameLine(0, g.Style.ItemInnerSpacing.x);
			ImGui::TextEx(a_label, label_end);
		}

		ImGui::EndGroup();

		if (a_tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", a_tooltip);
		}

		return value_changed;
	}

	static bool RenderRGBColorSliders(
		const char* a_label,
		float a_values[3],
		const char* a_tooltip = nullptr,
		bool* a_rightClicked = nullptr)
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (window->SkipItems) return false;

		ImGuiContext& g = *GImGui;
		const ImGuiStyle& style = g.Style;
		bool value_changed = false;

		float square_sz = ImGui::GetFrameHeight();
		float w_extra = square_sz + style.ItemInnerSpacing.x;
		float w_regular = ImGui::CalcItemWidth();
		g.NextItemData.ClearFlags();

		ImGui::BeginGroup();
		ImGui::PushID(a_label);

		ImGui::PushMultiItemsWidths(3, w_regular - w_extra);

		const char* formats[3] = { "R: %.2f", "G: %.2f", "B: %.2f" };

		for (int i = 0; i < 3; i++) {
			ImGui::PushID(i);
			if (i > 0) {
				ImGui::SameLine(0, style.ItemInnerSpacing.x);
			}
			value_changed |= ImGui::SliderFloat("", &a_values[i], 0.0f, 1.0f, formats[i], ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
			a_values[i] = std::clamp(a_values[i], 0.0f, 1.0f);

			ImGui::PopID();
			ImGui::PopItemWidth();
		}

		ImGui::SameLine(0, style.ItemInnerSpacing.x);
		if (ImGui::ColorEdit3("##colorPreviewPicker", a_values, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoOptions)) {
			for (int i = 0; i < 3; i++) {
				a_values[i] = std::clamp(a_values[i], 0.0f, 1.0f);
			}
			value_changed = true;
		}
		if (a_rightClicked && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			*a_rightClicked = true;
		}

		ImGui::PopID();

		const char* label_end = ImGui::FindRenderedTextEnd(a_label);
		if (a_label != label_end) {
			ImGui::SameLine(0, g.Style.ItemInnerSpacing.x);
			ImGui::TextEx(a_label, label_end);
		}

		ImGui::EndGroup();

		if (a_tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", a_tooltip);
		}

		return value_changed;
	}

	static float s_labelColumnW = 0.0f;

	static void ItemTooltip(const char* a_text)
	{
		if (a_text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", a_text);
		}
	}

	static bool BeginSettingsTable(const char* a_id)
	{
		if (!ImGui::BeginTable(a_id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
			return false;
		}
		ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, s_labelColumnW);
		ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch);
		return true;
	}

	static void SettingRow(const char* a_label, const char* a_tooltip = nullptr)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(a_label);
		ItemTooltip(a_tooltip);
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-FLT_MIN);
	}

	static bool s_engineFeaturesCollapsed = false;

	struct SavedEngineFeatures
	{
		bool clearUnderwater = true;
		bool fullShaderDistantWater = true;
		bool disableRiverFlow = true;
		bool realisticDistantWater = true;
		bool removeSunGlints = true;
	};
	static SavedEngineFeatures s_savedEngineFeatures{};

	static bool RenderSectionSeparator(const char* a_label, bool a_active, const char* a_tooltip)
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		const char* labelEnd = ImGui::FindRenderedTextEnd(a_label);
		ImGui::PushStyleColor(
			ImGuiCol_Text,
			a_active ? ImVec4{ 0.78f, 0.65f, 0.36f, 1.0f } : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::SeparatorTextEx(window->GetID(a_label), a_label, labelEnd, 0.0f);
		ImGui::PopStyleColor();

		if (a_active) {
			const ImGuiStyle& style = ImGui::GetStyle();
			const ImRect& bb = GImGui->LastItemData.Rect;
			const ImVec2 labelSize = ImGui::CalcTextSize(a_label, labelEnd, false);
			const float sep1X1 = bb.Min.x;
			const float sep2X2 = bb.Max.x;
			const float sepsY = ImTrunc((bb.Min.y + bb.Max.y) * 0.5f + 0.999f);
			const float labelAvailW = ImMax(0.0f, sep2X2 - sep1X1 - style.SeparatorTextPadding.x * 2.0f);
			const float labelX = bb.Min.x + style.SeparatorTextPadding.x + ImMax(0.0f, (labelAvailW - labelSize.x) * style.SeparatorTextAlign.x);
			const float sep1X2 = labelX - style.ItemSpacing.x;
			if (sep1X2 > sep1X1 && style.SeparatorTextBorderSize > 0.0f) {
				window->DrawList->AddLineH(sep1X1, sep1X2, sepsY, IM_COL32(199, 166, 92, 255), style.SeparatorTextBorderSize);
			}
		}

		ItemTooltip(a_tooltip);
		return ImGui::IsItemClicked(ImGuiMouseButton_Right) || (!a_active && ImGui::IsItemClicked(ImGuiMouseButton_Left));
	}

	static void ResetSettingsToDefaults()
	{
		auto& settings = Settings::GetSingleton();
		settings.enabled = true;
		settings.hasCustomColors = false;
		settings.clearUnderwater = true;
		settings.fullShaderDistantWater = true;
		settings.disableRiverFlow = true;
		settings.realisticDistantWater = true;
		settings.removeSunGlints = true;
		settings.fixDepthProperties = true;
		settings.waterTransparency = 10.0f;
		settings.depthReflections = 1.0f;
		settings.depthRefraction = 0.70f;
		settings.depthNormals = 0.91f;
		settings.depthSpecularLighting = 1.00f;

		settings.colorMode = 0;
		settings.shallowColor = { 0.0f, 0.0f, 0.0f };
		settings.deepColor = { 0.0f, 0.118812f, 0.101167f };
		settings.reflectionColor = { 0.0151946f, 0.0990099f, 0.0716247f };
		settings.shallowColorScale = { 1.0f, 1.0f, 1.0f };
		settings.deepColorScale = { 1.0f, 1.0f, 1.0f };
		settings.reflectionColorScale = { 1.0f, 1.0f, 1.0f };

		s_sampledInitialWater = false;
		s_engineFeaturesCollapsed = false;
		s_savedEngineFeatures = {};


		WaterPatcher::GetSingleton().RefreshWaterSettings(false);
		settings.UpdateSnapshot();
	}

	static bool TitleBarButton(const char* a_id, const ImVec2& a_pos, float a_size, bool& a_outHovered, bool& a_outHeld)
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		const ImGuiID id = window->GetID(a_id);
		const ImRect bb(a_pos, ImVec2(a_pos.x + a_size, a_pos.y + a_size));
		ImGui::ItemAdd(bb, id);
		const bool pressed = ImGui::ButtonBehavior(bb, id, &a_outHovered, &a_outHeld);
		if (a_outHovered) {
			const ImU32 bgCol = ImGui::GetColorU32(a_outHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
			window->DrawList->AddRectFilled(bb.Min, bb.Max, bgCol);
		}
		return pressed;
	}

	static ImU32 GetTitleBarIconAccent(bool a_hovered, bool a_held)
	{
		if (a_held) {
			return IM_COL32(228, 210, 160, 255);
		}
		if (a_hovered) {
			return IM_COL32(214, 186, 120, 255);
		}
		return IM_COL32(199, 166, 92, 255);
	}

	static bool RenderBypassIconButton(const char* a_id, const ImVec2& a_pos, float a_size, bool a_enabled)
	{
		bool hovered = false;
		bool held = false;
		const bool clicked = TitleBarButton(a_id, a_pos, a_size, hovered, held);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 p0 = a_pos;
		const ImVec2 p1(p0.x + a_size, p0.y + a_size);
		const ImU32 iconCol = a_enabled ? GetTitleBarIconAccent(hovered, held) : IM_COL32(235, 85, 85, 255);

		const ImVec2 center(p0.x + a_size * 0.5f, p0.y + a_size * 0.53f);
		const float r = a_size * 0.38f;

		drawList->PathArcTo(center, r, -0.28f * 3.14159265f, 1.28f * 3.14159265f, 20);
		drawList->PathStroke(iconCol, 0, 1.5f);

		const float stemTop = center.y - r * 1.25f;
		const float stemBot = center.y + r * 0.15f;
		drawList->AddLine(ImVec2(center.x, stemTop), ImVec2(center.x, stemBot), iconCol, 1.6f);

		if (!a_enabled) {
			const float pad = a_size * 0.12f;
			drawList->AddLine(ImVec2(p0.x + pad, p1.y - pad), ImVec2(p1.x - pad, p0.y + pad), iconCol, 1.5f);
		}

		if (hovered) {
			if (a_enabled) {
				ImGui::SetTooltip("Turn off Water On Steroids");
			} else {
				ImGui::SetTooltip("Turn on Water On Steroids");
			}
		}

		return clicked;
	}

	static bool RenderTitleBarResetButton(const char* a_id, const ImVec2& a_pos, float a_size)
	{
		bool hovered = false;
		bool held = false;
		const bool clicked = TitleBarButton(a_id, a_pos, a_size, hovered, held);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 p0 = a_pos;
		const ImU32 iconCol = GetTitleBarIconAccent(hovered, held);

		const ImVec2 center(p0.x + a_size * 0.5f, p0.y + a_size * 0.5f);
		const float r = a_size * 0.36f;
		const float s = a_size / 16.0f;

		const float startAngle = -0.40f * 3.14159265f;
		const float endAngle = 1.35f * 3.14159265f;
		drawList->PathArcTo(center, r, startAngle, endAngle, 20);
		drawList->PathStroke(iconCol, 0, 1.5f);

		const float tipX = center.x + std::cos(startAngle) * r;
		const float tipY = center.y + std::sin(startAngle) * r;
		const ImVec2 tip(tipX, tipY);
		const ImVec2 w1(tip.x + 3.2f * s, tip.y - 1.2f * s);
		const ImVec2 w2(tip.x + 1.2f * s, tip.y + 3.4f * s);
		drawList->AddTriangleFilled(tip, w1, w2, iconCol);
		drawList->AddCircleFilled(center, 1.2f * s, iconCol);

		if (hovered) {
			ImGui::SetTooltip("Reset settings to defaults");
		}

		return clicked;
	}

	static bool RenderResetDefaultsIconButton(
		const char* a_id,
		float a_size,
		const char* a_tooltip = "Reset settings to defaults")
	{
		ImGui::PushID(a_id);

		ImVec2 btnSize(a_size, a_size);
		ImVec2 screenPos = ImGui::GetCursorScreenPos();
		bool clicked = ImGui::InvisibleButton("##btn", btnSize);
		bool hovered = ImGui::IsItemHovered();
		bool active = ImGui::IsItemActive();

		ImDrawList* drawList = ImGui::GetWindowDrawList();

		ImVec2 p0 = screenPos;
		ImVec2 p1 = ImVec2(p0.x + a_size, p0.y + a_size);

		ImU32 bgCol = active ? IM_COL32(48, 48, 48, 255) :
			hovered ? IM_COL32(32, 32, 32, 240) :
			IM_COL32(14, 14, 14, 220);

		ImU32 iconCol = active ? IM_COL32(228, 210, 160, 255) :
			hovered ? IM_COL32(210, 178, 105, 255) :
			IM_COL32(170, 170, 170, 220);

		drawList->AddRectFilled(p0, p1, bgCol, 4.0f);

		ImVec2 center(p0.x + a_size * 0.5f, p0.y + a_size * 0.5f);
		float r = a_size * 0.28f;

		float startAngle = -0.40f * 3.14159265f;
		float endAngle = 1.35f * 3.14159265f;
		drawList->PathArcTo(center, r, startAngle, endAngle, 20);
		drawList->PathStroke(iconCol, 0, 1.5f);

		float tipX = center.x + std::cos(startAngle) * r;
		float tipY = center.y + std::sin(startAngle) * r;
		ImVec2 tip(tipX, tipY);
		ImVec2 w1(tip.x + 3.2f, tip.y - 1.2f);
		ImVec2 w2(tip.x + 1.2f, tip.y + 3.4f);
		drawList->AddTriangleFilled(tip, w1, w2, iconCol);

		drawList->AddCircleFilled(center, 1.3f, iconCol);

		if (hovered && a_tooltip) {
			ImGui::SetTooltip("%s", a_tooltip);
		}

		ImGui::PopID();
		return clicked;
	}

	static bool RenderSaveIniIconButton(const char* a_id, const ImVec2& a_pos, float a_size)
	{
		bool hovered = false;
		bool held = false;
		const bool clicked = TitleBarButton(a_id, a_pos, a_size, hovered, held);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 p0 = a_pos;
		const ImVec2 p1(p0.x + a_size, p0.y + a_size);
		const ImU32 iconCol = GetTitleBarIconAccent(hovered, held);

		const float pad = a_size * 0.11f;
		const ImVec2 d0(p0.x + pad, p0.y + pad);
		const ImVec2 d1(p1.x - pad, p1.y - pad);
		const float dw = d1.x - d0.x;
		const float dh = d1.y - d0.y;

		const float chamfer = a_size * 0.18f;
		drawList->PathLineTo(ImVec2(d0.x, d0.y));
		drawList->PathLineTo(ImVec2(d1.x - chamfer, d0.y));
		drawList->PathLineTo(ImVec2(d1.x, d0.y + chamfer));
		drawList->PathLineTo(ImVec2(d1.x, d1.y));
		drawList->PathLineTo(ImVec2(d0.x, d1.y));
		drawList->PathStroke(iconCol, ImDrawFlags_Closed, 1.4f);

		const float shutterW = dw * 0.50f;
		const float shutterH = dh * 0.36f;
		const float shutterX = d0.x + (dw - shutterW) * 0.40f;
		drawList->AddRect(ImVec2(shutterX, d0.y), ImVec2(shutterX + shutterW, d0.y + shutterH), iconCol, 0.0f, 0, 1.2f);

		const float labelH = dh * 0.34f;
		const float labelPad = dw * 0.16f;
		drawList->AddRectFilled(ImVec2(d0.x + labelPad, d1.y - labelH), ImVec2(d1.x - labelPad, d1.y), iconCol);

		if (hovered) {
			ImGui::SetTooltip("Save settings to INI");
		}

		return clicked;
	}

	static bool RenderDiagnosticLogoButton(const char* a_id, const ImVec2& a_pos, float a_size, int a_step)
	{
		bool hovered = false;
		bool held = false;
		const bool clicked = TitleBarButton(a_id, a_pos, a_size, hovered, held);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 p0 = a_pos;
		const ImVec2 p1(p0.x + a_size, p0.y + a_size);

		ImU32 iconCol = GetTitleBarIconAccent(hovered, held);
		if (a_step == 1) {
			iconCol = IM_COL32(255, 95, 95, 255);
		} else if (a_step == 2) {
			iconCol = IM_COL32(95, 230, 115, 255);
		} else if (a_step == 3) {
			iconCol = IM_COL32(255, 155, 50, 255);
		}

		const float pad = a_size * 0.06f;
		const ImVec2 b0(p0.x + pad, p0.y + pad);
		const ImVec2 b1(p1.x - pad, p1.y - pad);
		const float innerW = b1.x - b0.x;
		const float innerH = b1.y - b0.y;
		const float midY = b0.y + innerH * 0.52f;

		const ImVec2 pt1(b0.x, midY);
		const ImVec2 pt2(b0.x + innerW * 0.18f, midY);
		const ImVec2 pt3(b0.x + innerW * 0.34f, midY + innerH * 0.28f);
		const ImVec2 pt4(b0.x + innerW * 0.52f, midY - innerH * 0.46f);
		const ImVec2 pt5(b0.x + innerW * 0.70f, midY + innerH * 0.40f);
		const ImVec2 pt6(b0.x + innerW * 0.84f, midY);
		const ImVec2 pt7(b1.x, midY);

		drawList->AddLine(pt1, pt2, iconCol, 1.5f);
		drawList->AddLine(pt2, pt3, iconCol, 1.5f);
		drawList->AddLine(pt3, pt4, iconCol, 1.5f);
		drawList->AddLine(pt4, pt5, iconCol, 1.5f);
		drawList->AddLine(pt5, pt6, iconCol, 1.5f);
		drawList->AddLine(pt6, pt7, iconCol, 1.5f);

		if (hovered) {
			static constexpr const char* kTooltips[] = {
				"Start live water log (water looks normal)",
				"Mark water as normal",
				"Mark bug and stop live water log",
				"Restart live water log"
			};
			ImGui::SetTooltip("%s", kTooltips[std::clamp(a_step, 0, 3)]);
		}

		return clicked;
	}

	static bool RenderSampleColorIconButton(const char* a_id, float a_size)
	{
		ImGui::PushID(a_id);

		ImVec2 btnSize(a_size, a_size);
		ImVec2 screenPos = ImGui::GetCursorScreenPos();
		bool clicked = ImGui::InvisibleButton("##btn", btnSize);
		bool hovered = ImGui::IsItemHovered();
		bool active = ImGui::IsItemActive();

		ImDrawList* drawList = ImGui::GetWindowDrawList();

		ImVec2 p0 = screenPos;
		ImVec2 p1 = ImVec2(p0.x + a_size, p0.y + a_size);

		ImU32 bgCol = active ? IM_COL32(48, 48, 48, 255) :
			hovered ? IM_COL32(32, 32, 32, 240) :
			IM_COL32(14, 14, 14, 220);

		ImU32 iconCol = active ? IM_COL32(228, 210, 160, 255) :
			hovered ? IM_COL32(210, 178, 105, 255) :
			IM_COL32(170, 170, 170, 220);

		drawList->AddRectFilled(p0, p1, bgCol, 4.0f);

		float ox = p0.x;
		float oy = p0.y;

		ImVec2 pts[] = {
			ImVec2(ox + a_size * 0.36f, oy + a_size * 0.20f),
			ImVec2(ox + a_size * 0.57f, oy + a_size * 0.18f),
			ImVec2(ox + a_size * 0.75f, oy + a_size * 0.25f),
			ImVec2(ox + a_size * 0.84f, oy + a_size * 0.43f),
			ImVec2(ox + a_size * 0.80f, oy + a_size * 0.64f),
			ImVec2(ox + a_size * 0.64f, oy + a_size * 0.80f),
			ImVec2(ox + a_size * 0.43f, oy + a_size * 0.80f),
			ImVec2(ox + a_size * 0.38f, oy + a_size * 0.66f),
			ImVec2(ox + a_size * 0.27f, oy + a_size * 0.59f),
			ImVec2(ox + a_size * 0.18f, oy + a_size * 0.45f),
			ImVec2(ox + a_size * 0.20f, oy + a_size * 0.30f)
		};
		drawList->AddPolyline(pts, IM_ARRAYSIZE(pts), iconCol, ImDrawFlags_Closed, 1.4f);

		drawList->AddCircle(ImVec2(ox + a_size * 0.34f, oy + a_size * 0.66f), a_size * 0.07f, iconCol, 12, 1.2f);

		drawList->AddCircleFilled(ImVec2(ox + a_size * 0.35f, oy + a_size * 0.36f), a_size * 0.065f, iconCol);
		drawList->AddCircleFilled(ImVec2(ox + a_size * 0.52f, oy + a_size * 0.31f), a_size * 0.065f, iconCol);
		drawList->AddCircleFilled(ImVec2(ox + a_size * 0.69f, oy + a_size * 0.36f), a_size * 0.065f, iconCol);
		drawList->AddCircleFilled(ImVec2(ox + a_size * 0.72f, oy + a_size * 0.54f), a_size * 0.060f, iconCol);

		ImGui::PopID();
		return clicked;
	}

	static bool RenderTitleBarCloseButton(const char* a_id, const ImVec2& a_pos, float a_size)
	{
		bool hovered = false;
		bool held = false;
		const bool clicked = TitleBarButton(a_id, a_pos, a_size, hovered, held);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImU32 iconCol = GetTitleBarIconAccent(hovered, held);

		const ImVec2 center(a_pos.x + a_size * 0.5f, a_pos.y + a_size * 0.5f);
		const float r = a_size * 0.30f;
		drawList->AddLine(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r), iconCol, 1.5f);
		drawList->AddLine(ImVec2(center.x + r, center.y - r), ImVec2(center.x - r, center.y + r), iconCol, 1.5f);

		return clicked;
	}

	void Render()
	{
		auto& settings = Settings::GetSingleton();
		bool fullPatchChanged = false;

		ImGuiWindow* window = ImGui::GetCurrentWindow();
		{
			ImGuiContext& g = *GImGui;
			const ImGuiStyle& style = g.Style;
			const ImRect titleBarRect = window->TitleBarRect();
			const float btnSize = titleBarRect.GetHeight() - 2.0f;
			const float btnSpacing = 4.0f;
			const float closeBtnLeftX = titleBarRect.Max.x - style.FramePadding.x - btnSize;
			const float btnY = titleBarRect.Min.y + (titleBarRect.GetHeight() - btnSize) * 0.5f;
			const float firstBtnX = closeBtnLeftX - (btnSize + btnSpacing) * 4.0f;

			const ImGuiItemFlags prevItemFlags = g.CurrentItemFlags;
			const ImGuiNavLayer prevNavLayer = window->DC.NavLayerCurrent;
			g.CurrentItemFlags |= ImGuiItemFlags_NoNavDefaultFocus | ImGuiItemFlags_NoFocus;
			window->DC.NavLayerCurrent = ImGuiNavLayer_Menu;
			ImGui::PushClipRect(titleBarRect.Min, titleBarRect.Max, false);

			if (RenderBypassIconButton("##topBypassMod", ImVec2(firstBtnX, btnY), btnSize, settings.enabled)) {
				settings.enabled = !settings.enabled;
				fullPatchChanged = true;
			}
			if (RenderSaveIniIconButton("##topSaveIni", ImVec2(firstBtnX + (btnSize + btnSpacing) * 1.0f, btnY), btnSize)) {
				settings.Save();
				ShowHUDNotification("Settings saved to INI.");
				logger::info("Saved settings to INI.");
			}
			if (RenderTitleBarResetButton("##topResetDefaults", ImVec2(firstBtnX + (btnSize + btnSpacing) * 2.0f, btnY), btnSize)) {
				ResetSettingsToDefaults();
				ShowHUDNotification("Settings reset to defaults.");
				logger::info("Reset settings to defaults.");
			}

			static int    liveLogStep = 0;
			static double bugMarkedTime = -1.0;
			constexpr double kBugMarkedFlashSeconds = 2.0;
			if (!WaterWatch::IsEnabled()) {
				liveLogStep = 0;
			} else if (liveLogStep == 0) {
				liveLogStep = 1;
			}
			const bool bugMarkedFlash = liveLogStep == 0 && bugMarkedTime >= 0.0 &&
			                            ImGui::GetTime() - bugMarkedTime < kBugMarkedFlashSeconds;
			if (RenderDiagnosticLogoButton("##topDiagLogo", ImVec2(firstBtnX + (btnSize + btnSpacing) * 3.0f, btnY), btnSize, bugMarkedFlash ? 3 : liveLogStep)) {
				switch (liveLogStep) {
				case 0:
					bugMarkedTime = -1.0;
					WaterWatch::SetEnabled(true);
					if (WaterWatch::IsEnabled()) {
						settings.liveWaterLog = true;
						WaterWatch::MarkAfterFrames("LOOKS CORRECT", 2);
						liveLogStep = 2;
						ShowHUDNotification("Live water log recording. Marked: looks correct.");
					} else {
						ShowHUDNotification("Live water log could not start (see the SKSE log).");
					}
					break;
				case 1:
					WaterWatch::Mark("LOOKS CORRECT");
					liveLogStep = 2;
					ShowHUDNotification("Marked: looks correct.");
					break;
				default:
					WaterWatch::Mark("BUG VISIBLE");
					WaterWatch::SetEnabled(false);
					settings.liveWaterLog = false;
					liveLogStep = 0;
					bugMarkedTime = ImGui::GetTime();
					ShowHUDNotification("Marked: bug visible. Live water log stopped.");
					break;
				}
			}
			if (RenderTitleBarCloseButton("##topClose", ImVec2(closeBtnLeftX, btnY), btnSize)) {
				SetOpen(false);
			}

			ImGui::PopClipRect();
			window->DC.NavLayerCurrent = prevNavLayer;
			g.CurrentItemFlags = prevItemFlags;
		}

		if (!settings.enabled) {
			ImGui::BeginDisabled();
		}

		{
			const bool headerClicked = RenderSectionSeparator(
				"Engine Features",
				!s_engineFeaturesCollapsed,
				s_engineFeaturesCollapsed ?
					"All engine features are unchecked and collapsed.\n"
					"Right-click (or click) to restore and expand." :
					"Right-click to uncheck all engine features and collapse this section.");

			if (!s_engineFeaturesCollapsed) {
				const bool featureTable = ImGui::BeginTable("##engineFeatures", 2, ImGuiTableFlags_SizingStretchSame);

				if (featureTable) ImGui::TableNextColumn();
				ImGui::Checkbox("Clear Underwater", &settings.clearUnderwater);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Reduces the underwater blur.");
				}

				if (featureTable) ImGui::TableNextColumn();
				ImGui::Checkbox("Full Shader for Distant Water", &settings.fullShaderDistantWater);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Distant water uses the same shader as nearby water,\n"
						"with the colors of the nearest lake or river.");
				}

				if (featureTable) ImGui::TableNextColumn();
				ImGui::Checkbox("Remove Sun Glints", &settings.removeSunGlints);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Removes the flashing sun specks on calm water.\n"
						"The sun's reflection stays.");
				}

				if (featureTable) ImGui::TableNextColumn();
				ImGui::Checkbox("Realistic Distant Water", &settings.realisticDistantWater);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Keeps water color and depth at any distance instead of a flat sky reflection.\n"
						"Also turns on Disable River Flow.");
				}

				if (featureTable) ImGui::TableNextColumn();
				const bool riverFlowForced = settings.realisticDistantWater;
				bool       riverFlowShown = riverFlowForced || settings.disableRiverFlow;
				ImGui::BeginDisabled(riverFlowForced);
				ImGui::Checkbox("Disable River Flow", riverFlowForced ? &riverFlowShown : &settings.disableRiverFlow);
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip(
						"%s"
						"Removes river flow to hide the seam where rivers meet distant water.",
						riverFlowForced ?
							"Always on while Realistic Distant Water is on.\n\n" :
							"");
				}

				if (featureTable) {
					ImGui::EndTable();
				}

				if (headerClicked) {
					if (settings.clearUnderwater || settings.fullShaderDistantWater ||
						settings.disableRiverFlow ||
						settings.realisticDistantWater || settings.removeSunGlints) {
						s_savedEngineFeatures = {
							settings.clearUnderwater,
							settings.fullShaderDistantWater,
							settings.disableRiverFlow,
							settings.realisticDistantWater,
							settings.removeSunGlints
						};
					}
					settings.clearUnderwater = false;
					settings.fullShaderDistantWater = false;
					settings.disableRiverFlow = false;
					settings.realisticDistantWater = false;
					settings.removeSunGlints = false;
					s_engineFeaturesCollapsed = true;
					fullPatchChanged = true;
				}
			} else if (headerClicked) {
				settings.clearUnderwater = s_savedEngineFeatures.clearUnderwater;
				settings.fullShaderDistantWater = s_savedEngineFeatures.fullShaderDistantWater;
				settings.disableRiverFlow = s_savedEngineFeatures.disableRiverFlow;
				settings.realisticDistantWater = s_savedEngineFeatures.realisticDistantWater;
				settings.removeSunGlints = s_savedEngineFeatures.removeSunGlints;
				s_engineFeaturesCollapsed = false;
				fullPatchChanged = true;
			}
		}

		{
			const char* labels[] = { "Refraction", "Reflections", "Normals", "Specular Lighting", "Transparency",
				"Color Mode", "Shallow Color", "Deep Color", "Reflection Color" };
			float widest = 0.0f;
			for (const char* label : labels) {
				widest = (std::max)(widest, ImGui::CalcTextSize(label).x);
			}
			s_labelColumnW = widest + ImGui::GetStyle().ItemSpacing.x;
		}

		{
			const bool edgeHeaderClicked = RenderSectionSeparator(
				"Water Depth",
				settings.fixDepthProperties,
				"How water effects fade out toward the shore (0.00 = no fade, 1.00 = full fade).\n"
				"Off uses the game's values. Right-click to turn off.");

			auto depthSlider = [&](const char* a_label, const char* a_id, float* a_value, const char* a_tooltip) {
				SettingRow(a_label, a_tooltip);
				ImGui::SliderFloat(a_id, a_value, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_NoInput);
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					fullPatchChanged = true;
				}
				ItemTooltip(a_tooltip);
			};

			if (settings.fixDepthProperties) {
				if (BeginSettingsTable("##waterDepth")) {
					depthSlider("Refraction", "##depthRefraction", &settings.depthRefraction,
						"Refraction fade near the shore (default 0.70).");
					depthSlider("Reflections", "##depthReflections", &settings.depthReflections,
						"Reflection fade near the shore (default 1.00).");
					depthSlider("Normals", "##depthNormals", &settings.depthNormals,
						"Wave fade near the shore (default 0.91).");
					depthSlider("Specular Lighting", "##depthSpecular", &settings.depthSpecularLighting,
						"Sun glare fade near the shore (default 1.00).");

					ImGui::EndTable();
				}

				if (edgeHeaderClicked) {
					settings.fixDepthProperties = false;
					fullPatchChanged = true;
				}
			} else if (edgeHeaderClicked) {
				settings.fixDepthProperties = true;
				fullPatchChanged = true;
			}
		}

		RenderSectionSeparator("Water Colors", true, nullptr);

		if (!settings.hasCustomColors && !s_sampledInitialWater) {
			if (WaterPatcher::GetSingleton().CaptureActiveWaterColors()) {
				s_sampledInitialWater = true;
			}
		}

		if (BeginSettingsTable("##waterColors")) {
			const auto& style = ImGui::GetStyle();
			const float frameH = ImGui::GetFrameHeight();

			constexpr const char* kColorModeTip =
				"Direct Override: your own colors for all water.\n"
				"Tint / Multiplier: adjusts each water's own colors.";
			SettingRow("Color Mode", kColorModeTip);
			ImGui::SetNextItemWidth(-(frameH + style.ItemInnerSpacing.x));
			const char* modeItems[] = { "Direct Override", "Tint / Multiplier" };
			if (ImGui::Combo("##colorMode", &settings.colorMode, modeItems, IM_ARRAYSIZE(modeItems))) {
				if (settings.colorMode == 1) {
					settings.shallowColorScale = { 1.0f, 1.0f, 1.0f };
					settings.deepColorScale = { 1.0f, 1.0f, 1.0f };
					settings.reflectionColorScale = { 1.0f, 1.0f, 1.0f };
				}
				settings.hasCustomColors = true;
			}
			ItemTooltip(kColorModeTip);

			ImGui::SameLine(0, style.ItemInnerSpacing.x);
			if (settings.colorMode == 0) {
				if (RenderSampleColorIconButton("##directOverrideSampleColor", frameH)) {
					if (WaterPatcher::GetSingleton().CaptureActiveWaterColors()) {
						settings.hasCustomColors = true;
					}
				}
				ItemTooltip(
					"Sample Water Colors\n"
					"Copies the colors of the water you are in or closest to.");

				constexpr const char* kDeepTip =
					"Water color away from the shore.\n"
					"Right-click the square to copy it to Shallow Color.";
				SettingRow("Deep Color", kDeepTip);
				bool deepRightClicked = false;
				if (RenderRGBColorSliders("##deepColor", settings.deepColor.data(), kDeepTip, &deepRightClicked)) {
					settings.hasCustomColors = true;
				}
				if (deepRightClicked) {
					settings.shallowColor = settings.deepColor;
					settings.hasCustomColors = true;
				}

				constexpr const char* kShallowTip =
					"Water color near the shore.\n"
					"Right-click the square to copy it to Deep Color.";
				SettingRow("Shallow Color", kShallowTip);
				bool shallowRightClicked = false;
				if (RenderRGBColorSliders("##shallowColor", settings.shallowColor.data(), kShallowTip, &shallowRightClicked)) {
					settings.hasCustomColors = true;
				}
				if (shallowRightClicked) {
					settings.deepColor = settings.shallowColor;
					settings.hasCustomColors = true;
				}

				constexpr const char* kReflectionTip = "Color of reflections on the water.";
				SettingRow("Reflection Color", kReflectionTip);
				if (RenderRGBColorSliders("##reflectionColor", settings.reflectionColor.data(), kReflectionTip)) {
					settings.hasCustomColors = true;
				}
			} else {
				if (RenderResetDefaultsIconButton("##resetColorScales", frameH,
						"Reset multipliers to 1.00x")) {
					settings.shallowColorScale = { 1.0f, 1.0f, 1.0f };
					settings.deepColorScale = { 1.0f, 1.0f, 1.0f };
					settings.reflectionColorScale = { 1.0f, 1.0f, 1.0f };
				}

				constexpr const char* kDeepTip =
					"Multiplies the deep water color (1.00x = unchanged).\n"
					"Right-click the square to copy it to Shallow Color.";
				SettingRow("Deep Color", kDeepTip);
				bool deepRightClicked = false;
				if (RenderRGBScaleSliders("##deepColorScale", settings.deepColorScale.data(), 0.0f, Settings::kMaxScale, kDeepTip, &deepRightClicked)) {
					settings.hasCustomColors = true;
				}
				if (deepRightClicked) {
					settings.shallowColorScale = settings.deepColorScale;
					settings.hasCustomColors = true;
				}

				constexpr const char* kShallowTip =
					"Multiplies the shallow water color (1.00x = unchanged).\n"
					"Right-click the square to copy it to Deep Color.";
				SettingRow("Shallow Color", kShallowTip);
				bool shallowRightClicked = false;
				if (RenderRGBScaleSliders("##shallowColorScale", settings.shallowColorScale.data(), 0.0f, Settings::kMaxScale, kShallowTip, &shallowRightClicked)) {
					settings.hasCustomColors = true;
				}
				if (shallowRightClicked) {
					settings.deepColorScale = settings.shallowColorScale;
					settings.hasCustomColors = true;
				}

				constexpr const char* kReflectionTip = "Multiplies the reflection color (1.00x = unchanged).";
				SettingRow("Reflection Color", kReflectionTip);
				if (RenderRGBScaleSliders("##reflectionColorScale", settings.reflectionColorScale.data(), 0.0f, Settings::kMaxScale, kReflectionTip)) {
					settings.hasCustomColors = true;
				}
			}

			constexpr const char* kTransparencyTip =
				"How clear the water is: 0% = unchanged, 100% = fully clear.";
			SettingRow("Transparency", kTransparencyTip);
			ImGui::SliderFloat("##waterTransparency", &settings.waterTransparency, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_NoInput);
			ItemTooltip(kTransparencyTip);

			ImGui::EndTable();
		}

		if (!settings.enabled) {
			ImGui::EndDisabled();
		}

		if (fullPatchChanged) {
			WaterPatcher::GetSingleton().RefreshWaterSettings(true);
		}

		settings.UpdateSnapshot();
	}
}
