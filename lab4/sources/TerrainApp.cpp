#include "TerrainApp.h"

#include "DDSTextureLoader.h"

#include <algorithm>
#include <string>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

const int gNumFrameResources = 1;

TerrainApp::TerrainApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
    , mVoxelWorld(VoxelWorldSettings{})
{
    mMainWndCaption = L"Marching Cubes Landscape";

    const auto& settings = mVoxelWorld.Settings();
    const UINT cellCount =
        static_cast<UINT>((settings.DimX - 1) * (settings.DimY - 1) * (settings.DimZ - 1));
    mMaxMCVertices = cellCount * 15u;
}

TerrainApp::~TerrainApp()
{
    if (mCursorHidden)
    {
        while (::ShowCursor(TRUE) < 0)
        {
        }
        mCursorHidden = false;
    }

    if (md3dDevice != nullptr)
    {
        FlushCommandQueue();
    }
}

bool TerrainApp::Initialize()
{
    if (!D3DApp::Initialize())
    {
        return false;
    }

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCamera.SetPosition(0.0f, 80.0f, -140.0f);
    mCamera.RotatePitch(14.0f);
    mCamera.SetMoveSpeed(12.0f);
    mCamera.SetProjectionValues(60.0f, AspectRatio(), 0.5f, 2000.0f);

    mTemporalAA = std::make_unique<TemporalAA>(
        md3dDevice.Get(),
        static_cast<UINT>(mClientWidth),
        static_cast<UINT>(mClientHeight),
        mBackBufferFormat);

    mVoxelWorld.GenerateAll();

    CreateRootSignatures();
    CreateTAARootSignature();
    CompileShaders();
    CreatePipelineStates();
    CreateDrawCommandSignature();

    LoadAlbedoTextures();
    CreateMarchingResources();
    CreateChunkDensityTextures();
    CreateDescriptorHeap();

    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(md3dDevice.Get(), 1, true);
    mChunkCB = std::make_unique<UploadBuffer<MCChunkConstants>>(
        md3dDevice.Get(),
        static_cast<UINT>(mVoxelWorld.Chunks().size()),
        true);
    mTablesCB = std::make_unique<UploadBuffer<MarchingTablesConstants>>(md3dDevice.Get(), 1, true);
    mTAACB = std::make_unique<UploadBuffer<TAAConstants>>(md3dDevice.Get(), 1, true);
    mTablesCB->CopyData(0, BuildMarchingTablesConstants());

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);

    FlushCommandQueue();

    if (mStoneTexture)
    {
        mStoneTexture->UploadHeap.Reset();
    }

    if (mGrassTexture)
    {
        mGrassTexture->UploadHeap.Reset();
    }

    for (VoxelChunk& chunk : mVoxelWorld.Chunks())
    {
        chunk.DensityUpload.Reset();
    }

    mDrawArgsUpload.Reset();

    return true;
}

void TerrainApp::OnResize()
{
    D3DApp::OnResize();
    mCamera.SetProjectionValues(60.0f, AspectRatio(), 0.5f, 2000.0f);

    if (mTemporalAA != nullptr)
    {
        mTemporalAA->OnResize(static_cast<UINT>(mClientWidth), static_cast<UINT>(mClientHeight));
        CreateDescriptorHeap();
        mTAAFrameIndex = 0;
    }
}

void TerrainApp::Update(const GameTimer& gt)
{
    if (mCameraModeActive)
    {
        UpdateCameraLookFromCursor();

        const bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (lmbDown)
        {
            const double now = gt.TotalTime();
            if (mLastDigTime < 0.0 || (now - mLastDigTime) >= mDigIntervalSeconds)
            {
                ApplyDigging();
                mLastDigTime = now;
            }
        }
        else
        {
            mLastDigTime = -1.0;
        }
    }
    else
    {
        mLastDigTime = -1.0;
    }

    UpdatePassConstants();
    UpdateTAAConstants();
    CullChunks();
}

void TerrainApp::Draw(const GameTimer& gt)
{
    (void)gt;

    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvUavHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    for (VoxelChunk& chunk : mVoxelWorld.Chunks())
    {
        if (chunk.Dirty)
        {
            UploadChunkDensity(chunk);
        }
    }

    TransitionResource(mSceneColorBuffer.Get(), mSceneColorState, D3D12_RESOURCE_STATE_RENDER_TARGET);

    CD3DX12_CPU_DESCRIPTOR_HANDLE sceneRtv(
        mOffscreenRtvHeap->GetCPUDescriptorHandleForHeapStart(),
        static_cast<INT>(mSceneColorRtvIndex),
        mRtvDescriptorSize);

    const float clearColor[] = { 0.61f, 0.78f, 0.93f, 1.0f };
    mCommandList->ClearRenderTargetView(sceneRtv, clearColor, 0, nullptr);
    mCommandList->ClearDepthStencilView(
        DepthStencilView(),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f,
        0,
        0,
        nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = DepthStencilView();
    mCommandList->OMSetRenderTargets(1, &sceneRtv, true, &dsv);

    for (VoxelChunk* chunk : mVisibleChunks)
    {
        RenderChunk(*chunk);
    }

    TransitionResource(mSceneColorBuffer.Get(), mSceneColorState, D3D12_RESOURCE_STATE_GENERIC_READ);

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));

    if (mTAAEnabled)
    {
        ResolveTAA();
        TransitionResource(mTemporalAA->Resource(), mTAAOutputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        mCommandList->CopyResource(CurrentBackBuffer(), mTemporalAA->Resource());
        TransitionResource(mTemporalAA->Resource(), mTAAOutputState, D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    else
    {
        TransitionResource(mSceneColorBuffer.Get(), mSceneColorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        mCommandList->CopyResource(CurrentBackBuffer(), mSceneColorBuffer.Get());
        TransitionResource(mSceneColorBuffer.Get(), mSceneColorState, D3D12_RESOURCE_STATE_GENERIC_READ);
    }

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(mCommandList->Close());

    ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    FlushCommandQueue();

    for (VoxelChunk& chunk : mVoxelWorld.Chunks())
    {
        chunk.DensityUpload.Reset();
    }

    if (mTAAEnabled)
    {
        ++mTAAFrameIndex;
    }
    else
    {
        mTAAFrameIndex = 0;
    }
}

void TerrainApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    if ((btnState & MK_RBUTTON) != 0)
    {
        SetCameraMode(true);
    }
}

void TerrainApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    (void)x;
    (void)y;

    if ((btnState & MK_RBUTTON) == 0)
    {
        SetCameraMode(false);
    }
}

void TerrainApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    (void)btnState;
    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void TerrainApp::OnKeyPressed(const GameTimer& gt, WPARAM key)
{
    (void)gt;

    const bool rmbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const short wheelDelta = GET_WHEEL_DELTA_WPARAM(key);

    if (wheelDelta != 0)
    {
        if (rmbDown)
        {
            float speed = mCamera.GetMoveSpeed();
            speed += wheelDelta > 0 ? 1.0f : -1.0f;
            speed = std::clamp(speed, 0.1f, 1000.0f);
            mCamera.SetMoveSpeed(speed);
        }

        return;
    }

    if (rmbDown)
    {
        switch (key)
        {
        case 'W':
            mCamera.MoveForward();
            return;
        case 'S':
            mCamera.MoveBackward();
            return;
        case 'A':
            mCamera.MoveLeft();
            return;
        case 'D':
            mCamera.MoveRight();
            return;
        case VK_SHIFT:
            mCamera.MoveDown();
            return;
        case VK_SPACE:
            mCamera.MoveUp();
            return;
        default:
            break;
        }
    }

    switch (key)
    {
    case 'I':
        mVoxelWorld.Settings().IsoLevel += 0.03f;
        break;
    case 'K':
        mVoxelWorld.Settings().IsoLevel -= 0.03f;
        break;
    case 'G':
        ApplyDigging();
        break;
    case 'T':
        mTAAEnabled = !mTAAEnabled;
        mTAAFrameIndex = 0;
        break;
    case 'Y':
        mWireframeMode = !mWireframeMode;
        break;
    case VK_ADD:
    case VK_OEM_PLUS:
        mTexScale = (std::min)(mTexScale + 0.01f, 0.5f);
        break;
    case VK_SUBTRACT:
    case VK_OEM_MINUS:
        mTexScale = (std::max)(mTexScale - 0.01f, 0.01f);
        break;
    default:
        break;
    }
}

void TerrainApp::CreateRootSignatures()
{
    {
        CD3DX12_DESCRIPTOR_RANGE textureRange;
        textureRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

        CD3DX12_ROOT_PARAMETER rootParams[2];
        rootParams[0].InitAsConstantBufferView(0);
        rootParams[1].InitAsDescriptorTable(1, &textureRange, D3D12_SHADER_VISIBILITY_PIXEL);

        auto staticSamplers = GetStaticSamplers();

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            _countof(rootParams),
            rootParams,
            static_cast<UINT>(staticSamplers.size()),
            staticSamplers.data(),
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serializedRootSig = nullptr;
        ComPtr<ID3DBlob> errorBlob = nullptr;

        HRESULT hr = D3D12SerializeRootSignature(
            &rootSigDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(),
            errorBlob.GetAddressOf());

        if (errorBlob != nullptr)
        {
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        }

        ThrowIfFailed(hr);

        ThrowIfFailed(md3dDevice->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(mGraphicsRootSignature.GetAddressOf())));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE densityRange;
        densityRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_DESCRIPTOR_RANGE outputRange;
        outputRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);

        CD3DX12_ROOT_PARAMETER rootParams[4];
        rootParams[0].InitAsConstantBufferView(0);
        rootParams[1].InitAsConstantBufferView(1);
        rootParams[2].InitAsDescriptorTable(1, &densityRange);
        rootParams[3].InitAsDescriptorTable(1, &outputRange);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
            _countof(rootParams),
            rootParams,
            0,
            nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> serializedRootSig = nullptr;
        ComPtr<ID3DBlob> errorBlob = nullptr;

        HRESULT hr = D3D12SerializeRootSignature(
            &rootSigDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(),
            errorBlob.GetAddressOf());

        if (errorBlob != nullptr)
        {
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        }

        ThrowIfFailed(hr);

        ThrowIfFailed(md3dDevice->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(mComputeRootSignature.GetAddressOf())));
    }
}

void TerrainApp::CreateTAARootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE currentFrameRange;
    currentFrameRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE historyRange;
    historyRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

    CD3DX12_ROOT_PARAMETER rootParams[3];
    rootParams[0].InitAsConstantBufferView(0);
    rootParams[1].InitAsDescriptorTable(1, &currentFrameRange, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[2].InitAsDescriptorTable(1, &historyRange, D3D12_SHADER_VISIBILITY_PIXEL);

    auto staticSamplers = GetTAAStaticSamplers();

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        _countof(rootParams),
        rootParams,
        static_cast<UINT>(staticSamplers.size()),
        staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;

    HRESULT hr = D3D12SerializeRootSignature(
        &rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(),
        errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
    }

    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mTAARootSignature.GetAddressOf())));
}

void TerrainApp::CompileShaders()
{
    mShaders["voxelVS"] = d3dUtil::CompileShader(L"shaders/voxel_vs.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["voxelPS"] = d3dUtil::CompileShader(L"shaders/voxel_ps.hlsl", nullptr, "PS", "ps_5_0");
    mShaders["marchingCS"] = d3dUtil::CompileShader(L"shaders/voxel_mc_cs.hlsl", nullptr, "CS", "cs_5_0");
    mShaders["taaVS"] = d3dUtil::CompileShader(L"shaders/TAAResolve.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["taaPS"] = d3dUtil::CompileShader(L"shaders/TAAResolve.hlsl", nullptr, "PS", "ps_5_0");

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
}

void TerrainApp::CreatePipelineStates()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc = {};
    graphicsDesc.InputLayout = { mInputLayout.data(), static_cast<UINT>(mInputLayout.size()) };
    graphicsDesc.pRootSignature = mGraphicsRootSignature.Get();
    graphicsDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["voxelVS"]->GetBufferPointer()),
        mShaders["voxelVS"]->GetBufferSize()
    };
    graphicsDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["voxelPS"]->GetBufferPointer()),
        mShaders["voxelPS"]->GetBufferSize()
    };
    graphicsDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    graphicsDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    graphicsDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    graphicsDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    graphicsDesc.SampleMask = UINT_MAX;
    graphicsDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphicsDesc.NumRenderTargets = 1;
    graphicsDesc.RTVFormats[0] = mBackBufferFormat;
    graphicsDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    graphicsDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    graphicsDesc.DSVFormat = mDepthStencilFormat;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &graphicsDesc,
        IID_PPV_ARGS(&mPSOs["terrain"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireDesc = graphicsDesc;
    wireDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &wireDesc,
        IID_PPV_ARGS(&mPSOs["terrainWire"])));

    D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc = {};
    computeDesc.pRootSignature = mComputeRootSignature.Get();
    computeDesc.CS =
    {
        reinterpret_cast<BYTE*>(mShaders["marchingCS"]->GetBufferPointer()),
        mShaders["marchingCS"]->GetBufferSize()
    };

    ThrowIfFailed(md3dDevice->CreateComputePipelineState(
        &computeDesc,
        IID_PPV_ARGS(&mPSOs["marchingCubes"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC taaDesc = {};
    taaDesc.InputLayout = { nullptr, 0 };
    taaDesc.pRootSignature = mTAARootSignature.Get();
    taaDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["taaVS"]->GetBufferPointer()),
        mShaders["taaVS"]->GetBufferSize()
    };
    taaDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["taaPS"]->GetBufferPointer()),
        mShaders["taaPS"]->GetBufferSize()
    };
    taaDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    taaDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    taaDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    taaDesc.DepthStencilState.DepthEnable = FALSE;
    taaDesc.DepthStencilState.StencilEnable = FALSE;
    taaDesc.SampleMask = UINT_MAX;
    taaDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    taaDesc.NumRenderTargets = 1;
    taaDesc.RTVFormats[0] = mBackBufferFormat;
    taaDesc.SampleDesc.Count = 1;
    taaDesc.SampleDesc.Quality = 0;
    taaDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &taaDesc,
        IID_PPV_ARGS(&mPSOs["taa"])));
}

void TerrainApp::CreateDrawCommandSignature()
{
    D3D12_INDIRECT_ARGUMENT_DESC argumentDesc = {};
    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
    signatureDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signatureDesc.NumArgumentDescs = 1;
    signatureDesc.pArgumentDescs = &argumentDesc;

    ThrowIfFailed(md3dDevice->CreateCommandSignature(
        &signatureDesc,
        nullptr,
        IID_PPV_ARGS(mDrawCommandSignature.GetAddressOf())));
}

void TerrainApp::LoadAlbedoTextures()
{
    mStoneTexture = std::make_unique<Texture>();
    mStoneTexture->Name = "stone";
    mStoneTexture->Filename = L"assets/stone.dds";

    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
        md3dDevice.Get(),
        mCommandList.Get(),
        mStoneTexture->Filename.c_str(),
        mStoneTexture->Resource,
        mStoneTexture->UploadHeap));

    mGrassTexture = std::make_unique<Texture>();
    mGrassTexture->Name = "grass";
    mGrassTexture->Filename = L"assets/grass.dds";

    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
        md3dDevice.Get(),
        mCommandList.Get(),
        mGrassTexture->Filename.c_str(),
        mGrassTexture->Resource,
        mGrassTexture->UploadHeap));
}

void TerrainApp::CreateMarchingResources()
{
    const UINT64 vertexBufferBytes = static_cast<UINT64>(mMaxMCVertices) * sizeof(MCVertex);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(vertexBufferBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(mMCVertexBuffer.GetAddressOf())));

    mMCVertexState = D3D12_RESOURCE_STATE_COMMON;

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(mCounterBuffer.GetAddressOf())));

    mCounterState = D3D12_RESOURCE_STATE_COMMON;

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_DRAW_ARGUMENTS)),
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(mDrawArgsBuffer.GetAddressOf())));

    mDrawArgsState = D3D12_RESOURCE_STATE_COMMON;

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT)),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(mCounterResetUpload.GetAddressOf())));

    {
        UINT* mapped = nullptr;
        ThrowIfFailed(mCounterResetUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        mapped[0] = 0u;
        mCounterResetUpload->Unmap(0, nullptr);
    }

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_DRAW_ARGUMENTS)),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(mDrawArgsUpload.GetAddressOf())));

    {
        D3D12_DRAW_ARGUMENTS* mapped = nullptr;
        ThrowIfFailed(mDrawArgsUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        mapped[0].VertexCountPerInstance = 0;
        mapped[0].InstanceCount = 1;
        mapped[0].StartVertexLocation = 0;
        mapped[0].StartInstanceLocation = 0;
        mDrawArgsUpload->Unmap(0, nullptr);
    }

    TransitionResource(mDrawArgsBuffer.Get(), mDrawArgsState, D3D12_RESOURCE_STATE_COPY_DEST);
    mCommandList->CopyBufferRegion(
        mDrawArgsBuffer.Get(),
        0,
        mDrawArgsUpload.Get(),
        0,
        sizeof(D3D12_DRAW_ARGUMENTS));
    TransitionResource(mDrawArgsBuffer.Get(), mDrawArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    TransitionResource(mCounterBuffer.Get(), mCounterState, D3D12_RESOURCE_STATE_COPY_DEST);
    mCommandList->CopyBufferRegion(mCounterBuffer.Get(), 0, mCounterResetUpload.Get(), 0, sizeof(UINT));
    TransitionResource(mCounterBuffer.Get(), mCounterState, D3D12_RESOURCE_STATE_COMMON);

    mMCVBV.BufferLocation = mMCVertexBuffer->GetGPUVirtualAddress();
    mMCVBV.StrideInBytes = sizeof(MCVertex);
    mMCVBV.SizeInBytes = static_cast<UINT>(vertexBufferBytes);
}

void TerrainApp::CreateChunkDensityTextures()
{
    for (VoxelChunk& chunk : mVoxelWorld.Chunks())
    {
        D3D12_RESOURCE_DESC densityDesc = {};
        densityDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        densityDesc.Alignment = 0;
        densityDesc.Width = static_cast<UINT64>(chunk.DimX);
        densityDesc.Height = static_cast<UINT>(chunk.DimY);
        densityDesc.DepthOrArraySize = static_cast<UINT16>(chunk.DimZ);
        densityDesc.MipLevels = 1;
        densityDesc.Format = DXGI_FORMAT_R16_FLOAT;
        densityDesc.SampleDesc.Count = 1;
        densityDesc.SampleDesc.Quality = 0;
        densityDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        densityDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(md3dDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &densityDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(chunk.DensityTexture.GetAddressOf())));

        chunk.DensityState = D3D12_RESOURCE_STATE_COPY_DEST;
        UploadChunkDensity(chunk);
    }
}

void TerrainApp::CreateDescriptorHeap()
{
    const UINT chunkCount = static_cast<UINT>(mVoxelWorld.Chunks().size());

    mTextureSrvStart = 0;
    mChunkSrvStart = 3;
    mSceneColorSrvIndex = mChunkSrvStart + chunkCount;
    mTAAOutputSrvIndex = mSceneColorSrvIndex + 1;
    mTAAHistorySrvIndex = mSceneColorSrvIndex + 2;
    mVertexUavIndex = mSceneColorSrvIndex + 3;
    mCounterUavIndex = mVertexUavIndex + 1;

    const UINT descriptorCount = mCounterUavIndex + 1;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = descriptorCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &heapDesc,
        IID_PPV_ARGS(mSrvUavHeap.GetAddressOf())));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 3;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &rtvHeapDesc,
        IID_PPV_ARGS(mOffscreenRtvHeap.GetAddressOf())));

    D3D12_RESOURCE_DESC sceneDesc = {};
    sceneDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    sceneDesc.Alignment = 0;
    sceneDesc.Width = static_cast<UINT64>(mClientWidth);
    sceneDesc.Height = static_cast<UINT>(mClientHeight);
    sceneDesc.DepthOrArraySize = 1;
    sceneDesc.MipLevels = 1;
    sceneDesc.Format = mBackBufferFormat;
    sceneDesc.SampleDesc.Count = 1;
    sceneDesc.SampleDesc.Quality = 0;
    sceneDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    sceneDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    float clearColor[] = { 0.61f, 0.78f, 0.93f, 1.0f };
    CD3DX12_CLEAR_VALUE sceneClear(mBackBufferFormat, clearColor);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &sceneDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &sceneClear,
        IID_PPV_ARGS(mSceneColorBuffer.ReleaseAndGetAddressOf())));

    mSceneColorState = D3D12_RESOURCE_STATE_GENERIC_READ;
    mTAAOutputState = D3D12_RESOURCE_STATE_GENERIC_READ;
    mTAAHistoryState = D3D12_RESOURCE_STATE_GENERIC_READ;

    auto createTextureSrv = [&](ID3D12Resource* resource, UINT index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = resource->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = resource->GetDesc().MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            mSrvUavHeap->GetCPUDescriptorHandleForHeapStart(),
            index,
            mCbvSrvUavDescriptorSize);

        md3dDevice->CreateShaderResourceView(resource, &srvDesc, handle);
    };

    createTextureSrv(mStoneTexture->Resource.Get(), mTextureSrvStart + 0);
    createTextureSrv(mGrassTexture->Resource.Get(), mTextureSrvStart + 1);
    createTextureSrv(mStoneTexture->Resource.Get(), mTextureSrvStart + 2);

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = mBackBufferFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            mSrvUavHeap->GetCPUDescriptorHandleForHeapStart(),
            mSceneColorSrvIndex,
            mCbvSrvUavDescriptorSize);

        md3dDevice->CreateShaderResourceView(mSceneColorBuffer.Get(), &srvDesc, handle);
    }

    for (UINT i = 0; i < chunkCount; ++i)
    {
        VoxelChunk& chunk = mVoxelWorld.Chunks()[i];

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Texture3D.MostDetailedMip = 0;
        srvDesc.Texture3D.MipLevels = 1;
        srvDesc.Texture3D.ResourceMinLODClamp = 0.0f;

        const UINT descriptorIndex = mChunkSrvStart + i;
        chunk.DensitySrvIndex = descriptorIndex;

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            mSrvUavHeap->GetCPUDescriptorHandleForHeapStart(),
            descriptorIndex,
            mCbvSrvUavDescriptorSize);

        md3dDevice->CreateShaderResourceView(chunk.DensityTexture.Get(), &srvDesc, handle);
    }

    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = mMaxMCVertices;
        uavDesc.Buffer.StructureByteStride = sizeof(MCVertex);
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            mSrvUavHeap->GetCPUDescriptorHandleForHeapStart(),
            mVertexUavIndex,
            mCbvSrvUavDescriptorSize);

        md3dDevice->CreateUnorderedAccessView(mMCVertexBuffer.Get(), nullptr, &uavDesc, handle);
    }

    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = 1;
        uavDesc.Buffer.StructureByteStride = 0;
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            mSrvUavHeap->GetCPUDescriptorHandleForHeapStart(),
            mCounterUavIndex,
            mCbvSrvUavDescriptorSize);

        md3dDevice->CreateUnorderedAccessView(mCounterBuffer.Get(), nullptr, &uavDesc, handle);
    }

    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = mBackBufferFormat;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
            mOffscreenRtvHeap->GetCPUDescriptorHandleForHeapStart(),
            static_cast<INT>(mSceneColorRtvIndex),
            mRtvDescriptorSize);

        md3dDevice->CreateRenderTargetView(mSceneColorBuffer.Get(), &rtvDesc, rtvHandle);
    }

    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE taaSrvCpu(
            mSrvUavHeap->GetCPUDescriptorHandleForHeapStart(),
            mTAAOutputSrvIndex,
            mCbvSrvUavDescriptorSize);

        CD3DX12_GPU_DESCRIPTOR_HANDLE taaSrvGpu(
            mSrvUavHeap->GetGPUDescriptorHandleForHeapStart(),
            mTAAOutputSrvIndex,
            mCbvSrvUavDescriptorSize);

        CD3DX12_CPU_DESCRIPTOR_HANDLE taaRtvCpu(
            mOffscreenRtvHeap->GetCPUDescriptorHandleForHeapStart(),
            static_cast<INT>(mTAAOutputRtvIndex),
            mRtvDescriptorSize);

        mTemporalAA->BuildDescriptors(
            taaSrvCpu,
            taaSrvGpu,
            taaRtvCpu,
            mCbvSrvUavDescriptorSize,
            mRtvDescriptorSize);
    }
}

void TerrainApp::UploadChunkDensity(VoxelChunk& chunk)
{
    if (chunk.DensityTexture == nullptr)
    {
        return;
    }

    TransitionResource(chunk.DensityTexture.Get(), chunk.DensityState, D3D12_RESOURCE_STATE_COPY_DEST);

    const UINT64 uploadSize = GetRequiredIntermediateSize(chunk.DensityTexture.Get(), 0, 1);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(chunk.DensityUpload.ReleaseAndGetAddressOf())));

    D3D12_SUBRESOURCE_DATA subresource = {};
    subresource.pData = chunk.DensityHalf.data();
    subresource.RowPitch = static_cast<LONG_PTR>(chunk.DimX * sizeof(std::uint16_t));
    subresource.SlicePitch = subresource.RowPitch * chunk.DimY;

    UpdateSubresources<1>(
        mCommandList.Get(),
        chunk.DensityTexture.Get(),
        chunk.DensityUpload.Get(),
        0,
        0,
        1,
        &subresource);

    TransitionResource(
        chunk.DensityTexture.Get(),
        chunk.DensityState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    chunk.Dirty = false;
}

void TerrainApp::UpdatePassConstants()
{
    XMMATRIX view = mCamera.GetViewMatrix();
    XMMATRIX proj = mCamera.GetProjectionMatrix();

    if (mTAAEnabled)
    {
        XMFLOAT2 jitter = TemporalAA::GetJitter(mTAAFrameIndex);
        float jitterX = (jitter.x * 2.0f) / static_cast<float>(mClientWidth);
        float jitterY = (jitter.y * 2.0f) / static_cast<float>(mClientHeight);

        XMFLOAT4X4 projMat;
        XMStoreFloat4x4(&projMat, proj);
        projMat._13 += jitterX;
        projMat._23 += jitterY;
        proj = XMLoadFloat4x4(&projMat);
    }

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    PassConstants passConstants;
    XMStoreFloat4x4(&passConstants.View, view);
    XMStoreFloat4x4(&passConstants.Proj, proj);
    XMStoreFloat4x4(&passConstants.ViewProj, viewProj);

    passConstants.EyePosW = mCamera.GetPosition();
    passConstants.IsoLevel = mVoxelWorld.Settings().IsoLevel;
    passConstants.TexScale = mTexScale;
    passConstants.Ambient = 0.22f;
    passConstants.LightDir = XMFLOAT3(0.35f, -0.9f, 0.22f);

    mPassCB->CopyData(0, passConstants);
}

void TerrainApp::UpdateTAAConstants()
{
    TAAConstants taaConstants;
    taaConstants.JitterOffset = TemporalAA::GetJitter(mTAAFrameIndex);
    taaConstants.ScreenSize = XMFLOAT2(static_cast<float>(mClientWidth), static_cast<float>(mClientHeight));
    taaConstants.BlendFactor = (mTAAFrameIndex == 0) ? 1.0f : 0.1f;

    mTAACB->CopyData(0, taaConstants);
}

void TerrainApp::CullChunks()
{
    mVisibleChunks.clear();
    for (VoxelChunk& chunk : mVoxelWorld.Chunks())
    {
        mVisibleChunks.push_back(&chunk);
    }
}

BoundingFrustum TerrainApp::BuildFrustum() const
{
    return mCamera.GetFrustum();
}

void TerrainApp::RenderChunk(VoxelChunk& chunk)
{
    MCChunkConstants chunkConstants;
    chunkConstants.WorldOrigin = chunk.WorldOrigin;
    chunkConstants.VoxelSize = chunk.VoxelSize;
    chunkConstants.DimX = static_cast<UINT>(chunk.DimX);
    chunkConstants.DimY = static_cast<UINT>(chunk.DimY);
    chunkConstants.DimZ = static_cast<UINT>(chunk.DimZ);
    chunkConstants.IsoLevel = mVoxelWorld.Settings().IsoLevel;
    chunkConstants.MaxVertexCount = mMaxMCVertices;

    mChunkCB->CopyData(static_cast<int>(chunk.LinearIndex), chunkConstants);

    const UINT chunkCbStride = d3dUtil::CalcConstantBufferByteSize(sizeof(MCChunkConstants));
    const D3D12_GPU_VIRTUAL_ADDRESS chunkCbAddress =
        mChunkCB->Resource()->GetGPUVirtualAddress() +
        static_cast<UINT64>(chunk.LinearIndex) * static_cast<UINT64>(chunkCbStride);

    TransitionResource(mCounterBuffer.Get(), mCounterState, D3D12_RESOURCE_STATE_COPY_DEST);
    mCommandList->CopyBufferRegion(mCounterBuffer.Get(), 0, mCounterResetUpload.Get(), 0, sizeof(UINT));
    TransitionResource(mCounterBuffer.Get(), mCounterState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    TransitionResource(mMCVertexBuffer.Get(), mMCVertexState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());
    mCommandList->SetPipelineState(mPSOs["marchingCubes"].Get());

    mCommandList->SetComputeRootConstantBufferView(0, chunkCbAddress);
    mCommandList->SetComputeRootConstantBufferView(1, mTablesCB->Resource()->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE densityHandle(
        mSrvUavHeap->GetGPUDescriptorHandleForHeapStart(),
        chunk.DensitySrvIndex,
        mCbvSrvUavDescriptorSize);
    mCommandList->SetComputeRootDescriptorTable(2, densityHandle);

    CD3DX12_GPU_DESCRIPTOR_HANDLE outputHandle(
        mSrvUavHeap->GetGPUDescriptorHandleForHeapStart(),
        mVertexUavIndex,
        mCbvSrvUavDescriptorSize);
    mCommandList->SetComputeRootDescriptorTable(3, outputHandle);

    const UINT groupsX = (chunkConstants.DimX - 1 + 7u) / 8u;
    const UINT groupsY = (chunkConstants.DimY - 1 + 7u) / 8u;
    const UINT groupsZ = (chunkConstants.DimZ - 1 + 7u) / 8u;

    mCommandList->Dispatch(groupsX, groupsY, groupsZ);

    D3D12_RESOURCE_BARRIER uavBarriers[2] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(mMCVertexBuffer.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(mCounterBuffer.Get())
    };
    mCommandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

    TransitionResource(mCounterBuffer.Get(), mCounterState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(mDrawArgsBuffer.Get(), mDrawArgsState, D3D12_RESOURCE_STATE_COPY_DEST);
    mCommandList->CopyBufferRegion(mDrawArgsBuffer.Get(), 0, mCounterBuffer.Get(), 0, sizeof(UINT));
    TransitionResource(mDrawArgsBuffer.Get(), mDrawArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    TransitionResource(
        mMCVertexBuffer.Get(),
        mMCVertexState,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    mCommandList->SetGraphicsRootSignature(mGraphicsRootSignature.Get());
    mCommandList->SetPipelineState(mPSOs[mWireframeMode ? "terrainWire" : "terrain"].Get());

    mCommandList->SetGraphicsRootConstantBufferView(0, mPassCB->Resource()->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE textureHandle(
        mSrvUavHeap->GetGPUDescriptorHandleForHeapStart(),
        mTextureSrvStart,
        mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1, textureHandle);

    mCommandList->IASetVertexBuffers(0, 1, &mMCVBV);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    mCommandList->ExecuteIndirect(
        mDrawCommandSignature.Get(),
        1,
        mDrawArgsBuffer.Get(),
        0,
        nullptr,
        0);
}

void TerrainApp::ResolveTAA()
{
    if (mTAAFrameIndex == 0)
    {
        TransitionResource(mTemporalAA->HistoryResource(), mTAAHistoryState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float clearColor[] = { 0.61f, 0.78f, 0.93f, 1.0f };
        mCommandList->ClearRenderTargetView(mTemporalAA->HistoryRtv(), clearColor, 0, nullptr);
        TransitionResource(mTemporalAA->HistoryResource(), mTAAHistoryState, D3D12_RESOURCE_STATE_GENERIC_READ);
    }

    TransitionResource(mTemporalAA->Resource(), mTAAOutputState, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_VIEWPORT taaViewport = mTemporalAA->Viewport();
    D3D12_RECT taaScissor = mTemporalAA->ScissorRect();
    mCommandList->RSSetViewports(1, &taaViewport);
    mCommandList->RSSetScissorRects(1, &taaScissor);

    D3D12_CPU_DESCRIPTOR_HANDLE taaRtv = mTemporalAA->Rtv();
    mCommandList->OMSetRenderTargets(1, &taaRtv, true, nullptr);

    mCommandList->SetGraphicsRootSignature(mTAARootSignature.Get());
    mCommandList->SetPipelineState(mPSOs["taa"].Get());
    mCommandList->SetGraphicsRootConstantBufferView(0, mTAACB->Resource()->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE currentFrameHandle(
        mSrvUavHeap->GetGPUDescriptorHandleForHeapStart(),
        mSceneColorSrvIndex,
        mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1, currentFrameHandle);

    CD3DX12_GPU_DESCRIPTOR_HANDLE historyHandle(
        mSrvUavHeap->GetGPUDescriptorHandleForHeapStart(),
        mTAAHistorySrvIndex,
        mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(2, historyHandle);

    mCommandList->IASetVertexBuffers(0, 0, nullptr);
    mCommandList->IASetIndexBuffer(nullptr);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCommandList->DrawInstanced(3, 1, 0, 0);

    TransitionResource(mTemporalAA->Resource(), mTAAOutputState, D3D12_RESOURCE_STATE_GENERIC_READ);

    TransitionResource(mTemporalAA->HistoryResource(), mTAAHistoryState, D3D12_RESOURCE_STATE_COPY_DEST);
    TransitionResource(mTemporalAA->Resource(), mTAAOutputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    mCommandList->CopyResource(mTemporalAA->HistoryResource(), mTemporalAA->Resource());
    TransitionResource(mTemporalAA->HistoryResource(), mTAAHistoryState, D3D12_RESOURCE_STATE_GENERIC_READ);
    TransitionResource(mTemporalAA->Resource(), mTAAOutputState, D3D12_RESOURCE_STATE_GENERIC_READ);

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);
}

void TerrainApp::ApplyDigging()
{
    if (!mCameraModeActive)
    {
        return;
    }

    const XMFLOAT3 cameraPos = mCamera.GetPosition();
    XMFLOAT3 forward = mCamera.GetForward();

    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&forward));
    XMStoreFloat3(&forward, dir);

    XMFLOAT3 hitPos = {};
    float hitDistance = 0.0f;

    const float rayStep = (std::max)(0.25f, mVoxelWorld.Settings().VoxelSize * 0.35f);
    const bool hit = mVoxelWorld.RaycastSurface(
        cameraPos,
        forward,
        mDigMaxDistance,
        rayStep,
        hitPos,
        hitDistance);

    if (!hit)
    {
        return;
    }

    XMFLOAT3 digCenter(
        hitPos.x + forward.x * (mVoxelWorld.Settings().VoxelSize * 0.7f),
        hitPos.y + forward.y * (mVoxelWorld.Settings().VoxelSize * 0.7f),
        hitPos.z + forward.z * (mVoxelWorld.Settings().VoxelSize * 0.7f));

    mVoxelWorld.DigSphere(digCenter, mDigRadius, mDigStrength);
}

void TerrainApp::SetCameraMode(bool enabled)
{
    if (enabled == mCameraModeActive)
    {
        return;
    }

    mCameraModeActive = enabled;

    if (enabled)
    {
        SetCapture(mhMainWnd);

        if (!mCursorHidden)
        {
            while (::ShowCursor(FALSE) >= 0)
            {
            }
            mCursorHidden = true;
        }

        POINT centerClient = { mClientWidth / 2, mClientHeight / 2 };
        POINT centerScreen = centerClient;
        ClientToScreen(mhMainWnd, &centerScreen);
        SetCursorPos(centerScreen.x, centerScreen.y);
        mIgnoreNextMouseDelta = true;
    }
    else
    {
        ReleaseCapture();

        if (mCursorHidden)
        {
            while (::ShowCursor(TRUE) < 0)
            {
            }
            mCursorHidden = false;
        }
    }
}

void TerrainApp::UpdateCameraLookFromCursor()
{
    if (!mCameraModeActive)
    {
        return;
    }

    POINT centerClient = { mClientWidth / 2, mClientHeight / 2 };
    POINT centerScreen = centerClient;
    ClientToScreen(mhMainWnd, &centerScreen);

    POINT cursorPos = {};
    GetCursorPos(&cursorPos);

    int deltaX = cursorPos.x - centerScreen.x;
    int deltaY = cursorPos.y - centerScreen.y;

    if (mIgnoreNextMouseDelta)
    {
        deltaX = 0;
        deltaY = 0;
        mIgnoreNextMouseDelta = false;
    }

    if (deltaX != 0 || deltaY != 0)
    {
        mCamera.RotateYaw(static_cast<float>(deltaX) * mMouseSensitivity);
        mCamera.RotatePitch(static_cast<float>(deltaY) * mMouseSensitivity);
    }

    SetCursorPos(centerScreen.x, centerScreen.y);
}

void TerrainApp::TransitionResource(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES& currentState,
    D3D12_RESOURCE_STATES targetState)
{
    if (currentState == targetState)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        resource,
        currentState,
        targetState);
    mCommandList->ResourceBarrier(1, &barrier);

    currentState = targetState;
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 2> TerrainApp::GetStaticSamplers()
{
    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        1,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f,
        8);

    return { linearWrap, anisotropicWrap };
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 2> TerrainApp::GetTAAStaticSamplers()
{
    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    return { pointClamp, linearClamp };
}
