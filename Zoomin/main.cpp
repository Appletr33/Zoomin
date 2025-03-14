#include <winrt/base.h> 
#include <winrt/Windows.Foundation.h> 
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <d3d11.h>
#include <d3d11_1.h>  // For improved performance
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <cstring>
#ifdef min
#undef min
#endif
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <queue>

// GLFW includes
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <Windows.Graphics.Capture.Interop.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

// Constants for performance optimization
constexpr int MAX_FRAME_LATENCY = 1;
constexpr int CAPTURE_BUFFER_COUNT = 2;
constexpr int SWAP_CHAIN_BUFFER_COUNT = 2;
constexpr float DEFAULT_ZOOM = 1.0f;
constexpr float ZOOMED_MAGNIFICATION = 2.0f;
constexpr float ZOOM_SMOOTHING_FACTOR = 0.2f;

// Structure to synchronize frame processing
struct FrameContext {
    ComPtr<ID3D11Texture2D> texture;
    bool ready = false;
};

// Global variables for Windows Graphics Capture
GraphicsCaptureItem g_CaptureItem{ nullptr };
Direct3D11CaptureFramePool g_FramePool{ nullptr };
GraphicsCaptureSession g_CaptureSession{ nullptr };
event_token g_FrameArrivedToken;
std::atomic<bool> g_FrameArrived(false);
std::mutex g_CaptureMutex;
std::condition_variable g_FrameCondition;

// Producer-consumer queue for frame processing
std::queue<ComPtr<ID3D11Texture2D>> g_FrameQueue;
std::mutex g_QueueMutex;
std::condition_variable g_QueueCondition;
constexpr size_t MAX_QUEUE_SIZE = 2;  // Limit queue size to reduce latency

// Double-buffered textures for capture
ComPtr<ID3D11Texture2D> g_CaptureTextures[2];
int g_CurrentCaptureTexture = 0;

// Global running flag, window dimensions and swap chain
std::atomic<bool> g_Running = true;
GLFWwindow* g_Window = nullptr;
int g_ScreenWidth = 0;
int g_ScreenHeight = 0;
ComPtr<IDXGISwapChain1> g_SwapChain;

// DirectX resources
ComPtr<ID3D11Device> g_D3DDevice;
ComPtr<ID3D11DeviceContext> g_D3DContext;
ComPtr<ID3D11DeviceContext1> g_D3DContext1;  // D3D11.1 context for better performance
ComPtr<ID3D11Texture2D> g_RenderTarget;
ComPtr<ID3D11RenderTargetView> g_RenderTargetView;
ComPtr<ID3D11ShaderResourceView> g_FrameShaderResourceView;
ComPtr<ID3D11SamplerState> g_SamplerState;
ComPtr<ID3D11VertexShader> g_VertexShader;
ComPtr<ID3D11PixelShader> g_PixelShader;
ComPtr<ID3D11Buffer> g_VertexBuffer;
ComPtr<ID3D11InputLayout> g_InputLayout;

// Global zoom factor (starts at 1.0)
std::atomic<float> g_CurrentZoom(DEFAULT_ZOOM);
std::atomic<float> g_TargetZoom(DEFAULT_ZOOM);

// Constant buffer structure for the shader
struct MagnificationConstantBuffer {
    float magnificationFactor;
    float padding[3]; // 16-byte alignment
};
ComPtr<ID3D11Buffer> g_ConstantBuffer;

// Vertex structure for the fullscreen quad
#pragma pack(push, 1)
struct Vertex {
    float position[3];
    float texCoord[2];
};
#pragma pack(pop)

// Global variables to manage window visibility toggle.
// The window starts hidden.
std::atomic<bool> g_WindowVisible(false);
std::atomic<bool> g_WindowToggleRequest(false);

// Frame timing statistics
std::chrono::high_resolution_clock::time_point g_LastFrameTime;
std::atomic<float> g_FrameTimeMs(0.0f);
std::atomic<float> g_FrameRate(0.0f);
int g_FrameCount = 0;
std::chrono::high_resolution_clock::time_point g_FPSTime;

// Helper function to convert HRESULT to string
std::string HrToString(HRESULT hr) {
    char buffer[32];
    sprintf_s(buffer, "0x%08X", hr);
    return std::string(buffer);
}

// Global low-level keyboard hook to detect Shift+Esc (exit) and Numpad 8 (toggle window)
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* pKeyboard = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_KEYDOWN)
        {
            // Exit when Shift+Esc is pressed.
            if (pKeyboard->vkCode == VK_ESCAPE && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
            {
                g_Running = false;
                PostQuitMessage(0);  // Exit hook thread's message loop immediately
            }
            // Toggle window visibility when Numpad 8 is pressed (only on non-repeat).
            else if (pKeyboard->vkCode == VK_NUMPAD8)
            {
                if (!(pKeyboard->flags & 0x40000000))
                {
                    g_WindowToggleRequest = true;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Thread function to install the keyboard hook and run a message loop.
DWORD WINAPI KeyboardHookThread(LPVOID lpParam)
{
    HHOOK hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
    if (!hook)
    {
        std::cerr << "Failed to install keyboard hook." << std::endl;
        return 1;
    }
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) && g_Running)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    UnhookWindowsHookEx(hook);
    return 0;
}

// Initialize GLFW window with fullscreen, borderless, and click-through settings.
bool InitializeGLFW() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return false;
    }
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    g_ScreenWidth = mode->width;
    g_ScreenHeight = mode->height;

    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    g_Window = glfwCreateWindow(g_ScreenWidth, g_ScreenHeight, "Screen Magnifier", nullptr, nullptr);
    if (!g_Window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

    // Setup window as click-through.
    HWND hwnd = glfwGetWin32Window(g_Window);
    if (!hwnd) {
        std::cerr << "Failed to get native window handle" << std::endl;
        return false;
    }

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    SetWindowLong(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // Hide the window initially.
    glfwHideWindow(g_Window);
    return true;
}

// Create high-performance swap chain
bool CreateSwapChain()
{
    HWND hwnd = glfwGetWin32Window(g_Window);
    if (!hwnd)
    {
        std::cerr << "Could not retrieve HWND for swap chain creation." << std::endl;
        return false;
    }

    // Prevent our own window from being captured
    if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        std::cerr << "Failed to set window display affinity." << std::endl;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = g_ScreenWidth;
    swapChainDesc.Height = g_ScreenHeight;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // Most efficient swap effect
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapChainDesc.Flags = 0;

    ComPtr<IDXGIFactory2> dxgiFactory;
    ComPtr<IDXGIAdapter> dxgiAdapter;

    // Get the DXGI factory from the device
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = g_D3DDevice.As(&dxgiDevice);
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI device: " << HrToString(hr) << std::endl;
        return false;
    }

    // Set maximum frame latency to 1 for reduced latency
    ComPtr<IDXGIDevice1> dxgiDevice1;
    hr = dxgiDevice.As(&dxgiDevice1);
    if (SUCCEEDED(hr)) {
        dxgiDevice1->SetMaximumFrameLatency(MAX_FRAME_LATENCY);
    }

    hr = dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "Failed to get adapter: " << HrToString(hr) << std::endl;
        return false;
    }

    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        std::cerr << "Failed to get factory: " << HrToString(hr) << std::endl;
        return false;
    }

    // Create the swap chain
    hr = dxgiFactory->CreateSwapChainForHwnd(
        g_D3DDevice.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &g_SwapChain
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create swap chain: " << HrToString(hr) << std::endl;
        return false;
    }

    // Disable Alt+Enter fullscreen toggle
    dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    return true;
}

// Helper function to get the GraphicsCaptureItem for a window
GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd) {
    auto interopFactory = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    check_hresult(interopFactory->CreateForWindow(hwnd, guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        put_abi(item)));
    return item;
}

// Helper function to create an IDirect3DDevice from an ID3D11Device
winrt::com_ptr<::IInspectable> CreateDirect3DDeviceFromD3D11Device(ID3D11Device* d3dDevice) {
    com_ptr<IDXGIDevice> dxgiDevice;
    check_hresult(d3dDevice->QueryInterface(dxgiDevice.put()));

    com_ptr<::IInspectable> device;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), device.put()));
    return device;
}

// Find the target window by name
HWND FindTargetWindow(const char* windowName) {
    // Try exact match first
    HWND hwnd = FindWindowA(NULL, windowName);
    if (hwnd) {
        return hwnd;
    }

    // If exact match failed, try partial match with EnumWindows
    struct FindWindowData {
        const char* partialName;
        HWND result;
    };

    FindWindowData data = { windowName, NULL };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        FindWindowData* data = reinterpret_cast<FindWindowData*>(lParam);
        char title[256];
        GetWindowTextA(hwnd, title, sizeof(title));

        // Check if window is visible and title contains the target name
        if (IsWindowVisible(hwnd) && strstr(title, data->partialName) != nullptr) {
            data->result = hwnd;
            return FALSE; // Stop enumeration
        }
        return TRUE; // Continue enumeration
        }, reinterpret_cast<LPARAM>(&data));

    return data.result;
}

// Optimized frame arrived callback handler with direct GPU processing
void OnFrameArrived(Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);

    auto frame = sender.TryGetNextFrame();
    if (!frame) return;

    // Get the frame size
    auto frameContentSize = frame.ContentSize();

    try {
        // Acquire ID3D11Texture2D from the frame surface
        auto dxgiInterfaceAccess = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture;
        HRESULT hr = dxgiInterfaceAccess->GetInterface(IID_PPV_ARGS(&frameTexture));

        if (SUCCEEDED(hr) && frameTexture) {
            // Use next capture texture in our double-buffer
            int nextTexture = (g_CurrentCaptureTexture + 1) % 2;

            // If we don't have the capture texture or need to resize it
            if (!g_CaptureTextures[nextTexture] ||
                frameContentSize.Width != g_ScreenWidth ||
                frameContentSize.Height != g_ScreenHeight) {

                // Update dimensions
                g_ScreenWidth = frameContentSize.Width;
                g_ScreenHeight = frameContentSize.Height;

                // Create or recreate the texture
                D3D11_TEXTURE2D_DESC desc;
                frameTexture->GetDesc(&desc);

                D3D11_TEXTURE2D_DESC newDesc = {};
                newDesc.Width = g_ScreenWidth;
                newDesc.Height = g_ScreenHeight;
                newDesc.MipLevels = 1;
                newDesc.ArraySize = 1;
                newDesc.Format = desc.Format;
                newDesc.SampleDesc.Count = 1;
                newDesc.Usage = D3D11_USAGE_DEFAULT;
                newDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                newDesc.CPUAccessFlags = 0;
                newDesc.MiscFlags = 0;

                g_CaptureTextures[nextTexture].Reset();
                hr = g_D3DDevice->CreateTexture2D(&newDesc, nullptr, &g_CaptureTextures[nextTexture]);

                if (FAILED(hr)) {
                    std::cerr << "Failed to create texture: " << HrToString(hr) << std::endl;
                    return;
                }
            }

            // Direct texture-to-texture copy on the GPU (no CPU involvement)
            g_D3DContext->CopyResource(g_CaptureTextures[nextTexture].Get(), frameTexture.Get());

            // Switch to the new texture
            g_CurrentCaptureTexture = nextTexture;

            // Signal that a new frame is ready
            g_FrameArrived = true;
        }
    }
    catch (const winrt::hresult_error& ex) {
        std::cerr << "Error in OnFrameArrived: " << winrt::to_string(ex.message()) << std::endl;
    }
}

// Fast initialization of Windows Graphics Capture
bool InitializeCapture() {
    try {
        // Initialize WinRT
        winrt::init_apartment();

        // Try to find the target game window - try different possible names
        HWND gameWindow = nullptr;
        for (const char* name : { "Rainbow Six", "R6", "Siege", "Tom Clancy" }) {
            gameWindow = FindTargetWindow(name);
            if (gameWindow) {
                std::cout << "Found game window: " << name << std::endl;
                break;
            }
        }

        // Fall back to desktop if no game window found
        if (!gameWindow) {
            gameWindow = GetDesktopWindow();
            std::cout << "Game window not found, capturing desktop instead." << std::endl;
        }

        // Get actual window dimensions
        RECT windowRect;
        if (GetClientRect(gameWindow, &windowRect)) {
            g_ScreenWidth = windowRect.right - windowRect.left;
            g_ScreenHeight = windowRect.bottom - windowRect.top;

            // If client rect is too small, try window rect
            if (g_ScreenWidth < 200 || g_ScreenHeight < 200) {
                if (GetWindowRect(gameWindow, &windowRect)) {
                    g_ScreenWidth = windowRect.right - windowRect.left;
                    g_ScreenHeight = windowRect.bottom - windowRect.top;
                }
            }
        }

        // Create the capture item for the window
        g_CaptureItem = CreateCaptureItemForWindow(gameWindow);

        // Get window size from WinRT as a backup
        auto itemSize = g_CaptureItem.Size();

        // Use WinRT size if it's reasonably large and our other measurement failed
        if ((g_ScreenWidth < 200 || g_ScreenHeight < 200) &&
            itemSize.Width > 200 && itemSize.Height > 200) {
            g_ScreenWidth = itemSize.Width;
            g_ScreenHeight = itemSize.Height;
        }

        // Last resort - use desktop size
        if (g_ScreenWidth < 200 || g_ScreenHeight < 200) {
            RECT desktopRect;
            HWND desktopWindow = GetDesktopWindow();
            if (GetWindowRect(desktopWindow, &desktopRect)) {
                g_ScreenWidth = desktopRect.right - desktopRect.left;
                g_ScreenHeight = desktopRect.bottom - desktopRect.top;
            }
            else {
                // Fallback to common resolution
                g_ScreenWidth = 1920;
                g_ScreenHeight = 1080;
            }
        }

        std::cout << "Capture dimensions: " << g_ScreenWidth << "x" << g_ScreenHeight << std::endl;

        // Create a WinRT device from our D3D device
                // Get the DXGI device interface
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = g_D3DDevice.As(&dxgiDevice);
        if (FAILED(hr)) {
            std::cerr << "Failed to get DXGI device: " << HrToString(hr) << std::endl;
            return false;
        }

        // Create the WinRT device
        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
        if (FAILED(hr)) {
            std::cerr << "Failed to create WinRT device: " << HrToString(hr) << std::endl;
            return false;
        }

        // Get the proper WinRT interface
        auto winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        // Set up a high-performance frame pool
        g_FramePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            CAPTURE_BUFFER_COUNT,  // Small buffer count for lower latency
            { g_ScreenWidth, g_ScreenHeight }
        );

        // Subscribe to frame events
        g_FrameArrivedToken = g_FramePool.FrameArrived(OnFrameArrived);

        // Create and configure the capture session
        g_CaptureSession = g_FramePool.CreateCaptureSession(g_CaptureItem);
        g_CaptureSession.IsCursorCaptureEnabled(false);  // Disable cursor for better performance
        g_CaptureSession.IsBorderRequired(false);        // No border for better performance

        // Start capture
        g_CaptureSession.StartCapture();

        std::cout << "Windows Graphics Capture initialized" << std::endl;
        return true;
    }
    catch (const winrt::hresult_error& ex) {
        std::cerr << "WinRT Error: " << std::hex << ex.code() << " - "
            << winrt::to_string(ex.message()) << std::endl;
        return false;
    }
    catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << std::endl;
        return false;
    }
}

// Optimized DirectX initialization
bool InitializeDirectX() {
    HRESULT hr = S_OK;

    // Initialize COM
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Direct3D 11.1 feature levels for better performance
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    // Create the D3D11 device
    hr = D3D11CreateDevice(
        nullptr,                      // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,     // Hardware acceleration
        nullptr,                      // No software renderer
        createDeviceFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &g_D3DDevice,
        &featureLevel,
        &g_D3DContext
    );

    // Fallback if debug device creation fails
    if (FAILED(hr) && (createDeviceFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        createDeviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &g_D3DDevice, &featureLevel, &g_D3DContext
        );
    }

    // Last resort: try WARP software rendering
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &g_D3DDevice, &featureLevel, &g_D3DContext
        );
        if (FAILED(hr)) {
            std::cerr << "D3D11CreateDevice failed with all attempts: " << HrToString(hr) << std::endl;
            CoUninitialize();
            return false;
        }
    }

    // Get the ID3D11DeviceContext1 for better performance
    hr = g_D3DContext.As(&g_D3DContext1);
    if (SUCCEEDED(hr)) {
        std::cout << "Using Direct3D 11.1 for better performance" << std::endl;
    }

    // Set maximum frame latency for reduced latency
    ComPtr<IDXGIDevice1> dxgiDevice1;
    hr = g_D3DDevice.As(&dxgiDevice1);
    if (SUCCEEDED(hr)) {
        dxgiDevice1->SetMaximumFrameLatency(MAX_FRAME_LATENCY);
    }

    // Create render target
    D3D11_TEXTURE2D_DESC rtDesc = {};
    rtDesc.Width = g_ScreenWidth;
    rtDesc.Height = g_ScreenHeight;
    rtDesc.MipLevels = 1;
    rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    rtDesc.CPUAccessFlags = 0;
    rtDesc.MiscFlags = 0;

    hr = g_D3DDevice->CreateTexture2D(&rtDesc, nullptr, &g_RenderTarget);
    if (FAILED(hr)) {
        std::cerr << "Create render target texture failed: " << HrToString(hr) << std::endl;
        return false;
    }

    // Create render target view
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtDesc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;

    hr = g_D3DDevice->CreateRenderTargetView(g_RenderTarget.Get(), &rtvDesc, &g_RenderTargetView);
    if (FAILED(hr)) {
        std::cerr << "Create render target view failed: " << HrToString(hr) << std::endl;
        return false;
    }

    // Create sampler state
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = g_D3DDevice->CreateSamplerState(&sampDesc, &g_SamplerState);
    if (FAILED(hr)) {
        std::cerr << "Create sampler state failed: " << HrToString(hr) << std::endl;
        return false;
    }

    // Create swap chain
    if (!CreateSwapChain()) {
        return false;
    }

    return true;
}

// Create optimized shaders
bool CreateShaders() {
    HRESULT hr = S_OK;

    // Fast vertex shader
    const char* vsCode = R"(
        struct VS_INPUT {
            float3 position : POSITION;
            float2 texCoord : TEXCOORD0;
        };
        struct VS_OUTPUT {
            float4 position : SV_POSITION;
            float2 texCoord : TEXCOORD0;
        };
        VS_OUTPUT main(VS_INPUT input) {
            VS_OUTPUT output;
            output.position = float4(input.position, 1.0f);
            output.texCoord = input.texCoord;
            return output;
        }
    )";

    // Optimized pixel shader with smooth zoom and better performance
    const char* psCode = R"(
        Texture2D frameTexture : register(t0);
        SamplerState frameSampler : register(s0);
        
        cbuffer MagnificationBuffer : register(b0) {
            float magnificationFactor;
            float3 padding;
        }
        
        struct PS_INPUT {
            float4 position : SV_POSITION;
            float2 texCoord : TEXCOORD0;
        };
        
        float4 main(PS_INPUT input) : SV_TARGET {
            // Center of screen for zoom focus
            float2 center = float2(0.5, 0.5);
            
            // Calculate direction from center
            float2 dir = input.texCoord - center;
            
            // Apply zoom factor (inverted for magnification)
            dir = dir / magnificationFactor;
            
            // Calculate final sampling coordinate
            float2 zoomedCoord = center + dir;
            
            // Only sample within texture bounds
            if (zoomedCoord.x >= 0.0 && zoomedCoord.x <= 1.0 &&
                zoomedCoord.y >= 0.0 && zoomedCoord.y <= 1.0) {
                return frameTexture.Sample(frameSampler, zoomedCoord);
            }
            else {
                // Outside bounds - transparent black
                return float4(0.0, 0.0, 0.0, 0.0);
            }
        }
    )";

    // Compile vertex shader
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompile(vsCode, strlen(vsCode), "VertexShader", nullptr, nullptr, "main", "vs_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Vertex shader compilation failed: " << static_cast<char*>(errorBlob->GetBufferPointer()) << std::endl;
        }
        return false;
    }

    // Create vertex shader
    hr = g_D3DDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_VertexShader);
    if (FAILED(hr)) {
        std::cerr << "Create vertex shader failed: " << HrToString(hr) << std::endl;
        return false;
    }

    // Define input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    // Create input layout
    hr = g_D3DDevice->CreateInputLayout(
        layout,
        ARRAYSIZE(layout),
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        &g_InputLayout
    );

    if (FAILED(hr)) {
        std::cerr << "Create input layout failed: " << HrToString(hr) << std::endl;
        return false;
    }

    // Compile pixel shader
    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompile(psCode, strlen(psCode), "PixelShader", nullptr, nullptr, "main", "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Pixel shader compilation failed: " << static_cast<char*>(errorBlob->GetBufferPointer()) << std::endl;
        }
        return false;
    }

    // Create pixel shader
    hr = g_D3DDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_PixelShader);
    if (FAILED(hr)) {
        std::cerr << "Create pixel shader failed: " << HrToString(hr) << std::endl;
        return false;
    }

    // Create fullscreen quad vertices
    Vertex vertices[6] = {
        { {-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f} },  // Top-left
        { { 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f} },  // Bottom-right
        { {-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f} },  // Bottom-left
        { {-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f} },  // Top-left
        { { 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f} },  // Top-right
        { { 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f} }   // Bottom-right
    };

    // Create vertex buffer
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;  // Immutable for better performance
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices;

    hr = g_D3DDevice->CreateBuffer(&vbDesc, &vbData, &g_VertexBuffer);
    if (FAILED(hr)) {
        std::cerr << "Create vertex buffer failed: " << HrToString(hr) << std::endl;
        return false;
    }

    // Create constant buffer
    D3D11_BUFFER_DESC constantBufferDesc = {};
    constantBufferDesc.ByteWidth = sizeof(MagnificationConstantBuffer);
    constantBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantBufferDesc.CPUAccessFlags = 0;

    MagnificationConstantBuffer initialData = { DEFAULT_ZOOM, {0.0f, 0.0f, 0.0f} };
    D3D11_SUBRESOURCE_DATA constantBufferData = {};
    constantBufferData.pSysMem = &initialData;

    hr = g_D3DDevice->CreateBuffer(&constantBufferDesc, &constantBufferData, &g_ConstantBuffer);
    if (FAILED(hr)) {
        std::cerr << "Create constant buffer failed: " << HrToString(hr) << std::endl;
        return false;
    }

    return true;
}

// High-performance render function
void RenderCurrentFrame() {
    if (!g_CaptureTextures[g_CurrentCaptureTexture]) {
        return; // No texture to render yet
    }

    // Create shader resource view if needed
    if (!g_FrameShaderResourceView) {
        D3D11_TEXTURE2D_DESC desc;
        g_CaptureTextures[g_CurrentCaptureTexture]->GetDesc(&desc);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        HRESULT hr = g_D3DDevice->CreateShaderResourceView(
            g_CaptureTextures[g_CurrentCaptureTexture].Get(),
            &srvDesc,
            &g_FrameShaderResourceView
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to create SRV: " << HrToString(hr) << std::endl;
            return;
        }
    }

    // Get window dimensions
    HWND hwnd = glfwGetWin32Window(g_Window);
    RECT rect;
    GetClientRect(hwnd, &rect);
    unsigned int width = rect.right - rect.left;
    unsigned int height = rect.bottom - rect.top;

    // Set viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    g_D3DContext->RSSetViewports(1, &viewport);

    // Bind render target
    g_D3DContext->OMSetRenderTargets(1, g_RenderTargetView.GetAddressOf(), nullptr);

    // Clear render target
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_D3DContext->ClearRenderTargetView(g_RenderTargetView.Get(), clearColor);

    // Set up the pipeline
    g_D3DContext->IASetInputLayout(g_InputLayout.Get());
    g_D3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    g_D3DContext->IASetVertexBuffers(0, 1, g_VertexBuffer.GetAddressOf(), &stride, &offset);

    // Set shaders
    g_D3DContext->VSSetShader(g_VertexShader.Get(), nullptr, 0);
    g_D3DContext->PSSetShader(g_PixelShader.Get(), nullptr, 0);

    // Set shader resources
    g_D3DContext->PSSetSamplers(0, 1, g_SamplerState.GetAddressOf());
    g_D3DContext->PSSetShaderResources(0, 1, g_FrameShaderResourceView.GetAddressOf());
    g_D3DContext->PSSetConstantBuffers(0, 1, g_ConstantBuffer.GetAddressOf());

    // Draw
    g_D3DContext->Draw(6, 0);

    // Get the back buffer and present
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) {
        // Using Direct3D 11.1 context if available for better performance
        if (g_D3DContext1) {
            // Skip mapping stage - directly copy with optimized context
            g_D3DContext1->CopyResource(backBuffer.Get(), g_RenderTarget.Get());
        }
        else {
            g_D3DContext->CopyResource(backBuffer.Get(), g_RenderTarget.Get());
        }

        // Present with low latency
        g_SwapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    }
}

// High-performance frame processing
bool ProcessFrame() {
    static auto lastTime = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    float dt_ms = std::chrono::duration<float, std::milli>(now - lastTime).count();
    lastTime = now;

    // Update FPS counter
    g_FrameCount++;
    auto timeSinceLastFPS = std::chrono::duration<float>(now - g_FPSTime).count();
    if (timeSinceLastFPS > 1.0f) {
        g_FrameRate = g_FrameCount / timeSinceLastFPS;
        g_FrameTimeMs = (timeSinceLastFPS * 1000.0f) / g_FrameCount;
        g_FrameCount = 0;
        g_FPSTime = now;
    }

    // Check right mouse button state for zoom
    float targetZoom = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? ZOOMED_MAGNIFICATION : DEFAULT_ZOOM;
    g_TargetZoom = targetZoom;

    // Smooth zoom transition
    float currentZoom = g_CurrentZoom.load();
    float smoothFactor = std::min(1.0f, dt_ms * ZOOM_SMOOTHING_FACTOR);
    float newZoom = currentZoom + (targetZoom - currentZoom) * smoothFactor;
    g_CurrentZoom = newZoom;

    // Update constant buffer with new zoom
    MagnificationConstantBuffer cbData = { newZoom, {0.0f, 0.0f, 0.0f} };
    g_D3DContext->UpdateSubresource(g_ConstantBuffer.Get(), 0, nullptr, &cbData, 0, 0);

    // Render latest frame
    RenderCurrentFrame();

    return true;
}

// Clean up capture resources
void CleanupCapture() {
    if (g_FramePool) {
        g_FramePool.FrameArrived(g_FrameArrivedToken);
        g_FramePool.Close();
        g_FramePool = nullptr;
    }

    if (g_CaptureSession) {
        g_CaptureSession.Close();
        g_CaptureSession = nullptr;
    }

    g_CaptureItem = nullptr;

    for (auto& texture : g_CaptureTextures) {
        texture.Reset();
    }

    winrt::uninit_apartment();
}

// Main application entry point
int main() {
    // Start timing
    g_FPSTime = std::chrono::high_resolution_clock::now();
    g_LastFrameTime = g_FPSTime;

    if (!InitializeGLFW()) {
        return -1;
    }

    // Start keyboard hook
    DWORD threadId;
    HANDLE hHookThread = CreateThread(nullptr, 0, KeyboardHookThread, nullptr, 0, &threadId);
    if (!hHookThread) {
        std::cerr << "Failed to create keyboard hook thread." << std::endl;
        return -1;
    }

    // Initialize DirectX before capture
    if (!InitializeDirectX()) {
        glfwTerminate();
        return -1;
    }

    // Initialize capture after DirectX
    if (!InitializeCapture()) {
        glfwTerminate();
        return -1;
    }

    // Create shaders
    if (!CreateShaders()) {
        glfwTerminate();
        return -1;
    }

    std::cout << "Screen Magnifier initialized. Hold right-click to zoom; press Shift+ESC to exit. Toggle window visibility with Numpad 8." << std::endl;

    // For frame timing
    using clock = std::chrono::high_resolution_clock;
    const auto targetFrameTime = std::chrono::microseconds(8333); // ~120 FPS target

    // Main loop
    while (g_Running && !glfwWindowShouldClose(g_Window)) {
        auto frameStart = clock::now();

        ProcessFrame();
        glfwPollEvents();

        // Handle window visibility toggle
        if (g_WindowToggleRequest) {
            g_WindowVisible = !g_WindowVisible;

            if (g_WindowVisible) {
                glfwShowWindow(g_Window);
            }
            else {
                glfwHideWindow(g_Window);
            }

            g_WindowToggleRequest = false;
        }

        // Adaptive sleep for smooth frame pacing
        auto frameEnd = clock::now();
        auto frameDuration = frameEnd - frameStart;

        if (frameDuration < targetFrameTime) {
            auto sleepTime = std::min(
                std::chrono::duration_cast<std::chrono::nanoseconds>(targetFrameTime - frameDuration),
                std::chrono::nanoseconds(1)
            );
            std::this_thread::sleep_for(sleepTime);
        }
    }

    // Cleanup
    g_D3DContext->ClearState();
    g_VertexShader.Reset();
    g_PixelShader.Reset();
    g_InputLayout.Reset();
    g_VertexBuffer.Reset();
    g_ConstantBuffer.Reset();
    g_SamplerState.Reset();
    g_FrameShaderResourceView.Reset();
    g_RenderTargetView.Reset();
    g_RenderTarget.Reset();
    g_D3DContext1.Reset();
    g_D3DContext.Reset();
    g_D3DDevice.Reset();
    g_SwapChain.Reset();

    for (auto& texture : g_CaptureTextures) {
        texture.Reset();
    }

    CleanupCapture();

    glfwDestroyWindow(g_Window);
    glfwTerminate();

    WaitForSingleObject(hHookThread, INFINITE);
    CloseHandle(hHookThread);

    CoUninitialize();

    return 0;
}