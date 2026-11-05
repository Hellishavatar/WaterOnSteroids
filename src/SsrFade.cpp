#include "SsrFade.h"
#include "CellWaterMap.h"

#include <RE/M/Main.h>
#include <RE/N/NiCamera.h>
#include <RE/R/Renderer.h>
#include <RE/T/TES.h>
#include <RE/T/TESWorldSpace.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3dcompiler")

namespace
{
	using Microsoft::WRL::ComPtr;

	constexpr std::int32_t kMapCells = 16;
	constexpr float        kCellSize = 4096.0f;
	constexpr float        kNoWaterHeight = -1.0e30f;

	constexpr char kShader[] = R"(
cbuffer Fade : register(b0)
{
	float nearTimesFar;
	float farPlane;
	float range;
	float height;
	float4 forward;
	float4 rightAxis;
	float4 upAxis;
	float4 eye;
	float4 window;
};
Texture2D<float> Depth : register(t0);
Texture2D<float> Heights : register(t1);

void VS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2((id << 1) & 2, id & 2);
	position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float MapDistance(float3 o, float3 d)
{
	float  cellSize = window.w;
	float2 local = (o.xy - window.xy) / cellSize;
	int2   cell = (int2)floor(local);
	float2 stepDir = float2(d.x >= 0 ? 1 : -1, d.y >= 0 ? 1 : -1);
	float2 inverse = 1.0 / max(abs(d.xy), 1e-6);
	float2 delta = cellSize * inverse;
	float2 next = float2(d.x >= 0 ? cell.x + 1 - local.x : local.x - cell.x, d.y >= 0 ? cell.y + 1 - local.y : local.y - cell.y) * delta;
	float  start = 0;
	[loop] for (int i = 0; i < 32; ++i) {
		if (any(cell < 0) || any(cell >= (int)window.z) || start > 9000) {
			break;
		}
		float end = min(next.x, next.y);
		float h = Heights.Load(int3(cell, 0));
		if (o.z + d.z * start <= h) {
			return start;
		}
		if (d.z < 0) {
			float hit = (h - o.z) / d.z;
			if (hit <= end) {
				return max(hit, start);
			}
		}
		if (next.x < next.y) {
			next.x += delta.x;
			cell.x += (int)stepDir.x;
		} else {
			next.y += delta.y;
			cell.y += (int)stepDir.y;
		}
		start = end;
	}
	return 1e9;
}

float4 PS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float3 ray = forward.xyz + lerp(forward.w, rightAxis.w, uv.x) * rightAxis.xyz + lerp(upAxis.w, -upAxis.w, uv.y) * upAxis.xyz;
	float  distance;
	if (eye.w > 0) {
		distance = MapDistance(eye.xyz, normalize(ray));
	} else {
		uint width, h;
		Depth.GetDimensions(width, h);
		float depth = Depth.Load(int3(uv * float2(width, h), 0));
		distance = nearTimesFar / (farPlane - depth * range);
		if (height > 0 && ray.z < 0) {
			distance = height / -ray.z;
		}
		distance *= length(ray);
	}
	return float4(0, 0, 0, saturate((8192 - distance) / 4185));
}
)";

	struct Constants
	{
		float nearTimesFar;
		float farPlane;
		float range;
		float height;
		float forward[4];
		float rightAxis[4];
		float upAxis[4];
		float eye[4];
		float window[4];
	};

	struct Resources
	{
		ComPtr<ID3D11VertexShader>       vertex;
		ComPtr<ID3D11PixelShader>        pixel;
		ComPtr<ID3D11BlendState>         blend;
		ComPtr<ID3D11DepthStencilState>  depth;
		ComPtr<ID3D11RasterizerState>    raster;
		ComPtr<ID3D11Buffer>             constants;
		ComPtr<ID3D11Texture2D>          heights;
		ComPtr<ID3D11ShaderResourceView> heightsView;
		bool                             ready{ false };
		bool                             failed{ false };
	};

	Resources         resources;
	std::atomic<bool> doneThisFrame{ false };

	constexpr float kNoWater = 1.0e9f;
	float           nearestDistance{ kNoWater };
	float           pendingHeight{ 0.0f };
	float           nearestHeight{ 0.0f };
	bool            hasNearest{ false };

	RE::TESWorldSpace* mapWorld{ nullptr };
	std::int32_t       mapX{ 0 };
	std::int32_t       mapY{ 0 };
	bool               mapValid{ false };
	bool               mapLogged{ false };

	bool UpdateMap(ID3D11DeviceContext* a_context, RE::TESWorldSpace* a_world, std::int32_t a_originX, std::int32_t a_originY)
	{
		if (mapValid && a_world == mapWorld && a_originX == mapX && a_originY == mapY) {
			return true;
		}
		const CellWaterMap::Cells cells(a_world);
		if (!cells) {
			mapValid = false;
			return false;
		}
		std::array<float, kMapCells * kMapCells> values{};
		std::int32_t                             withWater = 0;
		for (std::int32_t j = 0; j < kMapCells; ++j) {
			for (std::int32_t i = 0; i < kMapCells; ++i) {
				const auto cell = cells.At(a_originX + i, a_originY + j);
				values[static_cast<std::size_t>(j) * kMapCells + i] = cell.water ? cell.height : kNoWaterHeight;
				withWater += cell.water ? 1 : 0;
			}
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(resources.heights.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			mapValid = false;
			return false;
		}
		for (std::int32_t j = 0; j < kMapCells; ++j) {
			std::memcpy(static_cast<std::byte*>(mapped.pData) + static_cast<std::size_t>(j) * mapped.RowPitch, values.data() + static_cast<std::size_t>(j) * kMapCells, sizeof(float) * kMapCells);
		}
		a_context->Unmap(resources.heights.Get(), 0);
		mapWorld = a_world;
		mapX = a_originX;
		mapY = a_originY;
		mapValid = true;
		if (!mapLogged) {
			mapLogged = true;
			logger::info("Realistic Distant Water: reflection fade uses the cell water heights of {:08X} ({} of {} cells around the camera have water).", a_world->GetFormID(), withWater, kMapCells * kMapCells);
		}
		return true;
	}

	ComPtr<ID3DBlob> Compile(const char* a_entry, const char* a_target)
	{
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> errors;
		const auto       result = D3DCompile(kShader, sizeof(kShader) - 1, "SsrFade", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
		if (FAILED(result)) {
			logger::error("Realistic Distant Water: could not compile the reflection fade ({}): {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
			return nullptr;
		}
		return code;
	}

	bool CreateResources(ID3D11Device* a_device)
	{
		const auto vertexCode = Compile("VS", "vs_5_0");
		const auto pixelCode = Compile("PS", "ps_5_0");
		if (!vertexCode || !pixelCode) {
			return false;
		}

		D3D11_BLEND_DESC blend{};
		blend.RenderTarget[0].BlendEnable = TRUE;
		blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
		blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;

		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthEnable = FALSE;
		depth.StencilEnable = FALSE;

		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;

		D3D11_BUFFER_DESC constants{};
		constants.ByteWidth = sizeof(Constants);
		constants.Usage = D3D11_USAGE_DYNAMIC;
		constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		D3D11_TEXTURE2D_DESC heights{};
		heights.Width = heights.Height = kMapCells;
		heights.MipLevels = 1;
		heights.ArraySize = 1;
		heights.Format = DXGI_FORMAT_R32_FLOAT;
		heights.SampleDesc.Count = 1;
		heights.Usage = D3D11_USAGE_DYNAMIC;
		heights.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		heights.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		return SUCCEEDED(a_device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, resources.vertex.GetAddressOf())) &&
		       SUCCEEDED(a_device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, resources.pixel.GetAddressOf())) &&
		       SUCCEEDED(a_device->CreateBlendState(&blend, resources.blend.GetAddressOf())) &&
		       SUCCEEDED(a_device->CreateDepthStencilState(&depth, resources.depth.GetAddressOf())) &&
		       SUCCEEDED(a_device->CreateRasterizerState(&raster, resources.raster.GetAddressOf())) &&
		       SUCCEEDED(a_device->CreateBuffer(&constants, nullptr, resources.constants.GetAddressOf())) &&
		       SUCCEEDED(a_device->CreateTexture2D(&heights, nullptr, resources.heights.GetAddressOf())) &&
		       SUCCEEDED(a_device->CreateShaderResourceView(resources.heights.Get(), nullptr, resources.heightsView.GetAddressOf()));
	}

	void FadeTarget(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target)
	{
		ComPtr<ID3D11Resource> resource;
		a_target->GetResource(resource.GetAddressOf());
		ComPtr<ID3D11Texture2D> texture;
		if (FAILED(resource.As(&texture))) {
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f };
		a_context->RSSetViewports(1, &viewport);
		a_context->OMSetRenderTargets(1, &a_target, nullptr);
		a_context->Draw(3, 0);
	}
}

void SsrFade::NewFrame()
{
	doneThisFrame.store(false, std::memory_order_relaxed);
	if (nearestDistance < kNoWater) {
		nearestHeight = pendingHeight;
		hasNearest = true;
		nearestDistance = kNoWater;
	}
}

void SsrFade::NoteWater(float a_distance, float a_height)
{
	if (a_distance < nearestDistance) {
		nearestDistance = a_distance;
		pendingHeight = a_height;
	}
}

void SsrFade::Apply(float a_waterHeight)
{
	if (resources.failed || doneThisFrame.exchange(true, std::memory_order_relaxed)) {
		return;
	}
	auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto* camera = RE::Main::WorldRootCamera();
	if (!renderer || !camera) {
		return;
	}
	auto& data = renderer->GetRuntimeData();
	auto* context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
	auto* device = reinterpret_cast<ID3D11Device*>(data.forwarder);
	auto* depthView = reinterpret_cast<ID3D11ShaderResourceView*>(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV);
	auto* reflections = reinterpret_cast<ID3D11RenderTargetView*>(data.renderTargets[RE::RENDER_TARGETS::kSSR].RTV);
	auto* rawReflections = reinterpret_cast<ID3D11RenderTargetView*>(data.renderTargets[RE::RENDER_TARGETS::kSSR_RAW].RTV);
	auto* blurredReflections = reinterpret_cast<ID3D11RenderTargetView*>(data.renderTargets[RE::RENDER_TARGETS::kSSR_BLURRED0].RTV);
	if (!context || !device || !depthView || (!reflections && !rawReflections)) {
		return;
	}
	if (!resources.ready) {
		if (!CreateResources(device)) {
			logger::error("Realistic Distant Water: could not create the reflection fade; far reflections stay at full strength.");
			resources.failed = true;
			return;
		}
		resources.ready = true;
		logger::info("Realistic Distant Water: reflection fade ready (reflections {}, raw {}).", fmt::ptr(reflections), fmt::ptr(rawReflections));
	}

	auto*              tes = RE::TES::GetSingleton();
	RE::TESWorldSpace* world = tes && !tes->interiorCell ? tes->GetRuntimeData2().worldSpace : nullptr;
	const std::int32_t originX = static_cast<std::int32_t>(std::floor(camera->world.translate.x / kCellSize)) - kMapCells / 2;
	const std::int32_t originY = static_cast<std::int32_t>(std::floor(camera->world.translate.y / kCellSize)) - kMapCells / 2;
	const bool         useMap = world && UpdateMap(context, world, originX, originY);

	const auto&              frustum = camera->GetRuntimeData2().viewFrustum;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(resources.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		return;
	}
	const auto& rotate = camera->world.rotate;
	const auto& eye = camera->world.translate;
	const float height = eye.z - (hasNearest ? nearestHeight : a_waterHeight);
	const Constants constants{
		frustum.fNear * frustum.fFar, frustum.fFar, frustum.fFar - frustum.fNear, height,
		{ rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0], frustum.fLeft },
		{ rotate.entry[0][2], rotate.entry[1][2], rotate.entry[2][2], frustum.fRight },
		{ rotate.entry[0][1], rotate.entry[1][1], rotate.entry[2][1], frustum.fTop },
		{ eye.x, eye.y, eye.z, useMap ? 1.0f : 0.0f },
		{ static_cast<float>(originX) * kCellSize, static_cast<float>(originY) * kCellSize, static_cast<float>(kMapCells), kCellSize }
	};
	std::memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(resources.constants.Get(), 0);

	ComPtr<ID3D11RenderTargetView>   oldTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ComPtr<ID3D11DepthStencilView>   oldDepthTarget;
	ComPtr<ID3D11BlendState>         oldBlend;
	FLOAT                            oldBlendFactor[4]{};
	UINT                             oldSampleMask = 0;
	ComPtr<ID3D11DepthStencilState>  oldDepth;
	UINT                             oldStencilRef = 0;
	ComPtr<ID3D11RasterizerState>    oldRaster;
	UINT                             oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT                   oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	D3D11_PRIMITIVE_TOPOLOGY         oldTopology{};
	ComPtr<ID3D11InputLayout>        oldLayout;
	ComPtr<ID3D11VertexShader>       oldVertex;
	ComPtr<ID3D11PixelShader>        oldPixel;
	ComPtr<ID3D11GeometryShader>     oldGeometry;
	ID3D11ShaderResourceView*        oldTextures[16]{};
	ComPtr<ID3D11Buffer>             oldConstants;

	ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, oldDepthTarget.GetAddressOf());
	for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		oldTargets[i].Attach(targets[i]);
	}
	context->OMGetBlendState(oldBlend.GetAddressOf(), oldBlendFactor, &oldSampleMask);
	context->OMGetDepthStencilState(oldDepth.GetAddressOf(), &oldStencilRef);
	context->RSGetState(oldRaster.GetAddressOf());
	context->RSGetViewports(&oldViewportCount, oldViewports);
	context->IAGetPrimitiveTopology(&oldTopology);
	context->IAGetInputLayout(oldLayout.GetAddressOf());
	context->VSGetShader(oldVertex.GetAddressOf(), nullptr, nullptr);
	context->PSGetShader(oldPixel.GetAddressOf(), nullptr, nullptr);
	context->GSGetShader(oldGeometry.GetAddressOf(), nullptr, nullptr);
	context->PSGetShaderResources(0, 16, oldTextures);
	context->PSGetConstantBuffers(0, 1, oldConstants.GetAddressOf());

	ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
	context->OMSetBlendState(resources.blend.Get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(resources.depth.Get(), 0);
	context->RSSetState(resources.raster.Get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetInputLayout(nullptr);
	context->VSSetShader(resources.vertex.Get(), nullptr, 0);
	context->PSSetShader(resources.pixel.Get(), nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	ID3D11ShaderResourceView* inputs[2]{ depthView, resources.heightsView.Get() };
	context->PSSetShaderResources(0, 2, inputs);
	context->PSSetConstantBuffers(0, 1, resources.constants.GetAddressOf());

	if (reflections) {
		FadeTarget(context, reflections);
	}
	if (rawReflections) {
		FadeTarget(context, rawReflections);
	}
	if (blurredReflections) {
		FadeTarget(context, blurredReflections);
	}

	ID3D11ShaderResourceView* noTextures[2]{};
	context->PSSetShaderResources(0, 2, noTextures);
	ID3D11RenderTargetView* restored[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
	for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
		restored[i] = oldTargets[i].Get();
	}
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, restored, oldDepthTarget.Get());
	context->OMSetBlendState(oldBlend.Get(), oldBlendFactor, oldSampleMask);
	context->OMSetDepthStencilState(oldDepth.Get(), oldStencilRef);
	context->RSSetState(oldRaster.Get());
	context->RSSetViewports(oldViewportCount, oldViewports);
	context->IASetPrimitiveTopology(oldTopology);
	context->IASetInputLayout(oldLayout.Get());
	context->VSSetShader(oldVertex.Get(), nullptr, 0);
	context->PSSetShader(oldPixel.Get(), nullptr, 0);
	context->GSSetShader(oldGeometry.Get(), nullptr, 0);
	context->PSSetShaderResources(0, 16, oldTextures);
	for (auto* texture : oldTextures) {
		if (texture) {
			texture->Release();
		}
	}
	ID3D11Buffer* oldConstantBuffer = oldConstants.Get();
	context->PSSetConstantBuffers(0, 1, &oldConstantBuffer);
}
