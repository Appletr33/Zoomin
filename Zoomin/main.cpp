#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>  // Using DXGI 1.6 for fullscreen compatibility
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

// GLFW includes
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

using Microsoft::WRL::ComPtr;

// Global running flag, window dimensions and swap chain
std::atomic<bool> g_Running = true;
GLFWwindow* g_Window = nullptr;
int g_ScreenWidth = 0;
int g_ScreenHeight = 0;
ComPtr<IDXGISwapChain1> g_SwapChain;  // Global swap chain

// DirectX resources
ComPtr<ID3D11Device>             g_D3DDevice;
ComPtr<ID3D11DeviceContext>      g_D3DContext;
ComPtr<IDXGIOutputDuplication>   g_OutputDuplication;
ComPtr<ID3D11Texture2D>          g_RenderTarget;
ComPtr<ID3D11RenderTargetView>   g_RenderTargetView;
ComPtr<ID3D11ShaderResourceView> g_FrameShaderResourceView;
ComPtr<ID3D11SamplerState>       g_SamplerState;
ComPtr<ID3D11VertexShader>       g_VertexShader;
ComPtr<ID3D11PixelShader>        g_PixelShader;
ComPtr<ID3D11Buffer>             g_VertexBuffer;
ComPtr<ID3D11InputLayout>        g_InputLayout;
ComPtr<ID3D11Texture2D>          g_StagingTexture;  // For CPU access

// Global zoom factor (starts at 1.0)
float g_CurrentZoom = 1.0f;

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
// The window is hidden immediately after creation.
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

// Create the swap chain.
bool CreateSwapChain()
{
    HWND hwnd = glfwGetWin32Window(g_Window);
    if (!hwnd)
    {
        std::cerr << "Could not retrieve HWND for swap chain creation." << std::endl;
        return false;
    }
    if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        std::cerr << "Failed to set window display affinity." << std::endl;
    }
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = g_ScreenWidth;
    swapChainDesc.Height = g_ScreenHeight;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGIDevice2> dxgiDevice2;
    HRESULT hr = g_D3DDevice.As(&dxgiDevice2);
    if (FAILED(hr))
    {
        std::cerr << "Failed to query IDXGIDevice2: " << HrToString(hr) << std::endl;
        return false;
    }
    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice2->GetAdapter(&dxgiAdapter);
    if (FAILED(hr))
    {
        std::cerr << "Failed to get adapter from IDXGIDevice2: " << HrToString(hr) << std::endl;
        return false;
    }
    ComPtr<IDXGIFactory2> dxgiFactory2;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory2));
    if (FAILED(hr))
    {
        std::cerr << "Failed to get factory from adapter: " << HrToString(hr) << std::endl;
        return false;
    }
    hr = dxgiFactory2->CreateSwapChainForHwnd(g_D3DDevice.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &g_SwapChain);
    if (FAILED(hr))
    {
        std::cerr << "Failed to create swap chain: " << HrToString(hr) << std::endl;
        return false;
    }
    return true;
}

// Initialize DirectX with DXGI 1.6 for improved fullscreen support.
bool InitializeDirectX() {

    std::cout << "Waiting 5 seconds before initializing capture..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    HRESULT hr = S_OK;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &g_D3DDevice, &featureLevel, &g_D3DContext);

    if (FAILED(hr))
    {
        if (createDeviceFlags & D3D11_CREATE_DEVICE_DEBUG)
        {
            createDeviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                &g_D3DDevice, &featureLevel, &g_D3DContext);
        }
        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
                featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                &g_D3DDevice, &featureLevel, &g_D3DContext);
            if (FAILED(hr))
            {
                std::cerr << "D3D11CreateDevice failed with all attempts: " << HrToString(hr) << std::endl;
                CoUninitialize();
                return false;
            }
        }
    }

    ComPtr<IDXGIDevice1> dxgiDevice1;
    hr = g_D3DDevice.As(&dxgiDevice1);
    if (SUCCEEDED(hr))
    {
        dxgiDevice1->SetMaximumFrameLatency(1);
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = g_D3DDevice.As(&dxgiDevice);
    if (FAILED(hr))
    {
        std::cerr << "Query IDXGIDevice failed: " << HrToString(hr) << std::endl;
        CoUninitialize();
        return false;
    }
    ComPtr<IDXGIAdapter> tempAdapter;
    hr = dxgiDevice->GetAdapter(&tempAdapter);
    if (FAILED(hr))
    {
        std::cerr << "GetAdapter failed: " << HrToString(hr) << std::endl;
        CoUninitialize();
        return false;
    }
    ComPtr<IDXGIAdapter1> dxgiAdapter;
    hr = tempAdapter.As(&dxgiAdapter);
    if (FAILED(hr))
    {
        std::cerr << "Query IDXGIAdapter1 failed: " << HrToString(hr) << std::endl;
        CoUninitialize();
        return false;
    }

    ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr))
    {
        std::cerr << "GetParent for IDXGIFactory2 failed: " << HrToString(hr) << std::endl;
        CoUninitialize();
        return false;
    }

    ComPtr<IDXGIFactory6> dxgiFactory6;
    hr = dxgiFactory.As(&dxgiFactory6);
    if (SUCCEEDED(hr))
    {
        std::cout << "Using DXGI 1.6 for better compatibility" << std::endl;
    }

    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    ComPtr<IDXGIAdapter1> currentAdapter;
    for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &currentAdapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        adapters.push_back(currentAdapter);
        currentAdapter = nullptr;
    }
    if (adapters.empty())
    {
        std::cerr << "No adapters found!" << std::endl;
        CoUninitialize();
        return false;
    }
    ComPtr<IDXGIOutput> dxgiOutput;
    ComPtr<IDXGIOutput6> dxgiOutput6;
    for (const auto& adapter : adapters)
    {
        DXGI_ADAPTER_DESC1 adapterDesc;
        adapter->GetDesc1(&adapterDesc);
        std::wcout << L"Trying adapter: " << adapterDesc.Description << std::endl;
        for (UINT i = 0; adapter->EnumOutputs(i, &dxgiOutput) != DXGI_ERROR_NOT_FOUND; i++)
        {
            DXGI_OUTPUT_DESC outputDesc;
            dxgiOutput->GetDesc(&outputDesc);
            std::wcout << L"  Trying output: " << outputDesc.DeviceName << std::endl;
            hr = dxgiOutput.As(&dxgiOutput6);
            if (FAILED(hr))
            {
                std::cout << "    Query IDXGIOutput6 failed: " << HrToString(hr) << std::endl;
                continue;
            }
            hr = dxgiOutput6->DuplicateOutput(g_D3DDevice.Get(), &g_OutputDuplication);
            if (SUCCEEDED(hr))
            {
                std::cout << "    Successfully created output duplication!" << std::endl;
                break;
            }
            else {
                const DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
                hr = dxgiOutput6->DuplicateOutput1(g_D3DDevice.Get(), 0, ARRAYSIZE(formats), formats, &g_OutputDuplication);
                if (SUCCEEDED(hr))
                {
                    std::cout << "    Successfully created output duplication with DuplicateOutput1!" << std::endl;
                    break;
                }
                std::cout << "    DuplicateOutput failed: " << HrToString(hr) << std::endl;
            }
            dxgiOutput = nullptr;
            dxgiOutput6 = nullptr;
        }
        if (g_OutputDuplication)
            break;
    }
    if (!g_OutputDuplication)
    {
        std::cerr << "Failed to create output duplication on any adapter/output combination." << std::endl;
        std::cerr << "This could be due to UAC elevation requirements or protected content." << std::endl;
        CoUninitialize();
        return false;
    }

    DXGI_OUTDUPL_DESC outputDuplDesc;
    g_OutputDuplication->GetDesc(&outputDuplDesc);
    std::cout << "Capturing at: " << outputDuplDesc.ModeDesc.Width << "x"
        << outputDuplDesc.ModeDesc.Height << " @ "
        << (outputDuplDesc.ModeDesc.RefreshRate.Numerator / outputDuplDesc.ModeDesc.RefreshRate.Denominator)
        << " Hz" << std::endl;

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
    if (FAILED(hr))
    {
        std::cerr << "Create render target texture failed: " << HrToString(hr) << std::endl;
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtDesc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    hr = g_D3DDevice->CreateRenderTargetView(g_RenderTarget.Get(), &rtvDesc, &g_RenderTargetView);
    if (FAILED(hr))
    {
        std::cerr << "Create render target view failed: " << HrToString(hr) << std::endl;
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = outputDuplDesc.ModeDesc.Width;
    stagingDesc.Height = outputDuplDesc.ModeDesc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = outputDuplDesc.ModeDesc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    hr = g_D3DDevice->CreateTexture2D(&stagingDesc, nullptr, &g_StagingTexture);
    if (FAILED(hr))
    {
        std::cerr << "Failed to create staging texture: " << HrToString(hr) << std::endl;
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_D3DDevice->CreateSamplerState(&sampDesc, &g_SamplerState);
    if (FAILED(hr))
    {
        std::cerr << "Create sampler state failed: " << HrToString(hr) << std::endl;
        return false;
    }

    if (!CreateSwapChain())
        return false;

    return true;
}

// Create shaders and initialize the constant buffer with a 1.0x zoom.
bool CreateShaders() {
    HRESULT hr = S_OK;
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
            float2 center = float2(0.5, 0.5);
            float2 dir = input.texCoord - center;
            dir = dir / magnificationFactor;
            float2 zoomedCoord = center + dir;
            if (zoomedCoord.x >= 0.0 && zoomedCoord.x <= 1.0 &&
                zoomedCoord.y >= 0.0 && zoomedCoord.y <= 1.0) {
                return frameTexture.Sample(frameSampler, zoomedCoord);
            }
            else {
                return float4(0.0, 0.0, 0.0, 0.0);
            }
        }
    )";

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompile(vsCode, strlen(vsCode), "VertexShader", nullptr, nullptr, "main", "vs_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
            std::cerr << "Vertex shader compilation failed: " << static_cast<char*>(errorBlob->GetBufferPointer()) << std::endl;
        return false;
    }
    hr = g_D3DDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_VertexShader);
    if (FAILED(hr))
    {
        std::cerr << "Create vertex shader failed: " << HrToString(hr) << std::endl;
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                      D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12,                     D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    UINT numElements = ARRAYSIZE(layout);
    hr = g_D3DDevice->CreateInputLayout(layout, numElements, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_InputLayout);
    if (FAILED(hr))
    {
        std::cerr << "Create input layout failed: " << HrToString(hr) << std::endl;
        return false;
    }

    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompile(psCode, strlen(psCode), "PixelShader", nullptr, nullptr, "main", "ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
            std::cerr << "Pixel shader compilation failed: " << static_cast<char*>(errorBlob->GetBufferPointer()) << std::endl;
        return false;
    }
    hr = g_D3DDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_PixelShader);
    if (FAILED(hr))
    {
        std::cerr << "Create pixel shader failed: " << HrToString(hr) << std::endl;
        return false;
    }

    Vertex vertices[6] = {
        { {-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f} },
        { { 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f} },
        { {-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f} },
        { {-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f} },
        { { 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f} },
        { { 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f} }
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = vertices;
    hr = g_D3DDevice->CreateBuffer(&vbDesc, &vbData, &g_VertexBuffer);
    if (FAILED(hr))
    {
        std::cerr << "Create vertex buffer failed: " << HrToString(hr) << std::endl;
        return false;
    }

    D3D11_BUFFER_DESC constantBufferDesc = {};
    constantBufferDesc.ByteWidth = sizeof(MagnificationConstantBuffer);
    constantBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantBufferDesc.CPUAccessFlags = 0;
    MagnificationConstantBuffer initialData = { 1.0f, {0.0f, 0.0f, 0.0f} };
    D3D11_SUBRESOURCE_DATA constantBufferData = {};
    constantBufferData.pSysMem = &initialData;
    hr = g_D3DDevice->CreateBuffer(&constantBufferDesc, &constantBufferData, &g_ConstantBuffer);
    if (FAILED(hr))
    {
        std::cerr << "Create constant buffer failed: " << HrToString(hr) << std::endl;
        return false;
    }
    return true;
}

// Render the current frame by setting the viewport, clearing the render target,
// setting up the pipeline, drawing the quad, and presenting the frame.
void RenderCurrentFrame() {
    HWND hwnd = glfwGetWin32Window(g_Window);
    RECT rect;
    GetClientRect(hwnd, &rect);
    unsigned int width = rect.right - rect.left;
    unsigned int height = rect.bottom - rect.top;
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    g_D3DContext->RSSetViewports(1, &viewport);

    g_D3DContext->OMSetRenderTargets(1, g_RenderTargetView.GetAddressOf(), nullptr);
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_D3DContext->ClearRenderTargetView(g_RenderTargetView.Get(), clearColor);

    g_D3DContext->IASetInputLayout(g_InputLayout.Get());
    g_D3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    g_D3DContext->IASetVertexBuffers(0, 1, g_VertexBuffer.GetAddressOf(), &stride, &offset);
    g_D3DContext->VSSetShader(g_VertexShader.Get(), nullptr, 0);
    g_D3DContext->PSSetShader(g_PixelShader.Get(), nullptr, 0);
    g_D3DContext->PSSetSamplers(0, 1, g_SamplerState.GetAddressOf());
    g_D3DContext->PSSetConstantBuffers(0, 1, g_ConstantBuffer.GetAddressOf());
    g_D3DContext->Draw(6, 0);

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr))
    {
        g_D3DContext->CopyResource(backBuffer.Get(), g_RenderTarget.Get());
        g_SwapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    }
    else
    {
        std::cout << "Failed to get back buffer from swap chain: " << HrToString(hr) << std::endl;
    }
}

// Process the frame: update the zoom based on the right mouse button,
// capture the desktop frame, update the shader resource, and render.
bool ProcessFrame() {
    HRESULT hr = S_OK;
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt_ms = std::chrono::duration<float, std::milli>(now - lastTime).count();
    lastTime = now;

    bool isRMB = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    float targetZoom = isRMB ? 1.4f : 1.0f;
    const float smoothingTime = 100.0f;
    float t = dt_ms / smoothingTime;
    if (t > 1.0f) t = 1.0f;
    g_CurrentZoom = g_CurrentZoom + (targetZoom - g_CurrentZoom) * t;

    MagnificationConstantBuffer cbData = { g_CurrentZoom, {0.0f, 0.0f, 0.0f} };
    g_D3DContext->UpdateSubresource(g_ConstantBuffer.Get(), 0, nullptr, &cbData, 0, 0);

    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    hr = g_OutputDuplication->AcquireNextFrame(25, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        RenderCurrentFrame();
        return true;
    }
    else if (FAILED(hr))
    {
        if (hr == DXGI_ERROR_ACCESS_LOST)
        {
            g_OutputDuplication.Reset();
            std::cout << "Access lost to desktop duplication, attempting to continue..." << std::endl;
            return true;
        }
        else
        {
            std::cout << "AcquireNextFrame failed: " << HrToString(hr) << std::endl;
            return true;
        }
    }

    ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    if (FAILED(hr))
    {
        std::cout << "Failed to QI for ID3D11Texture2D: " << HrToString(hr) << std::endl;
        g_OutputDuplication->ReleaseFrame();
        return true;
    }

    D3D11_TEXTURE2D_DESC textureDesc = {};
    desktopTexture->GetDesc(&textureDesc);

    if (g_FrameShaderResourceView == nullptr)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        hr = g_D3DDevice->CreateShaderResourceView(desktopTexture.Get(), &srvDesc, &g_FrameShaderResourceView);
        if (FAILED(hr))
        {
            std::cout << "Create shader resource view failed: " << HrToString(hr) << std::endl;
            g_OutputDuplication->ReleaseFrame();
            return true;
        }
    }
    else
    {
        g_D3DContext->CopyResource(g_StagingTexture.Get(), desktopTexture.Get());
    }

    g_D3DContext->PSSetShaderResources(0, 1, g_FrameShaderResourceView.GetAddressOf());
    RenderCurrentFrame();

    hr = g_OutputDuplication->ReleaseFrame();
    if (FAILED(hr))
        std::cout << "ReleaseFrame failed: " << HrToString(hr) << std::endl;

    return true;
}

// Main function: initializes GLFW, DirectX, shaders, and enters the frame loop.
// The window remains hidden until toggled with Numpad 8.
int main() {
    if (!InitializeGLFW())
        return -1;

    // Start the keyboard hook thread to detect Shift+Esc and Numpad 8.
    DWORD threadId;
    HANDLE hHookThread = CreateThread(nullptr, 0, KeyboardHookThread, nullptr, 0, &threadId);
    if (!hHookThread)
    {
        std::cerr << "Failed to create keyboard hook thread." << std::endl;
        return -1;
    }

    if (!InitializeDirectX())
    {
        glfwTerminate();
        return -1;
    }
    if (!CreateShaders())
    {
        glfwTerminate();
        return -1;
    }
    std::cout << "Screen Magnifier initialized. Hold right-click to zoom; press Shift+ESC to exit. Toggle window visibility with Numpad 8." << std::endl;

    using clock = std::chrono::high_resolution_clock;
    auto lastFrameTime = clock::now();
    const auto targetFrameTime = std::chrono::microseconds(8333);

    int errors = 0;
    while (g_Running && !glfwWindowShouldClose(g_Window))
    {
        auto frameStart = clock::now();

        if (!ProcessFrame())
            std::cout << "Error processing frame x" << ++errors << std::endl;

        glfwPollEvents();

        // Check for a toggle request and update window visibility accordingly.
        if (g_WindowToggleRequest)
        {
            if (g_WindowVisible)
            {
                glfwHideWindow(g_Window);
                g_WindowVisible = false;
            }
            else
            {
                glfwShowWindow(g_Window);
                g_WindowVisible = true;
            }
            g_WindowToggleRequest = false;
        }

        auto frameEnd = clock::now();
        auto frameDuration = frameEnd - frameStart;
        if (frameDuration < targetFrameTime)
        {
            auto sleepTime = std::min(
                std::chrono::duration_cast<std::chrono::nanoseconds>(targetFrameTime - frameDuration),
                std::chrono::nanoseconds(1)
            );
            std::this_thread::sleep_for(sleepTime);
        }
    }

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
    g_OutputDuplication.Reset();
    g_StagingTexture.Reset();
    g_D3DContext.Reset();
    g_D3DDevice.Reset();
    g_SwapChain.Reset();

    glfwDestroyWindow(g_Window);
    glfwTerminate();
    WaitForSingleObject(hHookThread, INFINITE);
    CloseHandle(hHookThread);
    CoUninitialize();
    return 0;
}