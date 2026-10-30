#include "ShaderPatch.h"
#include "ShaderBytecode.h"
#include "Settings.h"

#include <RE/T/TESWaterSystem.h>

#include <d3d11.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace
{
	constexpr std::uint32_t kSlotCreatePixelShader = 15;
	constexpr std::uint32_t kSlotPSSetShader = 9;

	constexpr GUID kPatchedShader{ 0x5f1e2a37, 0x9c4b, 0x4d3e, { 0xa1, 0x7e, 0x2b, 0x60, 0x93, 0xc4, 0xd8, 0x51 } };

	std::atomic<std::uint32_t> shadersPatched{ 0 };
	std::atomic<std::uint32_t> patchFailures{ 0 };
	std::atomic<bool>          deviceHooked{ false };
	std::atomic<bool>          enbFound{ false };

	std::shared_mutex                                               shadersLock;
	std::unordered_map<ID3D11PixelShader*, ID3D11PixelShader*> patchedShaders;

	bool Active()
	{
		const auto& settings = Settings::GetFastSnapshot();
		if (!settings.enabled || !settings.realisticDistantWater) {
			return false;
		}
		const auto* waterSystem = RE::TESWaterSystem::GetSingleton();
		return !(waterSystem && waterSystem->playerUnderwater);
	}

	struct Hook_CreatePixelShader
	{
		static HRESULT thunk(ID3D11Device* a_this, const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage, ID3D11PixelShader** a_shader)
		{
			const HRESULT result = func(a_this, a_bytecode, a_length, a_linkage, a_shader);
			if (FAILED(result) || !a_shader || !*a_shader || a_linkage) {
				return result;
			}
			const auto patched = ShaderBytecode::PatchRealisticWater(a_bytecode, a_length);
			if (!patched) {
				return result;
			}
			ID3D11PixelShader* shader = nullptr;
			if (FAILED(func(a_this, patched->data(), patched->size(), nullptr, &shader)) || !shader) {
				if (patchFailures++ < 5) {
					logger::warn("Realistic Distant Water: the device refused a patched water pixel shader; that shader stays vanilla.");
				}
				return result;
			}
			(*a_shader)->SetPrivateDataInterface(kPatchedShader, shader);
			{
				std::unique_lock lock(shadersLock);
				patchedShaders[*a_shader] = shader;
			}
			shader->Release();
			if (++shadersPatched == 1) {
				logger::info("Realistic Distant Water: first water pixel shader patched.");
			}
			return result;
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Hook_PSSetShader
	{
		static void thunk(ID3D11DeviceContext* a_this, ID3D11PixelShader* a_shader, ID3D11ClassInstance* const* a_instances, UINT a_count)
		{
			if (a_shader && Active()) {
				ID3D11PixelShader* candidate = nullptr;
				{
					std::shared_lock lock(shadersLock);
					if (const auto it = patchedShaders.find(a_shader); it != patchedShaders.end()) {
						candidate = it->second;
					}
				}
				if (candidate) {
					IUnknown* stored = nullptr;
					UINT      size = sizeof(stored);
					if (SUCCEEDED(a_shader->GetPrivateData(kPatchedShader, &size, &stored)) && stored) {
						if (stored == candidate) {
							a_shader = candidate;
						}
						stored->Release();
					}
				}
			}
			func(a_this, a_shader, a_instances, a_count);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	void HookDevice(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		if (!a_device || deviceHooked) {
			return;
		}
		if (const auto module = GetModuleHandleW(L"d3d11.dll"); module && GetProcAddress(module, "ENBGetSDKVersion")) {
			enbFound = true;
			return;
		}
		ID3D11DeviceContext* context = a_context;
		if (!context) {
			a_device->GetImmediateContext(&context);
		} else {
			context->AddRef();
		}
		if (!context) {
			return;
		}
		const bool hooked = Detour::Virtual<Hook_CreatePixelShader>(a_device, kSlotCreatePixelShader) &&
		                    Detour::Virtual<Hook_PSSetShader>(context, kSlotPSSetShader);
		context->Release();
		deviceHooked = hooked;
	}

	using CreateDeviceAndSwapChain_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

	CreateDeviceAndSwapChain_t originalCreateDevice{ nullptr };

	HRESULT WINAPI CreateDeviceAndSwapChain(IDXGIAdapter* a_adapter, D3D_DRIVER_TYPE a_driverType, HMODULE a_software, UINT a_flags,
		const D3D_FEATURE_LEVEL* a_levels, UINT a_levelCount, UINT a_sdkVersion, const DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_swapChain,
		ID3D11Device** a_device, D3D_FEATURE_LEVEL* a_level, ID3D11DeviceContext** a_context)
	{
		const HRESULT result = originalCreateDevice(a_adapter, a_driverType, a_software, a_flags, a_levels, a_levelCount, a_sdkVersion, a_desc,
			a_swapChain, a_device, a_level, a_context);
		if (SUCCEEDED(result) && a_device && *a_device) {
			HookDevice(*a_device, a_context ? *a_context : nullptr);
		}
		return result;
	}
}

void ShaderPatch::Install()
{
	originalCreateDevice = reinterpret_cast<CreateDeviceAndSwapChain_t>(SKSE::PatchIAT(CreateDeviceAndSwapChain, "d3d11.dll", "D3D11CreateDeviceAndSwapChain"));
	if (!originalCreateDevice) {
		logger::error("Realistic Distant Water: could not hook D3D11CreateDeviceAndSwapChain; distant water stays vanilla.");
	}
}

void ShaderPatch::ReportStatus()
{
	if (enbFound) {
		logger::info("Realistic Distant Water: ENB found; its own water shader is left alone (use DisableDistantReflection in enbseries.ini).");
	} else if (!deviceHooked) {
		logger::error("Realistic Distant Water: the game's D3D11 device was not hooked; distant water stays vanilla.");
	} else {
		logger::info("Realistic Distant Water: {} water pixel shaders patched so far ({} refused); distant water keeps its color, depth and refraction, and screen-space reflections keep the game's own distance fade.",
			shadersPatched.load(), patchFailures.load());
	}
}
