#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

constexpr wchar_t kPanelClass[] = L"VervormdeSkermNativePanel";
constexpr wchar_t kOverlayClass[] = L"VervormdeSkermNativeOverlay";
constexpr wchar_t kAppTitle[] = L"Vervormde Skerm Native · DirectX";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kBypassMessage = WM_APP + 2;
constexpr UINT_PTR kUiTimer = 1;
constexpr DWORD kWatchdogMissLimit = 12;
constexpr ULONGLONG kSafetyTimeoutMs = 15ULL * 60ULL * 1000ULL;

enum ControlId : int {
  IDC_STATUS = 100,
  IDC_METRICS,
  IDC_TOGGLE,
  IDC_K1_LABEL,
  IDC_K1_DOWN,
  IDC_K1_UP,
  IDC_K2_LABEL,
  IDC_K2_DOWN,
  IDC_K2_UP,
  IDC_ZOOM_LABEL,
  IDC_ZOOM_DOWN,
  IDC_ZOOM_UP,
  IDC_CENTER_LABEL,
  IDC_CENTER_UP,
  IDC_CENTER_LEFT,
  IDC_CENTER_RIGHT,
  IDC_CENTER_DOWN,
  IDC_GRID,
  IDC_FPS,
  IDC_PRESET_BARREL,
  IDC_PRESET_PINCUSHION,
  IDC_SAVE,
  IDC_RESET,
  IDC_EXIT,
  IDM_TRAY_SHOW = 300,
  IDM_TRAY_TOGGLE,
  IDM_TRAY_EXIT,
};

enum HotkeyId : int {
  HOTKEY_EXIT_FALLBACK = 400,
  HOTKEY_TOGGLE,
};

struct Settings {
  float k1 = 0.0f;
  float k2 = 0.0f;
  float zoom = 1.0f;
  float centerX = 0.5f;
  float centerY = 0.5f;
  bool grid = true;
  bool enabled = false;
  int fpsLimit = 30;
};

struct alignas(16) ShaderParams {
  float resolution[2]{};
  float center[2]{};
  float k1 = 0.0f;
  float k2 = 0.0f;
  float zoom = 1.0f;
  float grid = 1.0f;
  float rotation = 0.0f;
  float padding[3]{};
};

struct alignas(8) WatchdogShared {
  volatile LONG enabled = 0;
  volatile LONG shutdown = 0;
  volatile LONG64 heartbeat = 0;
};

struct RuntimeMetrics {
  double fps = 0.0;
  double cpuPercent = 0.0;
  SIZE_T workingSet = 0;
  SIZE_T privateBytes = 0;
};

HWND gPanel = nullptr;
HWND gOverlay = nullptr;
HFONT gFont = nullptr;
HBRUSH gPanelBrush = nullptr;
HMONITOR gMonitor = nullptr;
RECT gMonitorRect{};
Settings gSettings;
RuntimeMetrics gMetrics;
bool gRunning = true;
bool gQuitting = false;
bool gNeedsRedraw = true;
bool gPendingOverlayShow = false;
bool gSmokeTest = false;
bool gWatchdogTest = false;
int gExitCode = 0;
ULONGLONG gSafetyDeadline = 0;
NOTIFYICONDATAW gTray{};
std::wstring gSettingsPath;
std::wstring gSmokeLogPath;
HANDLE gInstanceMutex = nullptr;
HANDLE gWatchdogMapping = nullptr;
HANDLE gWatchdogProcess = nullptr;
WatchdogShared* gWatchdogState = nullptr;
HHOOK gKeyboardHook = nullptr;

uint64_t FileTimeValue(const FILETIME& value) {
  ULARGE_INTEGER converted{};
  converted.LowPart = value.dwLowDateTime;
  converted.HighPart = value.dwHighDateTime;
  return converted.QuadPart;
}

std::wstring HrText(HRESULT hr) {
  wchar_t buffer[64]{};
  swprintf_s(buffer, L"HRESULT 0x%08X", static_cast<unsigned int>(hr));
  return buffer;
}

std::wstring GetExecutablePath() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  return length == 0 ? std::wstring{} : std::wstring(buffer.data(), length);
}

std::wstring GetLocalDataDirectory() {
  PWSTR rawPath = nullptr;
  std::wstring result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath))) {
    result = rawPath;
    CoTaskMemFree(rawPath);
  }
  if (result.empty()) {
    wchar_t fallback[MAX_PATH]{};
    GetTempPathW(MAX_PATH, fallback);
    result = fallback;
  }
  result += L"\\VervormdeSkerm";
  CreateDirectoryW(result.c_str(), nullptr);
  return result;
}

float ReadIniFloat(const wchar_t* key, float fallback) {
  wchar_t fallbackText[32]{};
  wchar_t value[32]{};
  swprintf_s(fallbackText, L"%.6f", fallback);
  GetPrivateProfileStringW(L"undistort", key, fallbackText, value, 32, gSettingsPath.c_str());
  wchar_t* end = nullptr;
  const float parsed = wcstof(value, &end);
  return end == value || !std::isfinite(parsed) ? fallback : parsed;
}

void LoadSettings() {
  gSettings.k1 = std::clamp(ReadIniFloat(L"k1", 0.0f), -2.0f, 2.0f);
  gSettings.k2 = std::clamp(ReadIniFloat(L"k2", 0.0f), -3.0f, 3.0f);
  gSettings.zoom = std::clamp(ReadIniFloat(L"zoom", 1.0f), 0.70f, 1.40f);
  gSettings.centerX = std::clamp(ReadIniFloat(L"centerX", 0.5f), 0.20f, 0.80f);
  gSettings.centerY = std::clamp(ReadIniFloat(L"centerY", 0.5f), 0.20f, 0.80f);
  gSettings.grid = GetPrivateProfileIntW(L"undistort", L"grid", 1, gSettingsPath.c_str()) != 0;
  const int fps = GetPrivateProfileIntW(L"performance", L"fpsLimit", 30, gSettingsPath.c_str());
  gSettings.fpsLimit = fps >= 60 ? 60 : 30;
  gSettings.enabled = false;
}

void WriteIniFloat(const wchar_t* key, float value) {
  wchar_t text[32]{};
  swprintf_s(text, L"%.6f", value);
  WritePrivateProfileStringW(L"undistort", key, text, gSettingsPath.c_str());
}

void SaveSettings() {
  if (gSettingsPath.empty()) return;
  WriteIniFloat(L"k1", gSettings.k1);
  WriteIniFloat(L"k2", gSettings.k2);
  WriteIniFloat(L"zoom", gSettings.zoom);
  WriteIniFloat(L"centerX", gSettings.centerX);
  WriteIniFloat(L"centerY", gSettings.centerY);
  WritePrivateProfileStringW(L"undistort", L"grid", gSettings.grid ? L"1" : L"0", gSettingsPath.c_str());
  WritePrivateProfileStringW(L"performance", L"fpsLimit", gSettings.fpsLimit == 60 ? L"60" : L"30", gSettingsPath.c_str());
}

bool ExcludeWindowFromCapture(HWND window) {
  if (SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) return true;
  return SetWindowDisplayAffinity(window, WDA_MONITOR) != FALSE;
}

class Renderer {
 public:
  bool Initialize(HWND window, HMONITOR monitor, int width, int height) {
    width_ = width;
    height_ = height;

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) return Fail(L"无法创建 DXGI Factory", hr);

    for (UINT adapterIndex = 0;; ++adapterIndex) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory_->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      for (UINT outputIndex = 0;; ++outputIndex) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop && desc.Monitor == monitor) {
          adapter_ = adapter;
          output_ = output;
          rotation_ = RotationValue(desc.Rotation);
          break;
        }
      }
      if (output_) break;
    }
    if (!adapter_ || !output_) {
      lastError_ = L"找不到鼠标所在显示器对应的 DXGI 输出。";
      return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL actualLevel{};
    hr = D3D11CreateDevice(
      adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
      &device_, &actualLevel, &context_);
    if (hr == E_INVALIDARG) {
      hr = D3D11CreateDevice(
        adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION,
        &device_, &actualLevel, &context_);
    }
    if (FAILED(hr)) return Fail(L"无法创建 Direct3D 11 设备", hr);

    hr = output_.As(&output1_);
    if (FAILED(hr)) return Fail(L"显示器不支持 Desktop Duplication", hr);
    if (!CreateDuplication()) return false;

    ComPtr<IDXGIFactory2> factory2;
    hr = factory_.As(&factory2);
    if (FAILED(hr)) return Fail(L"系统缺少 DXGI 1.2", hr);

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = static_cast<UINT>(width);
    swapDesc.Height = static_cast<UINT>(height);
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.Scaling = DXGI_SCALING_STRETCH;
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory2->CreateSwapChainForHwnd(device_.Get(), window, &swapDesc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) return Fail(L"无法创建 DXGI Swap Chain", hr);
    factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return Fail(L"无法取得 Swap Chain 后台缓冲", hr);
    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
    if (FAILED(hr)) return Fail(L"无法创建渲染目标", hr);

    if (!CreateShaders()) return false;

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(ShaderParams);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&bufferDesc, nullptr, &constantBuffer_);
    if (FAILED(hr)) return Fail(L"无法创建着色器参数缓冲", hr);

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&samplerDesc, &sampler_);
    if (FAILED(hr)) return Fail(L"无法创建纹理采样器", hr);

    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;
    return true;
  }

  bool Tick(const Settings& settings, bool forceRedraw, bool& drewFrame) {
    drewFrame = false;
    bool acquiredFrame = false;
    if (!AcquireDesktopFrame(acquiredFrame)) return false;
    if (!hasFrame_ || (!acquiredFrame && !forceRedraw)) return true;

    ShaderParams params{};
    params.resolution[0] = static_cast<float>(width_);
    params.resolution[1] = static_cast<float>(height_);
    params.center[0] = settings.centerX;
    params.center[1] = settings.centerY;
    params.k1 = settings.k1;
    params.k2 = settings.k2;
    params.zoom = settings.zoom;
    params.grid = settings.grid ? 1.0f : 0.0f;
    params.rotation = rotation_;
    context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &params, 0, 0);

    ID3D11RenderTargetView* targets[] = {renderTarget_.Get()};
    ID3D11ShaderResourceView* resources[] = {captureView_.Get()};
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    ID3D11Buffer* constants[] = {constantBuffer_.Get()};
    context_->OMSetRenderTargets(1, targets, nullptr);
    context_->RSSetViewports(1, &viewport_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, resources);
    context_->PSSetSamplers(0, 1, samplers);
    context_->PSSetConstantBuffers(0, 1, constants);
    context_->Draw(3, 0);

    const HRESULT presentResult = swapChain_->Present(1, 0);
    ID3D11ShaderResourceView* empty[] = {nullptr};
    context_->PSSetShaderResources(0, 1, empty);
    if (FAILED(presentResult)) return Fail(L"DirectX Present 失败", presentResult);
    ++framesRendered_;
    drewFrame = true;
    return true;
  }

  const std::wstring& LastError() const { return lastError_; }
  uint64_t FramesRendered() const { return framesRendered_; }

 private:
  bool CreateDuplication() {
    duplication_.Reset();
    const HRESULT hr = output1_->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) return Fail(L"无法启动 Desktop Duplication；远程桌面或受保护内容可能阻止捕获", hr);
    return true;
  }

  bool AcquireDesktopFrame(bool& acquired) {
    acquired = false;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(0, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return true;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
      captureTexture_.Reset();
      captureView_.Reset();
      hasFrame_ = false;
      return CreateDuplication();
    }
    if (FAILED(hr)) return Fail(L"Desktop Duplication 取帧失败", hr);

    ComPtr<ID3D11Texture2D> source;
    hr = resource.As(&source);
    if (SUCCEEDED(hr)) {
      D3D11_TEXTURE2D_DESC sourceDesc{};
      source->GetDesc(&sourceDesc);
      if (!captureTexture_ || sourceDesc.Width != captureWidth_ || sourceDesc.Height != captureHeight_ || sourceDesc.Format != captureFormat_) {
        captureTexture_.Reset();
        captureView_.Reset();
        D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.SampleDesc.Count = 1;
        copyDesc.SampleDesc.Quality = 0;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        hr = device_->CreateTexture2D(&copyDesc, nullptr, &captureTexture_);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(captureTexture_.Get(), nullptr, &captureView_);
        if (SUCCEEDED(hr)) {
          captureWidth_ = sourceDesc.Width;
          captureHeight_ = sourceDesc.Height;
          captureFormat_ = sourceDesc.Format;
        }
      }
      if (SUCCEEDED(hr)) {
        context_->CopyResource(captureTexture_.Get(), source.Get());
        hasFrame_ = true;
        acquired = true;
      }
    }
    const HRESULT releaseResult = duplication_->ReleaseFrame();
    if (FAILED(hr)) return Fail(L"无法创建桌面 GPU 纹理", hr);
    if (FAILED(releaseResult)) return Fail(L"Desktop Duplication 释放帧失败", releaseResult);
    return true;
  }

  bool CreateShaders() {
    static constexpr char shaderSource[] = R"HLSL(
      cbuffer Params : register(b0) {
        float2 resolution;
        float2 center;
        float k1;
        float k2;
        float zoom;
        float grid;
        float rotation;
        float3 padding;
      };

      Texture2D desktopTexture : register(t0);
      SamplerState desktopSampler : register(s0);

      struct VertexOutput {
        float4 position : SV_POSITION;
        float2 uv : TEXCOORD0;
      };

      VertexOutput VSMain(uint vertexId : SV_VertexID) {
        VertexOutput output;
        float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
        output.uv = uv;
        output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
        return output;
      }

      float2 RotateForCapture(float2 uv) {
        if (rotation < 0.5) return uv;
        if (rotation < 1.5) return float2(uv.y, 1.0 - uv.x);
        if (rotation < 2.5) return 1.0 - uv;
        return float2(1.0 - uv.y, uv.x);
      }

      float4 PSMain(VertexOutput input) : SV_TARGET {
        float2 aspect = float2(resolution.x / resolution.y, 1.0);
        float2 p = (input.uv - center) * aspect;
        float r2 = dot(p, p);
        float radial = 1.0 + k1 * r2 + k2 * r2 * r2;
        float2 sourceUv = center + (p * radial / zoom) / aspect;
        float inside =
          step(0.0, sourceUv.x) * step(sourceUv.x, 1.0) *
          step(0.0, sourceUv.y) * step(sourceUv.y, 1.0);

        float3 desktop = desktopTexture.Sample(desktopSampler, saturate(RotateForCapture(sourceUv))).rgb;
        float3 color = lerp(float3(0.004, 0.008, 0.014), desktop, inside);

        float2 gridCell = frac(sourceUv * float2(16.0, 9.0));
        float2 gridDistance = min(gridCell, 1.0 - gridCell);
        float gridLine = 1.0 - smoothstep(0.0, 0.018, min(gridDistance.x, gridDistance.y));
        float centerX = 1.0 - smoothstep(0.0, 0.0025, abs(sourceUv.x - 0.5));
        float centerY = 1.0 - smoothstep(0.0, 0.0025, abs(sourceUv.y - 0.5));
        float3 gridColor = lerp(float3(0.10, 0.72, 1.0), float3(1.0, 0.34, 0.12), max(centerX, centerY));
        float calibrationLine = max(gridLine * 0.68, max(centerX, centerY));
        color = lerp(color, gridColor, calibrationLine * grid * inside);
        return float4(color, 1.0);
      }
    )HLSL";

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> vertexBlob;
    ComPtr<ID3DBlob> pixelBlob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, "native-undistort", nullptr, nullptr, "VSMain", "vs_4_0", flags, 0, &vertexBlob, &errors);
    if (FAILED(hr)) return ShaderFail(L"顶点着色器编译失败", hr, errors.Get());
    errors.Reset();
    hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, "native-undistort", nullptr, nullptr, "PSMain", "ps_4_0", flags, 0, &pixelBlob, &errors);
    if (FAILED(hr)) return ShaderFail(L"像素着色器编译失败", hr, errors.Get());
    hr = device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &vertexShader_);
    if (FAILED(hr)) return Fail(L"无法创建顶点着色器", hr);
    hr = device_->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &pixelShader_);
    if (FAILED(hr)) return Fail(L"无法创建像素着色器", hr);
    return true;
  }

  bool ShaderFail(const wchar_t* prefix, HRESULT hr, ID3DBlob* errors) {
    lastError_ = prefix;
    lastError_ += L"：";
    lastError_ += HrText(hr);
    if (errors && errors->GetBufferPointer()) {
      const std::string message(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
      const int needed = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
      if (needed > 0) {
        std::wstring wide(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), wide.data(), needed);
        lastError_ += L"\n" + wide;
      }
    }
    return false;
  }

  bool Fail(const wchar_t* prefix, HRESULT hr) {
    lastError_ = prefix;
    lastError_ += L"：";
    lastError_ += HrText(hr);
    return false;
  }

  static float RotationValue(DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
      case DXGI_MODE_ROTATION_ROTATE90: return 1.0f;
      case DXGI_MODE_ROTATION_ROTATE180: return 2.0f;
      case DXGI_MODE_ROTATION_ROTATE270: return 3.0f;
      default: return 0.0f;
    }
  }

  int width_ = 0;
  int height_ = 0;
  float rotation_ = 0.0f;
  UINT captureWidth_ = 0;
  UINT captureHeight_ = 0;
  DXGI_FORMAT captureFormat_ = DXGI_FORMAT_UNKNOWN;
  bool hasFrame_ = false;
  uint64_t framesRendered_ = 0;
  std::wstring lastError_;
  D3D11_VIEWPORT viewport_{};
  ComPtr<IDXGIFactory1> factory_;
  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<IDXGIOutput> output_;
  ComPtr<IDXGIOutput1> output1_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain1> swapChain_;
  ComPtr<ID3D11RenderTargetView> renderTarget_;
  ComPtr<ID3D11Texture2D> captureTexture_;
  ComPtr<ID3D11ShaderResourceView> captureView_;
  ComPtr<ID3D11VertexShader> vertexShader_;
  ComPtr<ID3D11PixelShader> pixelShader_;
  ComPtr<ID3D11Buffer> constantBuffer_;
  ComPtr<ID3D11SamplerState> sampler_;
};

std::unique_ptr<Renderer> gRenderer;

class MetricsSampler {
 public:
  void Initialize(uint64_t initialFrames) {
    frameCount_ = initialFrames;
    lastWall_ = CurrentWall();
    lastCpu_ = CurrentCpu();
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    processors_ = std::max<DWORD>(1, info.dwNumberOfProcessors);
  }

  void Sample(uint64_t frames) {
    const uint64_t wall = CurrentWall();
    const uint64_t cpu = CurrentCpu();
    const uint64_t wallDelta = wall - lastWall_;
    // Preserve the previous meaningful sample if two UI paths request metrics
    // almost simultaneously. This avoids transient 0 FPS/0% readings.
    if (wallDelta >= 5000000ULL) {
      const double seconds = static_cast<double>(wallDelta) / 10000000.0;
      gMetrics.fps = static_cast<double>(frames - frameCount_) / seconds;
      gMetrics.cpuPercent = std::clamp(
        static_cast<double>(cpu - lastCpu_) / static_cast<double>(wallDelta) * 100.0 / processors_,
        0.0, 100.0);
      frameCount_ = frames;
      lastWall_ = wall;
      lastCpu_ = cpu;
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
      gMetrics.workingSet = memory.WorkingSetSize;
      gMetrics.privateBytes = memory.PrivateUsage;
    }
  }

 private:
  static uint64_t CurrentWall() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return FileTimeValue(now);
  }

  static uint64_t CurrentCpu() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
    return FileTimeValue(kernel) + FileTimeValue(user);
  }

  uint64_t frameCount_ = 0;
  uint64_t lastWall_ = 0;
  uint64_t lastCpu_ = 0;
  DWORD processors_ = 1;
};

MetricsSampler gMetricsSampler;

void ApplyFont(HWND control) {
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
}

HWND AddControl(const wchar_t* klass, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id) {
  HWND control = CreateWindowExW(
    0, klass, text, WS_CHILD | WS_VISIBLE | style,
    x, y, width, height, gPanel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  if (control) ApplyFont(control);
  return control;
}

void UpdatePanelText() {
  if (!gPanel) return;
  SetWindowTextW(GetDlgItem(gPanel, IDC_STATUS), gSettings.enabled ? L"● 反畸变已启用" : L"○ 安全旁路（真实桌面）");
  SetWindowTextW(GetDlgItem(gPanel, IDC_TOGGLE), gSettings.enabled ? L"切回安全旁路" : L"启用反畸变");

  wchar_t text[160]{};
  swprintf_s(text, L"主曲率 k1：%+.3f", gSettings.k1);
  SetWindowTextW(GetDlgItem(gPanel, IDC_K1_LABEL), text);
  swprintf_s(text, L"边角曲率 k2：%+.3f", gSettings.k2);
  SetWindowTextW(GetDlgItem(gPanel, IDC_K2_LABEL), text);
  swprintf_s(text, L"画面缩放：%.2f×", gSettings.zoom);
  SetWindowTextW(GetDlgItem(gPanel, IDC_ZOOM_LABEL), text);
  swprintf_s(text, L"畸变中心：%.2f, %.2f", gSettings.centerX, gSettings.centerY);
  SetWindowTextW(GetDlgItem(gPanel, IDC_CENTER_LABEL), text);
  SetWindowTextW(GetDlgItem(gPanel, IDC_GRID), gSettings.grid ? L"隐藏校准网格" : L"显示校准网格");
  swprintf_s(text, L"帧率上限：%d FPS", gSettings.fpsLimit);
  SetWindowTextW(GetDlgItem(gPanel, IDC_FPS), text);

  const double mib = 1024.0 * 1024.0;
  swprintf_s(text, L"%.1f FPS · CPU %.1f%% · 内存 %.1f MiB · 私有 %.1f MiB",
             gMetrics.fps, gMetrics.cpuPercent,
             static_cast<double>(gMetrics.workingSet) / mib,
             static_cast<double>(gMetrics.privateBytes) / mib);
  SetWindowTextW(GetDlgItem(gPanel, IDC_METRICS), text);
}

void CreatePanelControls() {
  AddControl(L"STATIC", L"○ 安全旁路（真实桌面）", SS_CENTER | SS_CENTERIMAGE, 20, 16, 390, 30, IDC_STATUS);
  AddControl(L"STATIC", L"正在采样性能…", SS_CENTER | SS_CENTERIMAGE, 20, 49, 390, 24, IDC_METRICS);
  AddControl(L"BUTTON", L"启用反畸变", BS_PUSHBUTTON, 20, 82, 390, 42, IDC_TOGGLE);

  AddControl(L"STATIC", L"主曲率 k1：+0.000", SS_CENTERIMAGE, 20, 139, 235, 32, IDC_K1_LABEL);
  AddControl(L"BUTTON", L"－", BS_PUSHBUTTON, 270, 139, 64, 32, IDC_K1_DOWN);
  AddControl(L"BUTTON", L"＋", BS_PUSHBUTTON, 346, 139, 64, 32, IDC_K1_UP);

  AddControl(L"STATIC", L"边角曲率 k2：+0.000", SS_CENTERIMAGE, 20, 181, 235, 32, IDC_K2_LABEL);
  AddControl(L"BUTTON", L"－", BS_PUSHBUTTON, 270, 181, 64, 32, IDC_K2_DOWN);
  AddControl(L"BUTTON", L"＋", BS_PUSHBUTTON, 346, 181, 64, 32, IDC_K2_UP);

  AddControl(L"STATIC", L"画面缩放：1.00×", SS_CENTERIMAGE, 20, 223, 235, 32, IDC_ZOOM_LABEL);
  AddControl(L"BUTTON", L"－", BS_PUSHBUTTON, 270, 223, 64, 32, IDC_ZOOM_DOWN);
  AddControl(L"BUTTON", L"＋", BS_PUSHBUTTON, 346, 223, 64, 32, IDC_ZOOM_UP);

  AddControl(L"STATIC", L"畸变中心：0.50, 0.50", SS_CENTERIMAGE, 20, 269, 235, 32, IDC_CENTER_LABEL);
  AddControl(L"BUTTON", L"↑", BS_PUSHBUTTON, 318, 265, 44, 30, IDC_CENTER_UP);
  AddControl(L"BUTTON", L"←", BS_PUSHBUTTON, 270, 299, 44, 30, IDC_CENTER_LEFT);
  AddControl(L"BUTTON", L"→", BS_PUSHBUTTON, 366, 299, 44, 30, IDC_CENTER_RIGHT);
  AddControl(L"BUTTON", L"↓", BS_PUSHBUTTON, 318, 333, 44, 30, IDC_CENTER_DOWN);

  AddControl(L"BUTTON", L"隐藏校准网格", BS_PUSHBUTTON, 20, 379, 190, 34, IDC_GRID);
  AddControl(L"BUTTON", L"帧率上限：30 FPS", BS_PUSHBUTTON, 220, 379, 190, 34, IDC_FPS);
  AddControl(L"BUTTON", L"预设：桶形镜面", BS_PUSHBUTTON, 20, 423, 190, 34, IDC_PRESET_BARREL);
  AddControl(L"BUTTON", L"预设：枕形镜面", BS_PUSHBUTTON, 220, 423, 190, 34, IDC_PRESET_PINCUSHION);
  AddControl(L"BUTTON", L"保存参数", BS_PUSHBUTTON, 20, 467, 190, 34, IDC_SAVE);
  AddControl(L"BUTTON", L"重置并旁路", BS_PUSHBUTTON, 220, 467, 190, 34, IDC_RESET);

  AddControl(
    L"STATIC",
    L"作者：syh · 安全退出：Esc\r\n备用退出：Ctrl + Shift + Alt + F12\r\n启用/旁路：Ctrl + Alt + B\r\n覆盖层鼠标穿透；显示模式变化或渲染失联会自动旁路。",
    SS_LEFT, 20, 515, 390, 78, 0);
  AddControl(L"BUTTON", L"立即安全退出", BS_PUSHBUTTON, 20, 603, 390, 40, IDC_EXIT);
}

void ResetSafetyDeadline() {
  gSafetyDeadline = GetTickCount64() + kSafetyTimeoutMs;
}

void SetWatchdogEnabled(bool enabled) {
  if (gWatchdogState) InterlockedExchange(&gWatchdogState->enabled, enabled ? 1 : 0);
}

void SetEffectEnabled(bool enabled) {
  if (gQuitting) return;
  gSettings.enabled = enabled;
  SetWatchdogEnabled(enabled);
  ResetSafetyDeadline();
  if (enabled) {
    gNeedsRedraw = true;
    gPendingOverlayShow = true;
  } else {
    gPendingOverlayShow = false;
    if (gOverlay) ShowWindow(gOverlay, SW_HIDE);
  }
  UpdatePanelText();
}

void RequestExit() {
  if (gQuitting) return;
  gQuitting = true;
  gSettings.enabled = false;
  SetWatchdogEnabled(false);
  if (gOverlay) ShowWindow(gOverlay, SW_HIDE);
  SaveSettings();
  gRunning = false;
  PostQuitMessage(0);
}

void MarkParameterChanged() {
  gNeedsRedraw = true;
  ResetSafetyDeadline();
  UpdatePanelText();
}

void ExecuteControl(int id) {
  switch (id) {
    case IDC_TOGGLE:
    case IDM_TRAY_TOGGLE:
      SetEffectEnabled(!gSettings.enabled);
      break;
    case IDC_K1_DOWN: gSettings.k1 = std::max(-2.0f, gSettings.k1 - 0.05f); MarkParameterChanged(); break;
    case IDC_K1_UP: gSettings.k1 = std::min(2.0f, gSettings.k1 + 0.05f); MarkParameterChanged(); break;
    case IDC_K2_DOWN: gSettings.k2 = std::max(-3.0f, gSettings.k2 - 0.05f); MarkParameterChanged(); break;
    case IDC_K2_UP: gSettings.k2 = std::min(3.0f, gSettings.k2 + 0.05f); MarkParameterChanged(); break;
    case IDC_ZOOM_DOWN: gSettings.zoom = std::max(0.70f, gSettings.zoom - 0.02f); MarkParameterChanged(); break;
    case IDC_ZOOM_UP: gSettings.zoom = std::min(1.40f, gSettings.zoom + 0.02f); MarkParameterChanged(); break;
    case IDC_CENTER_UP: gSettings.centerY = std::max(0.20f, gSettings.centerY - 0.01f); MarkParameterChanged(); break;
    case IDC_CENTER_DOWN: gSettings.centerY = std::min(0.80f, gSettings.centerY + 0.01f); MarkParameterChanged(); break;
    case IDC_CENTER_LEFT: gSettings.centerX = std::max(0.20f, gSettings.centerX - 0.01f); MarkParameterChanged(); break;
    case IDC_CENTER_RIGHT: gSettings.centerX = std::min(0.80f, gSettings.centerX + 0.01f); MarkParameterChanged(); break;
    case IDC_GRID: gSettings.grid = !gSettings.grid; MarkParameterChanged(); break;
    case IDC_FPS: gSettings.fpsLimit = gSettings.fpsLimit == 30 ? 60 : 30; MarkParameterChanged(); break;
    case IDC_PRESET_BARREL:
      gSettings.k1 = -0.25f; gSettings.k2 = 0.05f; gSettings.zoom = 1.02f; MarkParameterChanged(); break;
    case IDC_PRESET_PINCUSHION:
      gSettings.k1 = 0.25f; gSettings.k2 = -0.05f; gSettings.zoom = 1.02f; MarkParameterChanged(); break;
    case IDC_SAVE:
      SaveSettings();
      MessageBeep(MB_OK);
      break;
    case IDC_RESET:
      gSettings = Settings{};
      SetEffectEnabled(false);
      MarkParameterChanged();
      break;
    case IDC_EXIT:
    case IDM_TRAY_EXIT:
      RequestExit();
      break;
    case IDM_TRAY_SHOW:
      ShowWindow(gPanel, SW_RESTORE);
      SetForegroundWindow(gPanel);
      break;
    default:
      break;
  }
}

void ShowTrayMenu() {
  POINT cursor{};
  GetCursorPos(&cursor);
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"显示控制面板");
  AppendMenuW(menu, MF_STRING, IDM_TRAY_TOGGLE, gSettings.enabled ? L"切回安全旁路" : L"启用反畸变");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"安全退出");
  SetForegroundWindow(gPanel);
  const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, gPanel, nullptr);
  DestroyMenu(menu);
  if (command != 0) ExecuteControl(static_cast<int>(command));
}

LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_DISPLAYCHANGE:
      if (gPanel) PostMessageW(gPanel, kBypassMessage, 0, 0);
      return 0;
    case WM_QUERYENDSESSION:
      ShowWindow(window, SW_HIDE);
      return TRUE;
    default:
      return DefWindowProcW(window, message, wParam, lParam);
  }
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (key && key->vkCode == VK_ESCAPE) {
      if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && gPanel) {
        PostMessageW(gPanel, WM_CLOSE, 0, 0);
      }
      // The overlay owns Esc while it is running, matching the Electron
      // safety behavior. Suppress both key-down and key-up.
      return 1;
    }
  }
  return CallNextHookEx(gKeyboardHook, code, wParam, lParam);
}

LRESULT CALLBACK PanelWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_COMMAND:
      if (HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == 0) ExecuteControl(LOWORD(wParam));
      return 0;
    case WM_HOTKEY:
      if (wParam == HOTKEY_EXIT_FALLBACK) RequestExit();
      else if (wParam == HOTKEY_TOGGLE) SetEffectEnabled(!gSettings.enabled);
      return 0;
    case WM_TIMER:
      if (wParam == kUiTimer && gRenderer) {
        gMetricsSampler.Sample(gRenderer->FramesRendered());
        UpdatePanelText();
      }
      return 0;
    case kTrayMessage:
      if (LOWORD(lParam) == WM_LBUTTONDBLCLK) ExecuteControl(IDM_TRAY_SHOW);
      else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) ShowTrayMenu();
      return 0;
    case kBypassMessage:
      SetEffectEnabled(false);
      return 0;
    case WM_POWERBROADCAST:
      if (wParam == PBT_APMSUSPEND) SetEffectEnabled(false);
      return TRUE;
    case WM_QUERYENDSESSION:
      if (gOverlay) ShowWindow(gOverlay, SW_HIDE);
      return TRUE;
    case WM_CLOSE:
      RequestExit();
      return 0;
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, RGB(225, 235, 245));
      SetBkColor(dc, RGB(10, 19, 29));
      return reinterpret_cast<LRESULT>(gPanelBrush);
    }
    default:
      return DefWindowProcW(window, message, wParam, lParam);
  }
}

bool RegisterWindowClasses(HINSTANCE instance) {
  WNDCLASSEXW overlayClass{};
  overlayClass.cbSize = sizeof(overlayClass);
  overlayClass.hInstance = instance;
  overlayClass.lpfnWndProc = OverlayWindowProc;
  overlayClass.lpszClassName = kOverlayClass;
  overlayClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&overlayClass)) return false;

  WNDCLASSEXW panelClass{};
  panelClass.cbSize = sizeof(panelClass);
  panelClass.hInstance = instance;
  panelClass.lpfnWndProc = PanelWindowProc;
  panelClass.lpszClassName = kPanelClass;
  panelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  panelClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  panelClass.hbrBackground = gPanelBrush;
  return RegisterClassExW(&panelClass) != 0;
}

bool CreateWindows(HINSTANCE instance) {
  POINT cursor{};
  GetCursorPos(&cursor);
  gMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(gMonitor, &info)) return false;
  gMonitorRect = info.rcMonitor;
  const int monitorWidth = gMonitorRect.right - gMonitorRect.left;
  const int monitorHeight = gMonitorRect.bottom - gMonitorRect.top;

  gOverlay = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
    kOverlayClass, L"Vervormde Skerm Native Overlay", WS_POPUP,
    gMonitorRect.left, gMonitorRect.top, monitorWidth, monitorHeight,
    nullptr, nullptr, instance, nullptr);
  if (!gOverlay) return false;
  if (!ExcludeWindowFromCapture(gOverlay)) return false;

  constexpr int clientWidth = 430;
  constexpr int clientHeight = 660;
  RECT panelRect{0, 0, clientWidth, clientHeight};
  AdjustWindowRectEx(&panelRect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
  const int panelWidth = panelRect.right - panelRect.left;
  const int panelHeight = panelRect.bottom - panelRect.top;
  const int panelX = std::max(info.rcWork.left + 12, info.rcWork.right - panelWidth - 18);
  const int panelY = info.rcWork.top + 18;
  gPanel = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    kPanelClass, kAppTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
    panelX, panelY, panelWidth, panelHeight,
    nullptr, nullptr, instance, nullptr);
  if (!gPanel) return false;
  CreatePanelControls();
  ExcludeWindowFromCapture(gPanel);
  return true;
}

void InitializeTray() {
  gTray.cbSize = sizeof(gTray);
  gTray.hWnd = gPanel;
  gTray.uID = 1;
  gTray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  gTray.uCallbackMessage = kTrayMessage;
  gTray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(gTray.szTip, L"Vervormde Skerm Native");
  Shell_NotifyIconW(NIM_ADD, &gTray);
  gTray.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &gTray);
}

bool InitializeHotkeys() {
  gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
  const bool fallbackReady = RegisterHotKey(gPanel, HOTKEY_EXIT_FALLBACK, MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_NOREPEAT, VK_F12) != FALSE;
  RegisterHotKey(gPanel, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B');
  return gKeyboardHook != nullptr && fallbackReady;
}

void CleanupShell() {
  UnregisterHotKey(gPanel, HOTKEY_EXIT_FALLBACK);
  UnregisterHotKey(gPanel, HOTKEY_TOGGLE);
  if (gKeyboardHook) {
    UnhookWindowsHookEx(gKeyboardHook);
    gKeyboardHook = nullptr;
  }
  if (gTray.cbSize != 0) Shell_NotifyIconW(NIM_DELETE, &gTray);
}

int RunWatchdogChild(const wchar_t* mappingName, DWORD parentPid) {
  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName);
  if (!mapping) return 2;
  auto* state = static_cast<WatchdogShared*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(WatchdogShared)));
  HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, parentPid);
  if (!state || !parent) {
    if (state) UnmapViewOfFile(state);
    CloseHandle(mapping);
    if (parent) CloseHandle(parent);
    return 3;
  }

  LONG64 lastHeartbeat = InterlockedCompareExchange64(&state->heartbeat, 0, 0);
  DWORD missed = 0;
  for (;;) {
    if (WaitForSingleObject(parent, 1000) == WAIT_OBJECT_0) break;
    if (InterlockedCompareExchange(&state->shutdown, 0, 0) != 0) break;
    const LONG enabled = InterlockedCompareExchange(&state->enabled, 0, 0);
    const LONG64 heartbeat = InterlockedCompareExchange64(&state->heartbeat, 0, 0);
    if (!enabled || heartbeat != lastHeartbeat) {
      missed = 0;
      lastHeartbeat = heartbeat;
      continue;
    }
    if (++missed >= kWatchdogMissLimit) {
      TerminateProcess(parent, 0xE001);
      break;
    }
  }
  CloseHandle(parent);
  UnmapViewOfFile(state);
  CloseHandle(mapping);
  return 0;
}

bool StartWatchdog() {
  wchar_t mappingName[96]{};
  swprintf_s(mappingName, L"Local\\VervormdeSkermWatchdog-%lu", GetCurrentProcessId());
  gWatchdogMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(WatchdogShared), mappingName);
  if (!gWatchdogMapping) return false;
  gWatchdogState = static_cast<WatchdogShared*>(MapViewOfFile(gWatchdogMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(WatchdogShared)));
  if (!gWatchdogState) return false;
  *gWatchdogState = WatchdogShared{};

  const std::wstring executable = GetExecutablePath();
  std::wstring command = L"\"" + executable + L"\" --watchdog \"" + mappingName + L"\" " + std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
  CloseHandle(process.hThread);
  gWatchdogProcess = process.hProcess;
  return true;
}

void StopWatchdog() {
  if (gWatchdogState) InterlockedExchange(&gWatchdogState->shutdown, 1);
  if (gWatchdogProcess) {
    WaitForSingleObject(gWatchdogProcess, 2500);
    CloseHandle(gWatchdogProcess);
    gWatchdogProcess = nullptr;
  }
  if (gWatchdogState) {
    UnmapViewOfFile(gWatchdogState);
    gWatchdogState = nullptr;
  }
  if (gWatchdogMapping) {
    CloseHandle(gWatchdogMapping);
    gWatchdogMapping = nullptr;
  }
}

void WriteSmokeLog(bool success) {
  std::wofstream log(gSmokeLogPath, std::ios::trunc);
  if (!log) return;
  const double mib = 1024.0 * 1024.0;
  log << L"mode=native-directx\n";
  log << L"success=" << (success ? L"true" : L"false") << L"\n";
  log << L"frames=" << (gRenderer ? gRenderer->FramesRendered() : 0) << L"\n";
  log << L"fps=" << gMetrics.fps << L"\n";
  log << L"cpu_percent=" << gMetrics.cpuPercent << L"\n";
  log << L"working_set_mib=" << static_cast<double>(gMetrics.workingSet) / mib << L"\n";
  log << L"private_mib=" << static_cast<double>(gMetrics.privateBytes) / mib << L"\n";
  if (gRenderer && !gRenderer->LastError().empty()) log << L"error=" << gRenderer->LastError() << L"\n";
}

bool HasArgument(int argc, wchar_t** argv, const wchar_t* expected) {
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], expected) == 0) return true;
  }
  return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc >= 4 && _wcsicmp(argv[1], L"--watchdog") == 0) {
    const DWORD parentPid = wcstoul(argv[3], nullptr, 10);
    const int result = RunWatchdogChild(argv[2], parentPid);
    LocalFree(argv);
    CoUninitialize();
    return result;
  }
  gSmokeTest = HasArgument(argc, argv, L"--smoke-test");
  gWatchdogTest = HasArgument(argc, argv, L"--watchdog-test");
  LocalFree(argv);

  gInstanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\VervormdeSkermNative-SingleInstance");
  if (!gInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"Vervormde Skerm Native 已经在运行。请查看控制面板或系统托盘。", kAppTitle, MB_OK | MB_ICONINFORMATION);
    if (gInstanceMutex) CloseHandle(gInstanceMutex);
    CoUninitialize();
    return 0;
  }

  const std::wstring dataDirectory = GetLocalDataDirectory();
  gSettingsPath = dataDirectory + L"\\settings.ini";
  wchar_t tempPath[MAX_PATH]{};
  GetTempPathW(MAX_PATH, tempPath);
  gSmokeLogPath = std::wstring(tempPath) + L"vervormde-skerm-native-smoke.log";
  LoadSettings();

  gPanelBrush = CreateSolidBrush(RGB(10, 19, 29));
  gFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  if (!RegisterWindowClasses(instance) || !CreateWindows(instance)) {
    MessageBoxW(nullptr, L"无法创建原生窗口。", kAppTitle, MB_OK | MB_ICONERROR);
    gExitCode = 1;
    goto cleanup;
  }

  {
    const int width = gMonitorRect.right - gMonitorRect.left;
    const int height = gMonitorRect.bottom - gMonitorRect.top;
    gRenderer = std::make_unique<Renderer>();
    if (!gRenderer->Initialize(gOverlay, gMonitor, width, height)) {
      const std::wstring message = L"DirectX 初始化失败，程序保持安全旁路并退出。\n\n" + gRenderer->LastError();
      if (gSmokeTest) WriteSmokeLog(false);
      MessageBoxW(nullptr, message.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
      gExitCode = 2;
      goto cleanup;
    }
  }

  InitializeTray();
  if (!InitializeHotkeys()) {
    MessageBoxW(nullptr, L"紧急退出热键注册失败。为保证安全，程序不会启动覆盖层。", kAppTitle, MB_OK | MB_ICONERROR);
    gExitCode = 5;
    goto cleanup;
  }
  if (!StartWatchdog()) {
    MessageBoxW(nullptr, L"独立安全看门狗启动失败。为保证安全，程序不会启动覆盖层。", kAppTitle, MB_OK | MB_ICONERROR);
    gExitCode = 6;
    goto cleanup;
  }
  gMetricsSampler.Initialize(gRenderer->FramesRendered());
  ResetSafetyDeadline();
  UpdatePanelText();

  if (gSmokeTest || gWatchdogTest) {
    gSettings.fpsLimit = 30;
    SetEffectEnabled(true);
  } else {
    ShowWindow(gPanel, SW_SHOWNORMAL);
    SetForegroundWindow(gPanel);
  }

  {
    const ULONGLONG smokeDeadline = GetTickCount64() + 8000;
    ULONGLONG nextFrame = GetTickCount64();
    ULONGLONG nextMetricSample = GetTickCount64() + 1000;
    MSG message{};
    while (gRunning) {
      if (gWatchdogState) InterlockedIncrement64(&gWatchdogState->heartbeat);

      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
          gRunning = false;
          break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (!gRunning) break;

      const ULONGLONG now = GetTickCount64();
      if (gSettings.enabled && now >= gSafetyDeadline) {
        SetEffectEnabled(false);
      }

      if (gSettings.enabled && now >= nextFrame) {
        bool drewFrame = false;
        if (!gRenderer->Tick(gSettings, gNeedsRedraw, drewFrame)) {
          SetEffectEnabled(false);
          if (!gSmokeTest) {
            const std::wstring error = L"渲染失败，已自动切回安全旁路。\n\n" + gRenderer->LastError();
            MessageBoxW(gPanel, error.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
          }
          gExitCode = 3;
          if (gSmokeTest) gRunning = false;
        } else if (drewFrame) {
          gNeedsRedraw = false;
          if (gPendingOverlayShow) {
            ShowWindow(gOverlay, SW_SHOWNOACTIVATE);
            SetWindowPos(gOverlay, HWND_TOPMOST, gMonitorRect.left, gMonitorRect.top,
                         gMonitorRect.right - gMonitorRect.left, gMonitorRect.bottom - gMonitorRect.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ExcludeWindowFromCapture(gOverlay);
            if (!gSmokeTest) SetWindowPos(gPanel, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            gPendingOverlayShow = false;
            // Release-only safety test: deliberately stop the parent heartbeat.
            // The independent watchdog process must terminate us and Windows
            // will remove the overlay without relying on this UI thread.
            if (gWatchdogTest) Sleep(30000);
          }
        }
        nextFrame = now + static_cast<ULONGLONG>(1000 / std::max(1, gSettings.fpsLimit));
      }

      if (now >= nextMetricSample) {
        gMetricsSampler.Sample(gRenderer->FramesRendered());
        UpdatePanelText();
        nextMetricSample = now + 1000;
      }

      if (gSmokeTest && now >= smokeDeadline) {
        gMetricsSampler.Sample(gRenderer->FramesRendered());
        const bool success = gExitCode == 0 && gRenderer->FramesRendered() > 0;
        WriteSmokeLog(success);
        gExitCode = success ? 0 : 4;
        RequestExit();
        break;
      }

      const ULONGLONG afterWork = GetTickCount64();
      ULONGLONG waitUntil = afterWork + 50;
      if (gSettings.enabled) waitUntil = std::min(waitUntil, nextFrame);
      if (gSmokeTest) waitUntil = std::min(waitUntil, smokeDeadline);
      const DWORD waitMs = waitUntil > afterWork ? static_cast<DWORD>(std::min<ULONGLONG>(50, waitUntil - afterWork)) : 0;
      MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
  }

cleanup:
  if (gOverlay) ShowWindow(gOverlay, SW_HIDE);
  SetWatchdogEnabled(false);
  CleanupShell();
  StopWatchdog();
  gRenderer.reset();
  if (gPanel) DestroyWindow(gPanel);
  if (gOverlay) DestroyWindow(gOverlay);
  if (gFont) DeleteObject(gFont);
  if (gPanelBrush) DeleteObject(gPanelBrush);
  if (gInstanceMutex) CloseHandle(gInstanceMutex);
  CoUninitialize();
  return gExitCode;
}
