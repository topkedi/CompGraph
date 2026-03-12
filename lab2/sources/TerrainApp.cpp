#include "TerrainApp.h"
#include "DDSTextureLoader.h"
#include <sstream>
#include <ctime>
#include <cstdlib>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

const int gNumFrameResources = 1;

TerrainApp::TerrainApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    mMainWndCaption = L"Terrain Renderer";
    mNoiseSeed = 1000.0f;
}

TerrainApp::~TerrainApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
}

bool TerrainApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCamera.SetPosition(475.0f, 600.0f, 75.0f);
    mCamera.SetProjectionValues(50.0f, AspectRatio(), 1.0f, 10000.0f);

    // Инициализируем предыдущую матрицу текущей
    XMMATRIX view = mCamera.GetViewMatrix();
    XMMATRIX proj = mCamera.GetProjectionMatrix();
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMStoreFloat4x4(&mPrevViewProj, viewProj);

    mTemporalAA = std::make_unique<TemporalAA>(md3dDevice.Get(), mClientWidth, mClientHeight, mBackBufferFormat);

    LoadTerrainTextures();
    CreateRootSignature();
    CreateTAARootSignature();
    CreateMotionVectorRootSignature();
    CompileShaders();
    GenerateTerrainMesh();
    SetupDescriptorHeaps();
    CreatePipelineState();
    CreateTAAPipelineState();
    CreateMotionVectorPipelineState();
    CreateVelocityDebugPipelineState();
    CreateObjectPipelineState();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    FlushCommandQueue();

    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(md3dDevice.Get(), 1, true);
    mTAACB = std::make_unique<UploadBuffer<TAAConstants>>(md3dDevice.Get(), 1, true);
    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(md3dDevice.Get(), 1, true);

    std::vector<float> lodDistances = { 300.0f, 900.0f, 1500.0f };
    float terrainSize = (float)(TilesX * TileSize);
    int maxDepth = 4;
    mQuadTree.Initialize(terrainSize, maxDepth, lodDistances);
    
    OutputDebugStringA(("QuadTree initialized: size=" + std::to_string(terrainSize) + 
                        ", maxDepth=" + std::to_string(maxDepth) + "\n").c_str());

    OutputDebugStringA("Controls: T - Toggle TAA, M - Toggle Motion Vector Visualization, V - Toggle Velocity Debug\n");
    OutputDebugStringA("Movement: WASD + QE, Hold SHIFT for 3x speed\n");
    OutputDebugStringA("Camera motion will generate velocity for TAA\n");

    return true;
}

void TerrainApp::OnResize()
{
    D3DApp::OnResize();
    mCamera.SetProjectionValues(50.0f, AspectRatio(), 1.0f, 10000.0f);
    
    if (mTemporalAA != nullptr)
    {
        mTemporalAA->OnResize(mClientWidth, mClientHeight);
    }

    // Пересоздаем буферы при изменении размера
    if (mTemporalAA != nullptr && !mTextures.empty())
    {
        // Освобождаем старые буферы
        mSceneColorBuffer.Reset();
        mMotionVectorBuffer.Reset();
        
        SetupDescriptorHeaps();
    }
}

void TerrainApp::Update(const GameTimer& gt)
{
    UpdateConstants(gt);
    UpdateTAAConstants(gt);
    CullTiles();
}

void TerrainApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSOs["terrain"].Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    if (mTAAEnabled)
    {
        // Render scene to texture
        DrawSceneToTexture();
        
        if (mShowMotionVectors)
        {
            // Показать motion vectors вместо TAA результата
            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

            float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
            mCommandList->ClearRenderTargetView(CurrentBackBufferView(), clearColor, 0, nullptr);
            mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

            ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
            mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

            mCommandList->SetGraphicsRootSignature(mMotionVectorRootSignature.Get());
            mCommandList->SetPipelineState(mPSOs["motionVisualize"].Get());

            CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
            srvHandle.Offset(mMotionVectorSrvIndex, mCbvSrvUavDescriptorSize);
            mCommandList->SetGraphicsRootDescriptorTable(0, srvHandle);

            mCommandList->IASetVertexBuffers(0, 0, nullptr);
            mCommandList->IASetIndexBuffer(nullptr);
            mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            mCommandList->DrawInstanced(3, 1, 0, 0);

            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        }
        else if (mShowVelocityDebug)
        {
            // Показать velocity debug (красный оверлей на движущихся пикселях)
            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

            mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

            ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
            mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

            mCommandList->SetGraphicsRootSignature(mMotionVectorRootSignature.Get());
            mCommandList->SetPipelineState(mPSOs["velocityDebug"].Get());

            // t0 - Motion vector buffer
            CD3DX12_GPU_DESCRIPTOR_HANDLE motionHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
            motionHandle.Offset(mMotionVectorSrvIndex, mCbvSrvUavDescriptorSize);
            mCommandList->SetGraphicsRootDescriptorTable(0, motionHandle);

            // t1 - Scene color buffer
            CD3DX12_GPU_DESCRIPTOR_HANDLE sceneHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
            sceneHandle.Offset(mSceneColorSrvIndex, mCbvSrvUavDescriptorSize);
            mCommandList->SetGraphicsRootDescriptorTable(1, sceneHandle);

            mCommandList->IASetVertexBuffers(0, 0, nullptr);
            mCommandList->IASetIndexBuffer(nullptr);
            mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            mCommandList->DrawInstanced(3, 1, 0, 0);

            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        }
        else
        {
            // Apply TAA
            ResolveTAA();

            // Copy TAA output to backbuffer
            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->Resource(),
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE));

            mCommandList->CopyResource(CurrentBackBuffer(), mTemporalAA->Resource());

            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->Resource(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));
            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
        }

        mFrameIndex++;
    }
    else
    {
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        float clearColor[] = { 0.85f, 0.92f, 0.98f, 1.0f };
        mCommandList->ClearRenderTargetView(CurrentBackBufferView(), clearColor, 0, nullptr);
        mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

        ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
        mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
        mCommandList->SetGraphicsRootConstantBufferView(0, mPassCB->Resource()->GetGPUVirtualAddress());

        CD3DX12_GPU_DESCRIPTOR_HANDLE heightmapHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        heightmapHandle.Offset(mHeightmapSrvIndex, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(2, heightmapHandle);

        // Рендеринг ландшафта
        mCommandList->SetPipelineState(mPSOs["terrainSimple"].Get());
        mCommandList->IASetVertexBuffers(0, 1, &mTerrainVBV);
        mCommandList->IASetIndexBuffer(&mTerrainIBV);
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);

        // Пустой ObjectConstants для совместимости
        ObjectConstants emptyObjConstants;
        emptyObjConstants.World = MathHelper::Identity4x4();
        emptyObjConstants.PrevWorld = MathHelper::Identity4x4();
        mObjectCB->CopyData(0, emptyObjConstants);
        mCommandList->SetGraphicsRootConstantBufferView(1, mObjectCB->Resource()->GetGPUVirtualAddress());

        for (auto* tile : mVisibleTiles)
        {
            int texIndex = tile->ColorTextureIndex;
            if (texIndex >= 0 && texIndex < (int)mTextures.size())
            {
                CD3DX12_GPU_DESCRIPTOR_HANDLE colorHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
                colorHandle.Offset(texIndex, mCbvSrvUavDescriptorSize);
                mCommandList->SetGraphicsRootDescriptorTable(3, colorHandle);
            }

            mCommandList->DrawIndexedInstanced(tile->IndexCount, 1, tile->IndexOffset, tile->VertexOffset, 0);
        }
        
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
    }

    ThrowIfFailed(mCommandList->Close());

    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    FlushCommandQueue();
}

void TerrainApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;
    SetCapture(mhMainWnd);
}

void TerrainApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void TerrainApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if ((btnState & MK_LBUTTON) != 0)
    {
        float deltaX = 0.15f * static_cast<float>(x - mLastMousePos.x);
        float deltaY = 0.15f * static_cast<float>(y - mLastMousePos.y);

        mCamera.RotateYaw(deltaX);
        mCamera.RotatePitch(deltaY);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void TerrainApp::OnKeyPressed(const GameTimer& gt, WPARAM key)
{
    // Проверяем, зажат ли Shift для ускоренного движения
    bool fastMode = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    
    switch (key)
    {
    case 'W':
        mCamera.MoveForward(fastMode);
        break;
    case 'S':
        mCamera.MoveBackward(fastMode);
        break;
    case 'A':
        mCamera.MoveLeft(fastMode);
        break;
    case 'D':
        mCamera.MoveRight(fastMode);
        break;
    case 'Q':
        mCamera.MoveUp(fastMode);
        break;
    case 'E':
        mCamera.MoveDown(fastMode);
        break;
    case 'R':
        mCamera.TurnUp();
        break;
    case 'F':
        mCamera.TurnDown();
        break;
    case 'C':
        break;
    case 'T':
        mTAAEnabled = !mTAAEnabled;
        if (mTAAEnabled)
        {
            mFrameIndex = 0;  // Reset frame counter
            OutputDebugStringA("TAA Enabled\n");
        }
        else
        {
            OutputDebugStringA("TAA Disabled\n");
        }
        break;
    case 'M':
        // Переключение режима отображения motion vectors (для отладки)
        mShowMotionVectors = !mShowMotionVectors;
        if (mShowMotionVectors)
        {
            mShowVelocityDebug = false;  // Выключаем velocity debug
            OutputDebugStringA("Motion Vector visualization ON\n");
        }
        else
        {
            OutputDebugStringA("Motion Vector visualization OFF\n");
        }
        break;
    case 'V':
        // Переключение velocity debug (красный оверлей на движущихся пикселях)
        mShowVelocityDebug = !mShowVelocityDebug;
        if (mShowVelocityDebug)
        {
            mShowMotionVectors = false;  // Выключаем motion vector visualization
            OutputDebugStringA("Velocity Debug ON (red overlay on moving pixels)\n");
        }
        else
        {
            OutputDebugStringA("Velocity Debug OFF\n");
        }
        break;
    }
}

void TerrainApp::CreateRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE heightmapRange;
    heightmapRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE colorRange;
    colorRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

    CD3DX12_ROOT_PARAMETER rootParams[4];
    rootParams[0].InitAsConstantBufferView(0);  // PassConstants
    rootParams[1].InitAsConstantBufferView(1);  // ObjectConstants (для куба)
    rootParams[2].InitAsDescriptorTable(1, &heightmapRange, D3D12_SHADER_VISIBILITY_ALL);
    rootParams[3].InitAsDescriptorTable(1, &colorRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, rootParams, 1, &samplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void TerrainApp::CompileShaders()
{
    OutputDebugStringW(L"Compiling shaders...\n");
    
    mShaders["terrainVS"] = d3dUtil::CompileShader(L"shaders/vs.hlsl", nullptr, "VS", "vs_5_0");
    OutputDebugStringW(L"VS compiled\n");
    
    mShaders["terrainHS"] = d3dUtil::CompileShader(L"shaders/hs.hlsl", nullptr, "HS", "hs_5_0");
    OutputDebugStringW(L"HS compiled\n");
    
    mShaders["terrainDS"] = d3dUtil::CompileShader(L"shaders/ds.hlsl", nullptr, "DS", "ds_5_0");
    OutputDebugStringW(L"DS compiled\n");
    
    mShaders["terrainPS"] = d3dUtil::CompileShader(L"shaders/ps.hlsl", nullptr, "PS", "ps_5_0");
    OutputDebugStringW(L"PS compiled\n");

    // TAA шейдеры
    mShaders["taaVS"] = d3dUtil::CompileShader(L"shaders/TAAResolve.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["taaPS"] = d3dUtil::CompileShader(L"shaders/TAAResolve.hlsl", nullptr, "PS", "ps_5_0");
    
    // Motion Vector визуализация шейдеры
    mShaders["motionVS"] = d3dUtil::CompileShader(L"shaders/MotionVectorVisualize.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["motionPS"] = d3dUtil::CompileShader(L"shaders/MotionVectorVisualize.hlsl", nullptr, "PS", "ps_5_0");
    
    // Velocity Debug шейдеры (красный оверлей)
    mShaders["velocityDebugVS"] = d3dUtil::CompileShader(L"shaders/VelocityDebug.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["velocityDebugPS"] = d3dUtil::CompileShader(L"shaders/VelocityDebug.hlsl", nullptr, "PS", "ps_5_0");
    
    // Объектные шейдеры
    mShaders["objectVS"] = d3dUtil::CompileShader(L"shaders/object_vs.hlsl", nullptr, "VS", "vs_5_0");
    // Используем тот же пиксельный шейдер что и для ландшафта

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    
    OutputDebugStringW(L"Shaders and input layout built\n");
}

void TerrainApp::CreatePipelineState()
{
    // PSO для ландшафта (с ObjectConstants для совместимости)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = { mShaders["terrainVS"]->GetBufferPointer(), mShaders["terrainVS"]->GetBufferSize() };
    psoDesc.HS = { mShaders["terrainHS"]->GetBufferPointer(), mShaders["terrainHS"]->GetBufferSize() };
    psoDesc.DS = { mShaders["terrainDS"]->GetBufferPointer(), mShaders["terrainDS"]->GetBufferSize() };
    psoDesc.PS = { mShaders["terrainPS"]->GetBufferPointer(), mShaders["terrainPS"]->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    psoDesc.NumRenderTargets = 2;  // Color + Motion Vectors для TAA режима
    psoDesc.RTVFormats[0] = mBackBufferFormat;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;  // Motion vectors
    psoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    psoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    psoDesc.DSVFormat = mDepthStencilFormat;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSOs["terrain"])));
    
    // PSO для обычного рендеринга (без motion vectors)
    psoDesc.NumRenderTargets = 1;  // Только Color
    psoDesc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;  // Очищаем второй render target
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSOs["terrainSimple"])));
}

void TerrainApp::GenerateTerrainMesh()
{
    std::vector<TerrainVertex> allVertices;
    std::vector<uint16_t> allIndices;

    const float patchSize = (float)TileSize / PatchesPerTile;
    const float terrainWidth = (float)(TilesX * TileSize);
    const float terrainDepth = (float)(TilesY * TileSize);

    for (int tileY = 0; tileY < TilesY; tileY++)
    {
        for (int tileX = 0; tileX < TilesX; tileX++)
        {
            TerrainTileInfo tile;
            tile.TileX = tileX;
            tile.TileY = tileY;
            tile.TileSize = TileSize;
            tile.VertexOffset = (UINT)allVertices.size();
            tile.IndexOffset = (UINT)allIndices.size();

            float tileStartX = (float)(tileX * TileSize);
            float tileStartZ = (float)(tileY * TileSize);

            for (int z = 0; z <= PatchesPerTile; z++)
            {
                for (int x = 0; x <= PatchesPerTile; x++)
                {
                    TerrainVertex vertex;

                    float posX = tileStartX + x * patchSize;
                    float posZ = tileStartZ + z * patchSize;

                    vertex.Pos = XMFLOAT3(posX, 0.0f, posZ);

                    float globalU = posX / terrainWidth;
                    float globalV = 1.0f - (posZ / terrainDepth);

                    float localU = (float)x / PatchesPerTile;
                    float localV = 1.0f - ((float)z / PatchesPerTile);

                    vertex.TexC = XMFLOAT2(globalU, globalV);
                    vertex.LocalTexC = XMFLOAT2(localU, localV);

                    allVertices.push_back(vertex);
                }
            }

            UINT baseVertex = tile.VertexOffset;
            for (int z = 0; z < PatchesPerTile; z++)
            {
                for (int x = 0; x < PatchesPerTile; x++)
                {
                    int topLeft = z * (PatchesPerTile + 1) + x;
                    int topRight = topLeft + 1;
                    int bottomLeft = (z + 1) * (PatchesPerTile + 1) + x;
                    int bottomRight = bottomLeft + 1;

                    allIndices.push_back((uint16_t)(topLeft));
                    allIndices.push_back((uint16_t)(topRight));
                    allIndices.push_back((uint16_t)(bottomLeft));
                    allIndices.push_back((uint16_t)(bottomRight));
                }
            }

            tile.IndexCount = PatchesPerTile * PatchesPerTile * 4;

            float minHeight = 0.0f;
            float maxHeight = 800.0f;

            XMFLOAT3 boundsMin(tileStartX, minHeight, tileStartZ);
            XMFLOAT3 boundsMax(tileStartX + TileSize, maxHeight, tileStartZ + TileSize);

            tile.Bounds = BoundingBox(
                XMFLOAT3((boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f, (boundsMin.z + boundsMax.z) * 0.5f),
                XMFLOAT3((boundsMax.x - boundsMin.x) * 0.5f, (boundsMax.y - boundsMin.y) * 0.5f, (boundsMax.z - boundsMin.z) * 0.5f)
            );

            tile.ColorTextureIndex = 1 + tileY * TilesX + tileX;
            tile.NormalTextureIndex = -1;

            OutputDebugStringA(("Tile (" + std::to_string(tileX) + "," + std::to_string(tileY) + ") -> texture index " + std::to_string(tile.ColorTextureIndex) + "\n").c_str());

            mTiles.push_back(tile);
        }
    }

    const UINT vbByteSize = (UINT)allVertices.size() * sizeof(TerrainVertex);
    const UINT ibByteSize = (UINT)allIndices.size() * sizeof(uint16_t);

    mTerrainVertexBuffer = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), allVertices.data(), vbByteSize, mTerrainVertexUploadBuffer);

    mTerrainIndexBuffer = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), allIndices.data(), ibByteSize, mTerrainIndexUploadBuffer);

    mTerrainVBV.BufferLocation = mTerrainVertexBuffer->GetGPUVirtualAddress();
    mTerrainVBV.StrideInBytes = sizeof(TerrainVertex);
    mTerrainVBV.SizeInBytes = vbByteSize;

    mTerrainIBV.BufferLocation = mTerrainIndexBuffer->GetGPUVirtualAddress();
    mTerrainIBV.Format = DXGI_FORMAT_R16_UINT;
    mTerrainIBV.SizeInBytes = ibByteSize;

    OutputDebugStringA(("Total vertices: " + std::to_string(allVertices.size()) + "\n").c_str());
    OutputDebugStringA(("Total indices: " + std::to_string(allIndices.size()) + "\n").c_str());
    OutputDebugStringA(("Total tiles: " + std::to_string(mTiles.size()) + "\n").c_str());
}

void TerrainApp::SetupDescriptorHeaps()
{
    UINT numTextures = 1 + TilesX * TilesY;
    UINT numTAADescriptors = 6;  // SceneColor, MotionVector, TAAOutput, TAAHistory (SRV + RTV)
    
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = numTextures + numTAADescriptors;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 6;  // SceneColor, MotionVector, TAAOutput, TAAHistory
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    for (size_t i = 0; i < mTextures.size(); i++)
    {
        auto& tex = mTextures[i];

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = tex->Resource->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, hDescriptor);
        hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);
    }

    // Создание буфера для scene color
    D3D12_RESOURCE_DESC texDesc;
    ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mClientWidth;
    texDesc.Height = mClientHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = mBackBufferFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    float clearColor[] = { 0.85f, 0.92f, 0.98f, 1.0f };
    CD3DX12_CLEAR_VALUE optClear(mBackBufferFormat, clearColor);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &optClear,
        IID_PPV_ARGS(&mSceneColorBuffer)));

    // Именуем ресурс для отладки в RenderDoc
    mSceneColorBuffer->SetName(L"Scene Color Buffer");

    // Создание буфера для motion vectors (RG16F формат для X,Y компонент)
    texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    float motionClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CD3DX12_CLEAR_VALUE motionClear(DXGI_FORMAT_R16G16_FLOAT, motionClearColor);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &motionClear,
        IID_PPV_ARGS(&mMotionVectorBuffer)));

    // Именуем ресурс для отладки в RenderDoc
    mMotionVectorBuffer->SetName(L"Velocity Texture");
    
    OutputDebugStringA("Velocity Texture created: DXGI_FORMAT_R16G16_FLOAT\n");

    mSceneColorSrvIndex = numTextures;
    mMotionVectorSrvIndex = numTextures + 1;
    mTAAOutputSrvIndex = numTextures + 2;
    mTAAHistorySrvIndex = numTextures + 3;

    mSceneColorRtvIndex = 0;
    mMotionVectorRtvIndex = 1;
    mTAAOutputRtvIndex = 2;
    mTAAHistoryRtvIndex = 3;

    // SRV для SceneColor
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    srvCpuHandle.Offset(mSceneColorSrvIndex, mCbvSrvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    srvGpuHandle.Offset(mSceneColorSrvIndex, mCbvSrvUavDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = mBackBufferFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    md3dDevice->CreateShaderResourceView(mSceneColorBuffer.Get(), &srvDesc, srvCpuHandle);

    // RTV для SceneColor
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvCpuHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvCpuHandle.Offset(mSceneColorRtvIndex, mRtvDescriptorSize);
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = mBackBufferFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    md3dDevice->CreateRenderTargetView(mSceneColorBuffer.Get(), &rtvDesc, rtvCpuHandle);

    // SRV для MotionVector
    srvCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    srvCpuHandle.Offset(mMotionVectorSrvIndex, mCbvSrvUavDescriptorSize);
    srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    md3dDevice->CreateShaderResourceView(mMotionVectorBuffer.Get(), &srvDesc, srvCpuHandle);

    // RTV для MotionVector
    rtvCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvCpuHandle.Offset(mMotionVectorRtvIndex, mRtvDescriptorSize);
    rtvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    md3dDevice->CreateRenderTargetView(mMotionVectorBuffer.Get(), &rtvDesc, rtvCpuHandle);

    // TAA дескрипторы
    srvCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    srvCpuHandle.Offset(mTAAOutputSrvIndex, mCbvSrvUavDescriptorSize);
    srvGpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    srvGpuHandle.Offset(mTAAOutputSrvIndex, mCbvSrvUavDescriptorSize);
    rtvCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvCpuHandle.Offset(mTAAOutputRtvIndex, mRtvDescriptorSize);

    mTemporalAA->BuildDescriptors(srvCpuHandle, srvGpuHandle, rtvCpuHandle, 
                                   mCbvSrvUavDescriptorSize, mRtvDescriptorSize);
}

void TerrainApp::LoadTerrainTextures()
{
    auto heightmapTex = std::make_unique<Texture>();
    heightmapTex->Name = "heightmap";
    heightmapTex->Filename = L"Terrain/003/Height_Out.dds";

    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
        mCommandList.Get(), heightmapTex->Filename.c_str(),
        heightmapTex->Resource, heightmapTex->UploadHeap));

    mHeightmapSrvIndex = 0;
    mTextures.push_back(std::move(heightmapTex));

    OutputDebugStringW(L"Loaded global heightmap from Terrain/003/Height_Out.dds\n");

    for (int y = 0; y < TilesY; y++)
    {
        for (int x = 0; x < TilesX; x++)
        {
            int fileY = (TilesY - 1) - y;

            std::wstringstream texSS;
            texSS << L"Terrain/001/Weathering/Weathering_Out_y" << fileY << L"_x" << x << L".dds";
            std::wstring texturePath = texSS.str();

            auto colorTex = std::make_unique<Texture>();
            colorTex->Name = "color_" + std::to_string(y) + "_" + std::to_string(x);
            colorTex->Filename = texturePath;

            HRESULT hr = DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
                mCommandList.Get(), colorTex->Filename.c_str(),
                colorTex->Resource, colorTex->UploadHeap);

            if (SUCCEEDED(hr))
            {
                mTextures.push_back(std::move(colorTex));
                OutputDebugStringW((L"Loaded tile texture: " + texturePath + L"\n").c_str());
            }
            else
            {
                OutputDebugStringW((L"Failed to load tile texture: " + texturePath + L"\n").c_str());
            }
        }
    }

    OutputDebugStringA(("Total textures loaded: " + std::to_string(mTextures.size()) + "\n").c_str());
}

void TerrainApp::UpdateConstants(const GameTimer& gt)
{
    XMMATRIX view = mCamera.GetViewMatrix();
    XMMATRIX proj = mCamera.GetProjectionMatrix();
    
    // Сохраняем ViewProj БЕЗ jitter для следующего кадра (как предыдущую матрицу)
    XMMATRIX viewProjNoJitter = XMMatrixMultiply(view, proj);
    
    // Apply jitter for TAA - modify projection matrix BEFORE multiplying with view
    if (mTAAEnabled)
    {
        XMFLOAT2 jitter = TemporalAA::GetJitter(mFrameIndex);
        
        // Jitter is in [-0.5, 0.5] range, convert to NDC space
        float jitterX = (jitter.x * 2.0f) / (float)mClientWidth;
        float jitterY = (jitter.y * 2.0f) / (float)mClientHeight;
        
        // Debug output
        static int debugCounter = 0;
        if (debugCounter++ % 60 == 0)
        {
            OutputDebugStringA(("TAA Frame: " + std::to_string(mFrameIndex) + 
                               ", Jitter: (" + std::to_string(jitter.x) + ", " + std::to_string(jitter.y) + ")" +
                               ", NDC: (" + std::to_string(jitterX) + ", " + std::to_string(jitterY) + ")\n").c_str());
        }
        
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
    XMStoreFloat4x4(&passConstants.ViewProjNoJitter, viewProjNoJitter);  // Для motion vectors
    passConstants.PrevViewProj = mPrevViewProj;  // Предыдущая матрица для motion vectors
    passConstants.EyePosW = mCamera.GetPosition();
    passConstants.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    passConstants.NearZ = 1.0f;
    passConstants.FarZ = 10000.0f;
    passConstants.TotalTime = gt.TotalTime();
    passConstants.DeltaTime = gt.DeltaTime();
    passConstants.HeightScale = 800.0f;
    passConstants.NoiseSeed = mNoiseSeed;

    mPassCB->CopyData(0, passConstants);
    
    // Сохраняем ViewProj БЕЗ jitter для следующего кадра
    XMStoreFloat4x4(&mPrevViewProj, viewProjNoJitter);
}

void TerrainApp::CullTiles()
{
    mVisibleTiles.clear();

    XMFLOAT3 cameraPos = mCamera.GetPosition();
    BoundingFrustum frustum = BuildFrustum();
    
    mQuadTree.Update(cameraPos, frustum);
    
    const auto& visibleNodes = mQuadTree.GetVisibleNodes();
    
    for (const auto& renderNode : visibleNodes)
    {
        TreeNode* node = renderNode.Node;
        
        float nodeMinX = node->Position.x - node->NodeSize * 0.5f;
        float nodeMinZ = node->Position.z - node->NodeSize * 0.5f;
        float nodeMaxX = node->Position.x + node->NodeSize * 0.5f;
        float nodeMaxZ = node->Position.z + node->NodeSize * 0.5f;
        
        for (auto& tile : mTiles)
        {
            float tileMinX = (float)(tile.TileX * tile.TileSize);
            float tileMinZ = (float)(tile.TileY * tile.TileSize);
            float tileMaxX = tileMinX + tile.TileSize;
            float tileMaxZ = tileMinZ + tile.TileSize;
            
            bool overlaps = !(nodeMaxX < tileMinX || nodeMinX > tileMaxX ||
                             nodeMaxZ < tileMinZ || nodeMinZ > tileMaxZ);
            
            if (overlaps)
            {
                bool alreadyAdded = false;
                for (auto* t : mVisibleTiles)
                {
                    if (t == &tile)
                    {
                        alreadyAdded = true;
                        break;
                    }
                }
                if (!alreadyAdded)
                {
                    mVisibleTiles.push_back(&tile);
                }
            }
        }
    }

    static int frameCount = 0;
    if (frameCount++ % 120 == 0)
    {
        OutputDebugStringA(("QuadTree nodes: " + std::to_string(visibleNodes.size()) + 
                           ", Visible tiles: " + std::to_string(mVisibleTiles.size()) + "\n").c_str());
        
        int lodCounts[4] = {0, 0, 0, 0};
        for (const auto& rn : visibleNodes)
        {
            lodCounts[static_cast<int>(rn.Detail)]++;
        }
        OutputDebugStringA(("LOD distribution: LOD0=" + std::to_string(lodCounts[0]) +
                           " LOD1=" + std::to_string(lodCounts[1]) +
                           " LOD2=" + std::to_string(lodCounts[2]) +
                           " LOD3=" + std::to_string(lodCounts[3]) + "\n").c_str());
    }
}

BoundingFrustum TerrainApp::BuildFrustum() const
{
    XMMATRIX proj = XMMatrixTranspose(mCamera.GetProjectionMatrix());
    BoundingFrustum frustum(proj);
    XMMATRIX view = XMMatrixTranspose(mCamera.GetViewMatrix());
    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    frustum.Transform(frustum, invView);
    return frustum;
}

void TerrainApp::CreateTAARootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);  // Current frame
    
    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);  // History frame
    
    CD3DX12_DESCRIPTOR_RANGE texTable2;
    texTable2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);  // Motion vectors

    CD3DX12_ROOT_PARAMETER slotRootParameter[4];
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[2].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[3].InitAsDescriptorTable(1, &texTable2, D3D12_SHADER_VISIBILITY_PIXEL);

    auto staticSamplers = GetStaticSamplers();

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
        (UINT)staticSamplers.size(), staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mTAARootSignature)));
}

void TerrainApp::CreateTAAPipelineState()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC taaPsoDesc = {};
    taaPsoDesc.InputLayout = { nullptr, 0 };
    taaPsoDesc.pRootSignature = mTAARootSignature.Get();
    taaPsoDesc.VS = { mShaders["taaVS"]->GetBufferPointer(), mShaders["taaVS"]->GetBufferSize() };
    taaPsoDesc.PS = { mShaders["taaPS"]->GetBufferPointer(), mShaders["taaPS"]->GetBufferSize() };
    taaPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    taaPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    taaPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    taaPsoDesc.DepthStencilState.DepthEnable = false;
    taaPsoDesc.SampleMask = UINT_MAX;
    taaPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    taaPsoDesc.NumRenderTargets = 1;
    taaPsoDesc.RTVFormats[0] = mBackBufferFormat;
    taaPsoDesc.SampleDesc.Count = 1;
    taaPsoDesc.SampleDesc.Quality = 0;
    taaPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&taaPsoDesc, IID_PPV_ARGS(&mPSOs["taa"])));
}

void TerrainApp::CreateMotionVectorRootSignature()
{
    // Две текстуры: t0 - motion vectors, t1 - scene color (для velocity debug)
    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);  // Motion vectors (t0)
    
    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);  // Scene color (t1)

    CD3DX12_ROOT_PARAMETER slotRootParameter[2];
    slotRootParameter[0].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[1].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);

    auto staticSamplers = GetStaticSamplers();

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, slotRootParameter,
        (UINT)staticSamplers.size(), staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mMotionVectorRootSignature)));
}

void TerrainApp::CreateMotionVectorPipelineState()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC motionPsoDesc = {};
    motionPsoDesc.InputLayout = { nullptr, 0 };
    motionPsoDesc.pRootSignature = mMotionVectorRootSignature.Get();
    motionPsoDesc.VS = { mShaders["motionVS"]->GetBufferPointer(), mShaders["motionVS"]->GetBufferSize() };
    motionPsoDesc.PS = { mShaders["motionPS"]->GetBufferPointer(), mShaders["motionPS"]->GetBufferSize() };
    motionPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    motionPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    motionPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    motionPsoDesc.DepthStencilState.DepthEnable = false;
    motionPsoDesc.SampleMask = UINT_MAX;
    motionPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    motionPsoDesc.NumRenderTargets = 1;
    motionPsoDesc.RTVFormats[0] = mBackBufferFormat;
    motionPsoDesc.SampleDesc.Count = 1;
    motionPsoDesc.SampleDesc.Quality = 0;
    motionPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&motionPsoDesc, IID_PPV_ARGS(&mPSOs["motionVisualize"])));
}

void TerrainApp::CreateVelocityDebugPipelineState()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC velocityDebugDesc = {};
    velocityDebugDesc.InputLayout = { nullptr, 0 };  // Fullscreen triangle без vertex buffer
    velocityDebugDesc.pRootSignature = mMotionVectorRootSignature.Get();  // Используем тот же root signature
    velocityDebugDesc.VS = { mShaders["velocityDebugVS"]->GetBufferPointer(), mShaders["velocityDebugVS"]->GetBufferSize() };
    velocityDebugDesc.PS = { mShaders["velocityDebugPS"]->GetBufferPointer(), mShaders["velocityDebugPS"]->GetBufferSize() };
    velocityDebugDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    velocityDebugDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    velocityDebugDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    velocityDebugDesc.DepthStencilState.DepthEnable = false;  // Без depth test
    velocityDebugDesc.SampleMask = UINT_MAX;
    velocityDebugDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    velocityDebugDesc.NumRenderTargets = 1;
    velocityDebugDesc.RTVFormats[0] = mBackBufferFormat;
    velocityDebugDesc.SampleDesc.Count = 1;
    velocityDebugDesc.SampleDesc.Quality = 0;
    velocityDebugDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&velocityDebugDesc, IID_PPV_ARGS(&mPSOs["velocityDebug"])));
}

void TerrainApp::CreateObjectPipelineState()
{
    // Input layout для объектов (локальные координаты)
    std::vector<D3D12_INPUT_ELEMENT_DESC> objectInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC objectPsoDesc = {};
    objectPsoDesc.InputLayout = { objectInputLayout.data(), (UINT)objectInputLayout.size() };
    objectPsoDesc.pRootSignature = mRootSignature.Get();
    objectPsoDesc.VS = { mShaders["objectVS"]->GetBufferPointer(), mShaders["objectVS"]->GetBufferSize() };
    objectPsoDesc.PS = { mShaders["terrainPS"]->GetBufferPointer(), mShaders["terrainPS"]->GetBufferSize() };
    objectPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    objectPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    objectPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    objectPsoDesc.SampleMask = UINT_MAX;
    objectPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    objectPsoDesc.NumRenderTargets = 2;  // Color + Motion Vectors
    objectPsoDesc.RTVFormats[0] = mBackBufferFormat;
    objectPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;  // Motion vectors
    objectPsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    objectPsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    objectPsoDesc.DSVFormat = mDepthStencilFormat;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&objectPsoDesc, IID_PPV_ARGS(&mPSOs["object"])));
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> TerrainApp::GetStaticSamplers()
{
    const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
        0,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        2,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        4,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f,
        8);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
        5,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f,
        8);

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp
    };
}

void TerrainApp::UpdateTAAConstants(const GameTimer& gt)
{
    TAAConstants taaConstants;
    taaConstants.JitterOffset = TemporalAA::GetJitter(mFrameIndex);
    taaConstants.ScreenSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    taaConstants.BlendFactor = mFrameIndex == 0 ? 1.0f : 0.1f;
    
    mTAACB->CopyData(0, taaConstants);
    
    // Отладочная информация
    static int debugFrameCounter = 0;
    if (mTAAEnabled && debugFrameCounter++ % 120 == 0)
    {
        OutputDebugStringA(("TAA Status: Frame=" + std::to_string(mFrameIndex) + 
                           ", BlendFactor=" + std::to_string(taaConstants.BlendFactor) +
                           ", ShowMotionVectors=" + (mShowMotionVectors ? "ON" : "OFF") + "\n").c_str());
    }
}

void TerrainApp::DrawSceneToTexture()
{
    // Переход буферов в состояние Render Target
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSceneColorBuffer.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mMotionVectorBuffer.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

    float clearColor[] = { 0.85f, 0.92f, 0.98f, 1.0f };
    float motionClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandles[2];
    rtvHandles[0] = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandles[0].Offset(mSceneColorRtvIndex, mRtvDescriptorSize);
    rtvHandles[1] = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandles[1].Offset(mMotionVectorRtvIndex, mRtvDescriptorSize);
    
    mCommandList->ClearRenderTargetView(rtvHandles[0], clearColor, 0, nullptr);
    mCommandList->ClearRenderTargetView(rtvHandles[1], motionClearColor, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Установка двух Render Targets: Color + Motion Vectors
    mCommandList->OMSetRenderTargets(2, rtvHandles, false, &DepthStencilView());

    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
    mCommandList->SetGraphicsRootConstantBufferView(0, mPassCB->Resource()->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE heightmapHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    heightmapHandle.Offset(mHeightmapSrvIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(2, heightmapHandle);

    // Рендеринг ландшафта
    mCommandList->SetPipelineState(mPSOs["terrain"].Get());
    mCommandList->IASetVertexBuffers(0, 1, &mTerrainVBV);
    mCommandList->IASetIndexBuffer(&mTerrainIBV);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);

    // Пустой ObjectConstants для ландшафта (ландшафт не двигается)
    ObjectConstants terrainObjConstants;
    terrainObjConstants.World = MathHelper::Identity4x4();
    terrainObjConstants.PrevWorld = MathHelper::Identity4x4();
    mObjectCB->CopyData(0, terrainObjConstants);
    mCommandList->SetGraphicsRootConstantBufferView(1, mObjectCB->Resource()->GetGPUVirtualAddress());

    for (auto* tile : mVisibleTiles)
    {
        int texIndex = tile->ColorTextureIndex;
        if (texIndex >= 0 && texIndex < (int)mTextures.size())
        {
            CD3DX12_GPU_DESCRIPTOR_HANDLE colorHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
            colorHandle.Offset(texIndex, mCbvSrvUavDescriptorSize);
            mCommandList->SetGraphicsRootDescriptorTable(3, colorHandle);
        }

        mCommandList->DrawIndexedInstanced(tile->IndexCount, 1, tile->IndexOffset, tile->VertexOffset, 0);
    }

    // Переход буферов обратно в Shader Resource
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSceneColorBuffer.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mMotionVectorBuffer.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void TerrainApp::ResolveTAA()
{
    // Clear history on first frame
    if (mFrameIndex == 0)
    {
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->HistoryResource(),
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
        
        float clearColor[] = { 0.85f, 0.92f, 0.98f, 1.0f };
        mCommandList->ClearRenderTargetView(mTemporalAA->HistoryRtv(), clearColor, 0, nullptr);
        
        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->HistoryResource(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
    }

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->Resource(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

    mCommandList->RSSetViewports(1, &mTemporalAA->Viewport());
    mCommandList->RSSetScissorRects(1, &mTemporalAA->ScissorRect());

    auto rtvHandle = mTemporalAA->Rtv();
    mCommandList->OMSetRenderTargets(1, &rtvHandle, true, nullptr);

    mCommandList->SetGraphicsRootSignature(mTAARootSignature.Get());
    mCommandList->SetPipelineState(mPSOs["taa"].Get());

    mCommandList->SetGraphicsRootConstantBufferView(0, mTAACB->Resource()->GetGPUVirtualAddress());

    // Current frame (SceneColor)
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    srvHandle.Offset(mSceneColorSrvIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);

    // History frame
    srvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    srvHandle.Offset(mTAAHistorySrvIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(2, srvHandle);

    // Motion vectors
    srvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    srvHandle.Offset(mMotionVectorSrvIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(3, srvHandle);

    mCommandList->IASetVertexBuffers(0, 0, nullptr);
    mCommandList->IASetIndexBuffer(nullptr);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCommandList->DrawInstanced(3, 1, 0, 0);

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->Resource(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->HistoryResource(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST));
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->Resource(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE));

    mCommandList->CopyResource(mTemporalAA->HistoryResource(), mTemporalAA->Resource());

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->HistoryResource(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTemporalAA->Resource(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));
}