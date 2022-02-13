#include "pch.h"
#include "Renderer.h"
#include "App.h"
#include "Utils.h"
#include "StrUtils.h"
#include <VertexTypes.h>
#include "EffectCompiler.h"
#include <rapidjson/document.h>
#include "FrameSourceBase.h"


extern std::shared_ptr<spdlog::logger> logger;


bool Renderer::Initialize() {
	if (!GetWindowRect(App::GetInstance().GetHwndSrc(), &_srcWndRect)) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("GetWindowRect 失败"));
		return false;
	}

	if (!_InitD3D()) {
		SPDLOG_LOGGER_ERROR(logger, "_InitD3D 失败");
		return false;
	}

	if (!_CreateSwapChain()) {
		SPDLOG_LOGGER_ERROR(logger, "_CreateSwapChain 失败");
		return false;
	}

	_gpuTimer.ResetElapsedTime();

	return true;
}

bool Renderer::InitializeEffectsAndCursor(const std::string& effectsJson) {
	RECT destRect;
	if (!_ResolveEffectsJson(effectsJson, destRect)) {
		SPDLOG_LOGGER_ERROR(logger, "_ResolveEffectsJson 失败");
		return false;
	}
	
	if (App::GetInstance().IsShowFPS()) {
		if (!_frameRateDrawer.Initialize(_backBuffer, destRect)) {
			SPDLOG_LOGGER_ERROR(logger, "初始化 FrameRateDrawer 失败");
			return false;
		}
	}

	if (!_cursorDrawer.Initialize(_backBuffer, destRect)) {
		SPDLOG_LOGGER_ERROR(logger, "初始化 CursorDrawer 失败");
		return false;
	}

	return true;
}


void Renderer::Render() {
	if (!_waitingForNextFrame) {
		WaitForSingleObjectEx(_frameLatencyWaitableObject.get(), 1000, TRUE);
	}

	if (!_CheckSrcState()) {
		SPDLOG_LOGGER_INFO(logger, "源窗口状态改变，退出全屏");
		App::GetInstance().Quit();
		return;
	}

	auto state = App::GetInstance().GetFrameSource().Update();
	_waitingForNextFrame = state == FrameSourceBase::UpdateState::Waiting
		|| state == FrameSourceBase::UpdateState::Error;
	if (_waitingForNextFrame) {
		return;
	}

	_gpuTimer.BeginFrame();

	_d3dDC->ClearState();
	// 所有渲染都使用三角形带拓扑
	_d3dDC->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	if (!_cursorDrawer.Update()) {
		SPDLOG_LOGGER_ERROR(logger, "更新光标位置失败");
	}

	// 更新常量
	if (!EffectDrawer::UpdateExprDynamicVars()) {
		SPDLOG_LOGGER_ERROR(logger, "UpdateExprDynamicVars 失败");
	}

	if (state == FrameSourceBase::UpdateState::NewFrame) {
		for (EffectDrawer& effect : _effects) {
			effect.Draw();
		}
	} else {
		// 此帧内容无变化
		// 从第一个有动态常量的 Effect 开始渲染
		// 如果没有则只渲染最后一个 Effect 的最后一个 pass

		size_t i = 0;
		for (; i < _effects.size(); ++i) {
			if (_effects[i].HasDynamicConstants()) {
				break;
			}
		}

		if (i == _effects.size()) {
			// 只渲染最后一个 Effect 的最后一个 pass
			_effects.back().Draw(true);
		} else {
			for (; i < _effects.size(); ++i) {
				_effects[i].Draw();
			}
		}
	}

	if (App::GetInstance().IsShowFPS()) {
		_frameRateDrawer.Draw();
	}

	_cursorDrawer.Draw();

	if (App::GetInstance().IsDisableVSync()) {
		_dxgiSwapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
	} else {
		_dxgiSwapChain->Present(1, 0);
	}
}

bool Renderer::GetRenderTargetView(ID3D11Texture2D* texture, ID3D11RenderTargetView** result) {
	auto it = _rtvMap.find(texture);
	if (it != _rtvMap.end()) {
		*result = it->second.get();
		return true;
	}

	winrt::com_ptr<ID3D11RenderTargetView>& r = _rtvMap[texture];
	HRESULT hr = _d3dDevice->CreateRenderTargetView(texture, nullptr, r.put());
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateRenderTargetView 失败", hr));
		return false;
	} else {
		*result = r.get();
		return true;
	}
}

bool Renderer::GetShaderResourceView(ID3D11Texture2D* texture, ID3D11ShaderResourceView** result) {
	auto it = _srvMap.find(texture);
	if (it != _srvMap.end()) {
		*result = it->second.get();
		return true;
	}

	winrt::com_ptr<ID3D11ShaderResourceView>& r = _srvMap[texture];
	HRESULT hr = _d3dDevice->CreateShaderResourceView(texture, nullptr, r.put());
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateShaderResourceView 失败", hr));
		return false;
	} else {
		*result = r.get();
		return true;
	}
}

bool Renderer::SetFillVS() {
	if (!_fillVS) {
		const char* src = "void m(uint i:SV_VERTEXID,out float4 p:SV_POSITION,out float2 c:TEXCOORD){c=float2(i&1,i>>1)*2;p=float4(c.x*2-1,-c.y*2+1,0,1);}";

		winrt::com_ptr<ID3DBlob> blob;
		if (!CompileShader(true, src, "m", blob.put(), "FillVS")) {
			SPDLOG_LOGGER_ERROR(logger, "编译 FillVS 失败");
			return false;
		}

		HRESULT hr = _d3dDevice->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _fillVS.put());
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 FillVS 失败", hr));
			return false;
		}
	}
	
	_d3dDC->IASetInputLayout(nullptr);
	_d3dDC->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	_d3dDC->VSSetShader(_fillVS.get(), nullptr, 0);

	return true;
}


bool Renderer::SetCopyPS(ID3D11SamplerState* sampler, ID3D11ShaderResourceView* input) {
	if (!_copyPS) {
		const char* src = "Texture2D t:register(t0);SamplerState s:register(s0);float4 m(float4 p:SV_POSITION,float2 c:TEXCOORD):SV_Target{return t.Sample(s,c);}";

		winrt::com_ptr<ID3DBlob> blob;
		if (!CompileShader(false, src, "m", blob.put(), "CopyPS")) {
			SPDLOG_LOGGER_ERROR(logger, "编译 CopyPS 失败");
			return false;
		}

		HRESULT hr = _d3dDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _copyPS.put());
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 CopyPS 失败", hr));
			return false;
		}
	}

	_d3dDC->PSSetShader(_copyPS.get(), nullptr, 0);
	_d3dDC->PSSetConstantBuffers(0, 0, nullptr);
	_d3dDC->PSSetShaderResources(0, 1, &input);
	_d3dDC->PSSetSamplers(0, 1, &sampler);

	return true;
}

bool Renderer::SetSimpleVS(ID3D11Buffer* simpleVB) {
	if (!_simpleVS) {
		const char* src = "void m(float4 p:SV_POSITION,float2 c:TEXCOORD,out float4 q:SV_POSITION,out float2 d:TEXCOORD) {q=p;d=c;}";

		winrt::com_ptr<ID3DBlob> blob;
		if (!CompileShader(true, src, "m", blob.put(), "SimpleVS")) {
			SPDLOG_LOGGER_ERROR(logger, "编译 SimpleVS 失败");
			return false;
		}

		HRESULT hr = _d3dDevice->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _simpleVS.put());
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 SimpleVS 失败", hr));
			return false;
		}

		hr = _d3dDevice->CreateInputLayout(
			VertexPositionTexture::InputElements,
			VertexPositionTexture::InputElementCount,
			blob->GetBufferPointer(),
			blob->GetBufferSize(),
			_simpleIL.put()
		);
		if (FAILED(hr)) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 SimpleVS 输入布局失败", hr));
			return false;
		}
	}

	_d3dDC->IASetInputLayout(_simpleIL.get());

	UINT stride = sizeof(VertexPositionTexture);
	UINT offset = 0;
	_d3dDC->IASetVertexBuffers(0, 1, &simpleVB, &stride, &offset);

	_d3dDC->VSSetShader(_simpleVS.get(), nullptr, 0);

	return true;
}

static inline void LogAdapter(const DXGI_ADAPTER_DESC1& adapterDesc) {
	SPDLOG_LOGGER_INFO(logger, fmt::format("当前图形适配器：\n\tVendorId：{:#x}\n\tDeviceId：{:#x}\n\t描述：{}",
		adapterDesc.VendorId, adapterDesc.DeviceId, StrUtils::UTF16ToUTF8(adapterDesc.Description)));
}

static winrt::com_ptr<IDXGIAdapter1> ObtainGraphicsAdapter(IDXGIFactory4* dxgiFactory, int adapterIdx) {
	winrt::com_ptr<IDXGIAdapter1> adapter;

	if (adapterIdx >= 0) {
		HRESULT hr = dxgiFactory->EnumAdapters1(adapterIdx, adapter.put());
		if (SUCCEEDED(hr)) {
			DXGI_ADAPTER_DESC1 desc;
			HRESULT hr = adapter->GetDesc1(&desc);
			if (FAILED(hr)) {
				return nullptr;
			}

			LogAdapter(desc);
			return adapter;
		}
	}
	
	// 枚举查找第一个支持 D3D11 的图形适配器
	for (UINT adapterIndex = 0;
		SUCCEEDED(dxgiFactory->EnumAdapters1(adapterIndex,adapter.put()));
		++adapterIndex
	) {
		DXGI_ADAPTER_DESC1 desc;
		HRESULT hr = adapter->GetDesc1(&desc);
		if (FAILED(hr)) {
			continue;
		}

		if (desc.Flags == DXGI_ADAPTER_FLAG_SOFTWARE) {
			continue;
		}

		D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
			// 不支持功能级别 9.x，但这里加上没坏处
			D3D_FEATURE_LEVEL_9_3,
			D3D_FEATURE_LEVEL_9_2,
			D3D_FEATURE_LEVEL_9_1,
		};
		UINT nFeatureLevels = ARRAYSIZE(featureLevels);

		hr = D3D11CreateDevice(
			adapter.get(),
			D3D_DRIVER_TYPE_UNKNOWN,
			nullptr,
			0,
			featureLevels,
			nFeatureLevels,
			D3D11_SDK_VERSION,
			nullptr,
			nullptr,
			nullptr
		);
		if (SUCCEEDED(hr)) {
			LogAdapter(desc);
			return adapter;
		}
	}

	// 回落到 Basic Render Driver Adapter（WARP）
	// https://docs.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp
	HRESULT hr = dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 WARP 设备失败", hr));
		return nullptr;
	}

	return adapter;
}

bool Renderer::CompileShader(bool isVS, std::string_view hlsl, const char* entryPoint,
	ID3DBlob** blob, const char* sourceName, ID3DInclude* include
) {
	winrt::com_ptr<ID3DBlob> errorMsgs = nullptr;

	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
	const char* target;
	if (isVS) {
		target = _featureLevel >= D3D_FEATURE_LEVEL_11_0 ? "vs_5_0" :
			(_featureLevel == D3D_FEATURE_LEVEL_10_1 ? "vs_4_1" : "vs_4_0");
	} else {
		target = _featureLevel >= D3D_FEATURE_LEVEL_11_0 ? "ps_5_0" :
			(_featureLevel == D3D_FEATURE_LEVEL_10_1 ? "ps_4_1" : "ps_4_0");
	} 

	HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), sourceName, nullptr, include,
		entryPoint, target, flags, 0, blob, errorMsgs.put());
	if (FAILED(hr)) {
		if (errorMsgs) {
			SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg(fmt::format("编译{}着色器失败：{}",
				isVS ? "顶点" : "像素", (const char*)errorMsgs->GetBufferPointer()), hr));
		}
		return false;
	} else {
		if (errorMsgs) {
			// 显示警告消息
			SPDLOG_LOGGER_WARN(logger, fmt::format("编译{}着色器时产生警告：{}",
				isVS ? "顶点" : "像素", (const char*)errorMsgs->GetBufferPointer()));
		}
	}

	return true;
}

bool Renderer::IsDebugLayersAvailable() {
#ifdef _DEBUG
	static std::optional<bool> result = std::nullopt;

	if (!result.has_value()) {
		HRESULT hr = D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_NULL,       // There is no need to create a real hardware device.
			nullptr,
			D3D11_CREATE_DEVICE_DEBUG,  // Check for the SDK layers.
			nullptr,                    // Any feature level will do.
			0,
			D3D11_SDK_VERSION,
			nullptr,                    // No need to keep the D3D device reference.
			nullptr,                    // No need to know the feature level.
			nullptr                     // No need to keep the D3D device context reference.
		);

		result = SUCCEEDED(hr);
	}

	return result.value_or(false);
#else
	// Relaese 配置不使用调试层
	return false;
#endif
}

bool Renderer::_InitD3D() {
#ifdef _DEBUG
	UINT flag = DXGI_CREATE_FACTORY_DEBUG;
#else
	UINT flag = 0;
#endif // _DEBUG
	
	HRESULT hr = CreateDXGIFactory2(flag, IID_PPV_ARGS(_dxgiFactory.put()));
	if (FAILED(hr)) {
		return false;
	}

	// 检查可变帧率支持
	BOOL supportTearing = FALSE;
	winrt::com_ptr<IDXGIFactory5> dxgiFactory5 = _dxgiFactory.try_as<IDXGIFactory5>();
	if (!dxgiFactory5) {
		SPDLOG_LOGGER_WARN(logger, "获取 IDXGIFactory5 失败");
	} else {
		hr = dxgiFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supportTearing, sizeof(supportTearing));
		if (FAILED(hr)) {
			SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("CheckFeatureSupport 失败", hr));
		}
	}
	_supportTearing = !!supportTearing;

	SPDLOG_LOGGER_INFO(logger, fmt::format("可变刷新率支持：{}", supportTearing ? "是" : "否"));

	if (App::GetInstance().IsDisableVSync() && !supportTearing) {
		SPDLOG_LOGGER_ERROR(logger, "当前显示器不支持可变刷新率");
		App::GetInstance().SetErrorMsg(ErrorMessages::VSYNC_OFF_NOT_SUPPORTED);
		return false;
	}

	UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	if (IsDebugLayersAvailable()) {
		// 在 DEBUG 配置启用调试层
		createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	}

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		// 不支持功能级别 9.x，但这里加上没坏处
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1,
	};
	UINT nFeatureLevels = ARRAYSIZE(featureLevels);

	_graphicsAdapter = ObtainGraphicsAdapter(_dxgiFactory.get(), App::GetInstance().GetAdapterIdx());
	if (!_graphicsAdapter) {
		SPDLOG_LOGGER_ERROR(logger, "找不到可用 Adapter");
		return false;
	}

	winrt::com_ptr<ID3D11Device> d3dDevice;
	winrt::com_ptr<ID3D11DeviceContext> d3dDC;
	hr = D3D11CreateDevice(
		_graphicsAdapter.get(),
		D3D_DRIVER_TYPE_UNKNOWN,
		nullptr,
		createDeviceFlags,
		featureLevels,
		nFeatureLevels,
		D3D11_SDK_VERSION,
		d3dDevice.put(),
		&_featureLevel,
		d3dDC.put()
	);

	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("D3D11CreateDevice 失败", hr));
		return false;
	}

	std::string_view fl;
	switch (_featureLevel) {
	case D3D_FEATURE_LEVEL_11_1:
		fl = "11.1";
		break;
	case D3D_FEATURE_LEVEL_11_0:
		fl = "11.0";
		break;
	case D3D_FEATURE_LEVEL_10_1:
		fl = "10.1";
		break;
	case D3D_FEATURE_LEVEL_10_0:
		fl = "10.0";
		break;
	case D3D_FEATURE_LEVEL_9_3:
		fl = "9.3";
		break;
	case D3D_FEATURE_LEVEL_9_2:
		fl = "9.2";
		break;
	case D3D_FEATURE_LEVEL_9_1:
		fl = "9.1";
		break;
	default:
		fl = "未知";
		break;
	}
	SPDLOG_LOGGER_INFO(logger, fmt::format("已创建 D3D Device\n\t功能级别：{}", fl));

	_d3dDevice = d3dDevice.try_as<ID3D11Device1>();
	if (!_d3dDevice) {
		SPDLOG_LOGGER_ERROR(logger, "获取 ID3D11Device1 失败");
		return false;
	}

	_d3dDC = d3dDC.try_as<ID3D11DeviceContext1>();
	if (!_d3dDC) {
		SPDLOG_LOGGER_ERROR(logger, "获取 ID3D11DeviceContext1 失败");
		return false;
	}

	_dxgiDevice = _d3dDevice.try_as<IDXGIDevice1>();
	if (!_dxgiDevice) {
		SPDLOG_LOGGER_ERROR(logger, "获取 IDXGIDevice 失败");
		return false;
	}

	return true;
}

bool Renderer::_CreateSwapChain() {
	const RECT& hostWndRect = App::GetInstance().GetHostWndRect();

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = hostWndRect.right - hostWndRect.left;
	sd.Height = hostWndRect.bottom - hostWndRect.top;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
	sd.BufferCount = App::GetInstance().IsDisableLowLatency() ? 3 : 2;
	// 使用 DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL 而不是 DXGI_SWAP_EFFECT_FLIP_DISCARD
	// 不渲染四周（可能存在的）黑边，因此必须保证交换链缓冲区不被改变
	// 否则将不得不在每帧渲染前清空后缓冲区，这个操作在一些显卡上比较耗时
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	// 只要显卡支持始终启用 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
	sd.Flags = (_supportTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
		| DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	winrt::com_ptr<IDXGISwapChain1> dxgiSwapChain = nullptr;
	HRESULT hr = _dxgiFactory->CreateSwapChainForHwnd(
		_d3dDevice.get(),
		App::GetInstance().GetHwndHost(),
		&sd,
		nullptr,
		nullptr,
		dxgiSwapChain.put()
	);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建交换链失败", hr));
		return false;
	}

	_dxgiSwapChain = dxgiSwapChain.try_as<IDXGISwapChain2>();
	if (!_dxgiSwapChain) {
		SPDLOG_LOGGER_ERROR(logger, "获取 IDXGISwapChain2 失败");
		return false;
	}

	// 关闭低延迟模式时将最大延迟设为 2 以使 CPU 和 GPU 并行执行
	_dxgiSwapChain->SetMaximumFrameLatency(App::GetInstance().IsDisableLowLatency() ? 2 : 1);

	_frameLatencyWaitableObject.reset(_dxgiSwapChain->GetFrameLatencyWaitableObject());
	if (!_frameLatencyWaitableObject) {
		SPDLOG_LOGGER_ERROR(logger, "GetFrameLatencyWaitableObject 失败");
		return false;
	}

	hr = _dxgiFactory->MakeWindowAssociation(App::GetInstance().GetHwndHost(), DXGI_MWA_NO_ALT_ENTER);
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("MakeWindowAssociation 失败", hr));
	}

	// 检查 Multiplane Overlay 和 Hardware Composition 支持
	BOOL supportMPO = FALSE;
	BOOL supportHardwareComposition = FALSE;
	winrt::com_ptr<IDXGIOutput> output;
	hr = _dxgiSwapChain->GetContainingOutput(output.put());
	if (FAILED(hr)) {
		SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("获取 IDXGIOutput 失败", hr));
	} else {
		winrt::com_ptr<IDXGIOutput2> output2 = output.try_as<IDXGIOutput2>();
		if (!output2) {
			SPDLOG_LOGGER_WARN(logger, "获取 IDXGIOutput2 失败");
		} else {
			supportMPO = output2->SupportsOverlays();
		}

		winrt::com_ptr<IDXGIOutput6> output6 = output.try_as<IDXGIOutput6>();
		if (!output6) {
			SPDLOG_LOGGER_WARN(logger, "获取 IDXGIOutput6 失败");
		} else {
			UINT flags;
			hr = output6->CheckHardwareCompositionSupport(&flags);
			if (FAILED(hr)) {
				SPDLOG_LOGGER_WARN(logger, MakeComErrorMsg("CheckHardwareCompositionSupport 失败", hr));
			} else {
				supportHardwareComposition = flags & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED;
			}
		}
	}

	SPDLOG_LOGGER_INFO(logger, fmt::format("Hardware Composition 支持：{}", supportHardwareComposition ? "是" : "否"));
	SPDLOG_LOGGER_INFO(logger, fmt::format("Multiplane Overlay 支持：{}", supportMPO ? "是" : "否"));

	hr = _dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(_backBuffer.put()));
	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("获取后缓冲区失败", hr));
		return false;
	}

	return true;
}

bool CheckForeground(HWND hwndForeground) {
	wchar_t className[256]{};
	if (!GetClassName(hwndForeground, (LPWSTR)className, 256)) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("GetClassName 失败"));
		return false;
	}

	// 排除桌面窗口和 Alt+Tab 窗口
	if (!std::wcscmp(className, L"WorkerW") || !std::wcscmp(className, L"ForegroundStaging") ||
		!std::wcscmp(className, L"MultitaskingViewFrame") || !std::wcscmp(className, L"XamlExplorerHostIslandWindow")
	) {
		return true;
	}

	RECT rectForground{};

	// 如果捕获模式可以捕获到弹窗，则允许小的弹窗
	if (App::GetInstance().GetFrameSource().IsScreenCapture()
		&& GetWindowStyle(hwndForeground) & (WS_POPUP | WS_CHILD)
	) {
		if (!Utils::GetWindowFrameRect(hwndForeground, rectForground)) {
			SPDLOG_LOGGER_ERROR(logger, "GetWindowFrameRect 失败");
			return false;
		}

		// 弹窗如果完全在源窗口客户区内则不退出全屏
		const RECT& srcFrameRect = App::GetInstance().GetFrameSource().GetSrcFrameRect();
		if (rectForground.left >= srcFrameRect.left
			&& rectForground.right <= srcFrameRect.right
			&& rectForground.top >= srcFrameRect.top
			&& rectForground.bottom <= srcFrameRect.bottom
		) {
			return true;
		}
	}

	// 非多屏幕模式下退出全屏
	if (!App::GetInstance().IsMultiMonitorMode()) {
		return false;
	}

	if (rectForground == RECT{}) {
		if (!Utils::GetWindowFrameRect(hwndForeground, rectForground)) {
			SPDLOG_LOGGER_ERROR(logger, "GetWindowFrameRect 失败");
			return false;
		}
	}

	IntersectRect(&rectForground, &App::GetInstance().GetHostWndRect(), &rectForground);

	// 允许稍微重叠，否则前台窗口最大化时会意外退出
	if (rectForground.right - rectForground.left < 10 || rectForground.right - rectForground.top < 10) {
		return true;
	}

	// 排除开始菜单，它的类名是 CoreWindow
	if (std::wcscmp(className, L"Windows.UI.Core.CoreWindow")) {
		// 记录新的前台窗口
		SPDLOG_LOGGER_INFO(logger, fmt::format("新的前台窗口：\n\t类名：{}", StrUtils::UTF16ToUTF8(className)));
		return false;
	}

	DWORD dwProcId = 0;
	if (!GetWindowThreadProcessId(hwndForeground, &dwProcId)) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("GetWindowThreadProcessId 失败"));
		return false;
	}

	Utils::ScopedHandle hProc(Utils::SafeHandle(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dwProcId)));
	if (!hProc) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("OpenProcess 失败"));
		return false;
	}

	wchar_t fileName[MAX_PATH] = { 0 };
	if (!GetModuleFileNameEx(hProc.get(), NULL, fileName, MAX_PATH)) {
		SPDLOG_LOGGER_ERROR(logger, MakeWin32ErrorMsg("GetModuleFileName 失败"));
		return false;
	}

	std::string exeName = StrUtils::UTF16ToUTF8(fileName);
	exeName = exeName.substr(exeName.find_last_of(L'\\') + 1);
	StrUtils::ToLowerCase(exeName);

	// win10: searchapp.exe 和 startmenuexperiencehost.exe
	// win11: searchhost.exe 和 startmenuexperiencehost.exe
	if (exeName == "searchapp.exe" || exeName == "searchhost.exe" || exeName == "startmenuexperiencehost.exe") {
		return true;
	}

	return false;
}

bool Renderer::_CheckSrcState() {
	HWND hwndSrc = App::GetInstance().GetHwndSrc();

	if (!App::GetInstance().IsBreakpointMode()) {
		HWND hwndForeground = GetForegroundWindow();
		if (hwndForeground && hwndForeground != hwndSrc && !CheckForeground(hwndForeground)) {
			SPDLOG_LOGGER_INFO(logger, "前台窗口已改变");
			return false;
		}
	}

	if (Utils::GetWindowShowCmd(hwndSrc) != SW_NORMAL) {
		SPDLOG_LOGGER_INFO(logger, "源窗口显示状态改变");
		return false;
	}

	RECT rect;
	if (!GetWindowRect(hwndSrc, &rect)) {
		SPDLOG_LOGGER_ERROR(logger, "GetWindowRect 失败");
		return false;
	}

	if (_srcWndRect != rect) {
		SPDLOG_LOGGER_INFO(logger, "源窗口位置或大小改变");
		return false;
	}

	return true;
}

bool Renderer::_ResolveEffectsJson(const std::string& effectsJson, RECT& destRect) {
	_effectInput = App::GetInstance().GetFrameSource().GetOutput();
	D3D11_TEXTURE2D_DESC inputDesc;
	_effectInput->GetDesc(&inputDesc);

	const RECT& hostWndRect = App::GetInstance().GetHostWndRect();
	SIZE hostSize = { hostWndRect.right - hostWndRect.left,hostWndRect.bottom - hostWndRect.top };

	rapidjson::Document doc;
	if (doc.Parse(effectsJson.c_str(), effectsJson.size()).HasParseError()) {
		// 解析 json 失败
		SPDLOG_LOGGER_ERROR(logger, fmt::format("解析 json 失败\n\t错误码：{}", doc.GetParseError()));
		return false;
	}

	if (!doc.IsArray()) {
		SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：根元素不为数组");
		return false;
	}

	std::vector<SIZE> texSizes;
	texSizes.push_back({ (LONG)inputDesc.Width, (LONG)inputDesc.Height });

	const auto& effectsArr = doc.GetArray();
	_effects.reserve(effectsArr.Size());
	texSizes.reserve(static_cast<size_t>(effectsArr.Size()) + 1);

	// 不得为空
	if (effectsArr.Empty()) {
		SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：根元素为空");
		return false;
	}

	for (const auto& effectJson : effectsArr) {
		if (!effectJson.IsObject()) {
			SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：根数组中存在非法成员");
			return false;
		}

		EffectDrawer& effect = _effects.emplace_back();

		auto effectName = effectJson.FindMember("effect");
		if (effectName == effectJson.MemberEnd() || !effectName->value.IsString()) {
			SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：未找到 effect 属性或该属性的值不合法");
			return false;
		}

		if (!effect.Initialize((L"effects\\" + StrUtils::UTF8ToUTF16(effectName->value.GetString()) + L".hlsl").c_str())) {
			SPDLOG_LOGGER_ERROR(logger, fmt::format("初始化效果 {} 失败", effectName->value.GetString()));
			return false;
		}

		if (effect.CanSetOutputSize()) {
			// scale 属性可用
			auto scaleProp = effectJson.FindMember("scale");
			if (scaleProp != effectJson.MemberEnd()) {
				if (!scaleProp->value.IsArray()) {
					SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：非法的 scale 属性");
					return false;
				}

				// scale 属性的值为两个元素组成的数组
				// [+, +]：缩放比例
				// [0, 0]：非等比例缩放到屏幕大小
				// [-, -]：相对于屏幕能容纳的最大等比缩放的比例

				const auto& scale = scaleProp->value.GetArray();
				if (scale.Size() != 2 || !scale[0].IsNumber() || !scale[1].IsNumber()) {
					SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：非法的 scale 属性");
					return false;
				}

				float scaleX = scale[0].GetFloat();
				float scaleY = scale[1].GetFloat();

				static float DELTA = 1e-5f;

				SIZE outputSize = texSizes.back();;

				if (scaleX >= DELTA) {
					if (scaleY < DELTA) {
						SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：非法的 scale 属性");
						return false;
					}

					outputSize = { std::lroundf(outputSize.cx * scaleX), std::lroundf(outputSize.cy * scaleY) };
				} else if (std::abs(scaleX) < DELTA) {
					if (std::abs(scaleY) >= DELTA) {
						SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：非法的 scale 属性");
						return false;
					}

					outputSize = hostSize;
				} else {
					if (scaleY > -DELTA) {
						SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：非法的 scale 属性");
						return false;
					}

					float fillScale = std::min(float(hostSize.cx) / outputSize.cx, float(hostSize.cy) / outputSize.cy);
					outputSize = {
						std::lroundf(outputSize.cx * fillScale * -scaleX),
						std::lroundf(outputSize.cy * fillScale * -scaleY)
					};
				}

				effect.SetOutputSize(outputSize);
			}
		}

#pragma push_macro("GetObject")
#undef GetObject
		for (const auto& prop : effectJson.GetObject()) {
#pragma pop_macro("GetObject")
			if (!prop.name.IsString()) {
				SPDLOG_LOGGER_ERROR(logger, "解析 json 失败：非法的效果名");
				return false;
			}

			std::string_view name = prop.name.GetString();

			if (name == "effect" || (effect.CanSetOutputSize() && name == "scale")) {
				continue;
			} else {
				auto type = effect.GetConstantType(name);
				if (type == EffectDrawer::ConstantType::Float) {
					if (!prop.value.IsNumber()) {
						SPDLOG_LOGGER_ERROR(logger, fmt::format("解析 json 失败：成员 {} 的类型非法", name));
						return false;
					}

					if (!effect.SetConstant(name, prop.value.GetFloat())) {
						SPDLOG_LOGGER_ERROR(logger, fmt::format("解析 json 失败：成员 {} 的值非法", name));
						return false;
					}
				} else if (type == EffectDrawer::ConstantType::Int) {
					int value;
					if (prop.value.IsInt()) {
						value = prop.value.GetInt();
					} else if (prop.value.IsBool()) {
						// bool 值视为 int
						value = (int)prop.value.GetBool();
					} else {
						SPDLOG_LOGGER_ERROR(logger, fmt::format("解析 json 失败：成员 {} 的类型非法", name));
						return false;
					}

					if (!effect.SetConstant(name, value)) {
						SPDLOG_LOGGER_ERROR(logger, fmt::format("解析 json 失败：成员 {} 的值非法", name));
						return false;
					}
				} else {
					SPDLOG_LOGGER_ERROR(logger, fmt::format("解析 json 失败：非法成员 {}", name));
					return false;
				}
			}
		}

		SIZE& outputSize = texSizes.emplace_back();
		if (!effect.CalcOutputSize(texSizes[texSizes.size() - 2], outputSize)) {
			SPDLOG_LOGGER_ERROR(logger, "CalcOutputSize 失败");
			return false;
		}
	}

	if (_effects.size() == 1) {
		if (!_effects.back().Build(_effectInput, _backBuffer)) {
			SPDLOG_LOGGER_ERROR(logger, "构建效果失败");
			return false;
		}
	} else {
		// 创建效果间的中间纹理
		winrt::com_ptr<ID3D11Texture2D> curTex = _effectInput;

		D3D11_TEXTURE2D_DESC desc{};
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		assert(texSizes.size() == _effects.size() + 1);
		for (size_t i = 0, end = _effects.size() - 1; i < end; ++i) {
			SIZE texSize = texSizes[i + 1];
			desc.Width = texSize.cx;
			desc.Height = texSize.cy;

			winrt::com_ptr<ID3D11Texture2D> outputTex;
			HRESULT hr = _d3dDevice->CreateTexture2D(&desc, nullptr, outputTex.put());
			if (FAILED(hr)) {
				SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("CreateTexture2D 失败", hr));
				return false;
			}

			if (!_effects[i].Build(curTex, outputTex)) {
				SPDLOG_LOGGER_ERROR(logger, "构建效果失败");
				return false;
			}

			curTex = outputTex;
		}

		// 最后一个效果输出到后缓冲纹理
		if (!_effects.back().Build(curTex, _backBuffer)) {
			SPDLOG_LOGGER_ERROR(logger, "构建效果失败");
			return false;
		}
	}

	SIZE outputSize = texSizes.back();
	destRect.left = (hostSize.cx - outputSize.cx) / 2;
	destRect.right = destRect.left + outputSize.cx;
	destRect.top = (hostSize.cy - outputSize.cy) / 2;
	destRect.bottom = destRect.top + outputSize.cy;

	return true;
}

bool Renderer::SetAlphaBlend(bool enable) {
	if (!enable) {
		_d3dDC->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		return true;
	}
	
	if (!_alphaBlendState) {
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = TRUE;
		desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		HRESULT hr = _d3dDevice->CreateBlendState(&desc, _alphaBlendState.put());
		if (FAILED(hr)) {
			SPDLOG_LOGGER_CRITICAL(logger, MakeComErrorMsg("CreateBlendState 失败", hr));
			return false;
		}
	}
	
	_d3dDC->OMSetBlendState(_alphaBlendState.get(), nullptr, 0xffffffff);
	return true;
}

bool Renderer::GetSampler(EffectSamplerFilterType filterType, EffectSamplerAddressType addressType, ID3D11SamplerState** result) {
	winrt::com_ptr<ID3D11SamplerState>* sampler;
	D3D11_TEXTURE_ADDRESS_MODE addressMode;
	D3D11_FILTER filter;

	if (filterType == EffectSamplerFilterType::Linear) {
		filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		if (addressType == EffectSamplerAddressType::Clamp) {
			sampler = &_linearClampSampler;
			addressMode = D3D11_TEXTURE_ADDRESS_CLAMP;
		} else {
			sampler = &_linearWrapSampler;
			addressMode = D3D11_TEXTURE_ADDRESS_WRAP;
		}
	} else {
		filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		if (addressType == EffectSamplerAddressType::Clamp) {
			sampler = &_pointClampSampler;
			addressMode = D3D11_TEXTURE_ADDRESS_CLAMP;
		} else {
			sampler = &_pointWrapSampler;
			addressMode = D3D11_TEXTURE_ADDRESS_WRAP;
		}
	}
	
	if (*sampler) {
		*result = sampler->get();
		return true;
	}

	D3D11_SAMPLER_DESC desc{};
	desc.Filter = filter;
	desc.AddressU = addressMode;
	desc.AddressV = addressMode;
	desc.AddressW = addressMode;
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MinLOD = 0;
	desc.MaxLOD = 0;
	HRESULT hr = _d3dDevice->CreateSamplerState(&desc, sampler->put());

	if (FAILED(hr)) {
		SPDLOG_LOGGER_ERROR(logger, MakeComErrorMsg("创建 ID3D11SamplerState 出错", hr));
		return false;
	}

	*result = sampler->get();
	return true;
}
