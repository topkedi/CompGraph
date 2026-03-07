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

    // Initialize atmosphere with clean settings
    mAtmosphere.SetCleanAtmosphere();

    LoadTerrainTextures();
    CreateRootSignature();
    CreateSkyRootSignature();
    CreateFogRootSignature();
    CompileShaders();
    GenerateTerrainMesh();
    GenerateSkySphereMesh();
    SetupDescriptorHeaps();
    BuildSceneRenderTarget();
    CreatePipelineState();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    FlushCommandQueue();

    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(md3dDevice.Get(), 1, true);

    std::vector<float> lodDistances = { 300.0f, 900.0f, 1500.0f };
    float terrainSize = (float)(TilesX * TileSize);
    int maxDepth = 4;
    mQuadTree.Initialize(terrainSize, maxDepth, lodDistances);
    
    OutputDebugStringA(("QuadTree initialized: size=" + std::to_string(terrainSize) + 
                        ", maxDepth=" + std::to_string(maxDepth) + "\n").c_str());

    return true;
}

void TerrainApp::OnResize()
{
    D3DApp::OnResize();
    mCamera.SetProjectionValues(50.0f, AspectRatio(), 1.0f, 10000.0f);
    
    // Rebuild scene render target for fog
    if (md3dDevice != nullptr && mSrvDescriptorHeap != nullptr)
    {
        BuildSceneRenderTarget();
    }
}

void TerrainApp::Update(const GameTimer& gt)
{
    UpdateConstants(gt);
    UpdateAtmosphereConstants(gt);
    CullTiles();
}

void TerrainApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSOs["terrain"].Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    // Transition scene render target to render target state
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSceneRenderTarget.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear scene render target
    float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    mCommandList->ClearRenderTargetView(mSceneRtvHandle, clearColor, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Render scene to intermediate render target
    mCommandList->OMSetRenderTargets(1, &mSceneRtvHandle, true, &DepthStencilView());

    // Draw terrain
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
    mCommandList->SetGraphicsRootConstantBufferView(0, mPassCB->Resource()->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE heightmapHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    heightmapHandle.Offset(mHeightmapSrvIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1, heightmapHandle);

    mCommandList->IASetVertexBuffers(0, 1, &mTerrainVBV);
    mCommandList->IASetIndexBuffer(&mTerrainIBV);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);

    for (auto* tile : mVisibleTiles)
    {
        int texIndex = tile->ColorTextureIndex;
        if (texIndex >= 0 && texIndex < (int)mTextures.size())
        {
            CD3DX12_GPU_DESCRIPTOR_HANDLE colorHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
            colorHandle.Offset(texIndex, mCbvSrvUavDescriptorSize);
            mCommandList->SetGraphicsRootDescriptorTable(2, colorHandle);
        }

        mCommandList->DrawIndexedInstanced(tile->IndexCount, 1, tile->IndexOffset, tile->VertexOffset, 0);
    }

    // Draw sky with atmospheric scattering
    mCommandList->SetPipelineState(mPSOs["sky"].Get());
    mCommandList->SetGraphicsRootSignature(mSkyRootSignature.Get());
    mCommandList->SetGraphicsRootConstantBufferView(0, mPassCB->Resource()->GetGPUVirtualAddress());
    
    mCommandList->IASetVertexBuffers(0, 1, &mSkyVBV);
    mCommandList->IASetIndexBuffer(&mSkyIBV);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCommandList->DrawIndexedInstanced(mSkyIndexCount, 1, 0, 0, 0);

    // Transition scene render target to shader resource
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSceneRenderTarget.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    // Transition depth buffer to shader resource for fog
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    // Transition back buffer to render target
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Apply fog post-process to back buffer
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);
    mCommandList->SetGraphicsRootSignature(mFogRootSignature.Get());
    mCommandList->SetPipelineState(mPSOs["fog"].Get());

    // Bind fog resources
    mCommandList->SetGraphicsRootConstantBufferView(0, mPassCB->Resource()->GetGPUVirtualAddress());
    mCommandList->SetGraphicsRootDescriptorTable(1, mSceneSrvGpuHandle);
    mCommandList->SetGraphicsRootDescriptorTable(2, mDepthSrvGpuHandle);

    // Draw fullscreen triangle
    mCommandList->IASetVertexBuffers(0, 0, nullptr);
    mCommandList->IASetIndexBuffer(nullptr);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCommandList->DrawInstanced(3, 1, 0, 0);

    // Transition depth buffer back to depth write
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));

    // Transition back buffer to present
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

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
    switch (key)
    {
    case 'W':
        mCamera.MoveForward();
        break;
    case 'S':
        mCamera.MoveBackward();
        break;
    case 'A':
        mCamera.MoveLeft();
        break;
    case 'D':
        mCamera.MoveRight();
        break;
    case 'Q':
        mCamera.MoveUp();
        break;
    case 'E':
        mCamera.MoveDown();
        break;
    case 'R':
        mCamera.TurnUp();
        break;
    case 'F':
        mCamera.TurnDown();
        break;
    case 'C':
        break;
    // Atmosphere presets
    case '1':
        mAtmosphere.SetCleanAtmosphere();
        OutputDebugStringA("Atmosphere: Clean\n");
        break;
    case '2':
        mAtmosphere.SetDirtyAtmosphere();
        OutputDebugStringA("Atmosphere: Dirty/Polluted\n");
        break;
    case '3':
        mAtmosphere.SetMarsAtmosphere();
        OutputDebugStringA("Atmosphere: Mars\n");
        break;
    case '4':
        mAtmosphere.SetSunsetAtmosphere();
        OutputDebugStringA("Atmosphere: Sunset\n");
        break;
    // Toggle fog
    case 'T':
        mFogEnabled = !mFogEnabled;
        OutputDebugStringA(mFogEnabled ? "Fog: Enabled\n" : "Fog: Disabled\n");
        break;
    }
}

void TerrainApp::CreateRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE heightmapRange;
    heightmapRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE colorRange;
    colorRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

    CD3DX12_ROOT_PARAMETER rootParams[3];
    rootParams[0].InitAsConstantBufferView(0);
    rootParams[1].InitAsDescriptorTable(1, &heightmapRange, D3D12_SHADER_VISIBILITY_ALL);
    rootParams[2].InitAsDescriptorTable(1, &colorRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, rootParams, 1, &samplerDesc,
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
    
    mShaders["skyVS"] = d3dUtil::CompileShader(L"shaders/Sky.hlsl", nullptr, "VS", "vs_5_0");
    OutputDebugStringW(L"Sky VS compiled\n");
    
    mShaders["skyPS"] = d3dUtil::CompileShader(L"shaders/Sky.hlsl", nullptr, "PS", "ps_5_0");
    OutputDebugStringW(L"Sky PS compiled\n");
    
    mShaders["fogVS"] = d3dUtil::CompileShader(L"shaders/Fog.hlsl", nullptr, "VS", "vs_5_0");
    OutputDebugStringW(L"Fog VS compiled\n");
    
    mShaders["fogPS"] = d3dUtil::CompileShader(L"shaders/Fog.hlsl", nullptr, "PS", "ps_5_0");
    OutputDebugStringW(L"Fog PS compiled\n");

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
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = mBackBufferFormat;
    psoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    psoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    psoDesc.DSVFormat = mDepthStencilFormat;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSOs["terrain"])));
    
    // Sky PSO
    std::vector<D3D12_INPUT_ELEMENT_DESC> skyInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    
    D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = {};
    skyPsoDesc.InputLayout = { skyInputLayout.data(), (UINT)skyInputLayout.size() };
    skyPsoDesc.pRootSignature = mSkyRootSignature.Get();
    skyPsoDesc.VS = { mShaders["skyVS"]->GetBufferPointer(), mShaders["skyVS"]->GetBufferSize() };
    skyPsoDesc.PS = { mShaders["skyPS"]->GetBufferPointer(), mShaders["skyPS"]->GetBufferSize() };
    skyPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    skyPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    skyPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    skyPsoDesc.SampleMask = UINT_MAX;
    skyPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    skyPsoDesc.NumRenderTargets = 1;
    skyPsoDesc.RTVFormats[0] = mBackBufferFormat;
    skyPsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    skyPsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    skyPsoDesc.DSVFormat = mDepthStencilFormat;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["sky"])));
    
    // Fog post-process PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC fogPsoDesc = {};
    fogPsoDesc.InputLayout = { nullptr, 0 };  // No vertex input (fullscreen triangle)
    fogPsoDesc.pRootSignature = mFogRootSignature.Get();
    fogPsoDesc.VS = { mShaders["fogVS"]->GetBufferPointer(), mShaders["fogVS"]->GetBufferSize() };
    fogPsoDesc.PS = { mShaders["fogPS"]->GetBufferPointer(), mShaders["fogPS"]->GetBufferSize() };
    fogPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    fogPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    fogPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    fogPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    fogPsoDesc.DepthStencilState.DepthEnable = FALSE;
    fogPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    fogPsoDesc.SampleMask = UINT_MAX;
    fogPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    fogPsoDesc.NumRenderTargets = 1;
    fogPsoDesc.RTVFormats[0] = mBackBufferFormat;
    fogPsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    fogPsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    fogPsoDesc.DSVFormat = mDepthStencilFormat;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&fogPsoDesc, IID_PPV_ARGS(&mPSOs["fog"])));
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
    // +2 for scene render target SRV and depth SRV for fog
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1 + TilesX * TilesY + 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

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
    XMMATRIX viewProj = XMMatrixMultiply(proj, view);

    PassConstants passConstants;
    XMStoreFloat4x4(&passConstants.View, view);
    XMStoreFloat4x4(&passConstants.Proj, proj);
    XMStoreFloat4x4(&passConstants.ViewProj, viewProj);
    passConstants.EyePosW = mCamera.GetPosition();
    passConstants.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    passConstants.NearZ = 1.0f;
    passConstants.FarZ = 10000.0f;
    passConstants.TotalTime = gt.TotalTime();
    passConstants.DeltaTime = gt.DeltaTime();
    passConstants.HeightScale = 800.0f;
    passConstants.NoiseSeed = mNoiseSeed;
    
    // Atmosphere parameters
    auto& params = mAtmosphere.GetParameters();
    
    // Sun direction based on angle
    float sunY = sinf(mSunAngle);
    float sunXZ = cosf(mSunAngle);
    passConstants.SunDirection = { sunXZ * sinf(mSunAzimuth), sunY, sunXZ * cosf(mSunAzimuth) };
    
    // Normalize sun direction
    XMVECTOR sunVec = XMLoadFloat3(&passConstants.SunDirection);
    sunVec = XMVector3Normalize(sunVec);
    XMStoreFloat3(&passConstants.SunDirection, sunVec);
    
    passConstants.SunIntensity = params.SunIntensity;
    passConstants.RayleighScattering = params.RayleighCoefficients;
    passConstants.PlanetRadius = params.PlanetRadius;
    passConstants.MieScattering = params.MieCoefficients;
    passConstants.AtmosphereRadius = params.PlanetRadius + params.AtmosphereHeight;
    passConstants.RayleighScaleHeight = params.RayleighScaleHeight;
    passConstants.MieScaleHeight = params.MieScaleHeight;
    passConstants.MieAnisotropy = params.MieAnisotropy;
    passConstants.AtmosphereDensity = params.DensityMultiplier;
    
    // Camera position in km (relative to planet surface)
    XMFLOAT3 camPos = mCamera.GetPosition();
    passConstants.CameraPositionKm = { camPos.x * 0.001f, camPos.y * 0.001f, camPos.z * 0.001f };
    passConstants.Exposure = params.Exposure;
    passConstants.NumSamples = params.NumViewSamples;
    passConstants.NumLightSamples = params.NumLightSamples;
    
    // Fog parameters
    passConstants.FogInscatteringColor = { 0.5f, 0.6f, 0.7f };
    passConstants.FogDensity = mFogDensity;
    passConstants.FogHeightFalloff = mFogHeightFalloff;
    passConstants.FogHeight = 0.0f;
    passConstants.FogStartDistance = 0.0f;
    passConstants.FogCutoffDistance = 5000.0f;
    passConstants.FogMaxOpacity = 1.0f;
    passConstants.FogEnabled = mFogEnabled ? 1 : 0;

    mPassCB->CopyData(0, passConstants);
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

void TerrainApp::CreateSkyRootSignature()
{
    CD3DX12_ROOT_PARAMETER rootParams[1];
    rootParams[0].InitAsConstantBufferView(0);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, rootParams, 0, nullptr,
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
        IID_PPV_ARGS(mSkyRootSignature.GetAddressOf())));
}

void TerrainApp::CreateFogRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE sceneTexTable;
    sceneTexTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    
    CD3DX12_DESCRIPTOR_RANGE depthTexTable;
    depthTexTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

    CD3DX12_ROOT_PARAMETER rootParams[3];
    rootParams[0].InitAsConstantBufferView(0);
    rootParams[1].InitAsDescriptorTable(1, &sceneTexTable, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[2].InitAsDescriptorTable(1, &depthTexTable, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[2];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, rootParams, 2, samplers,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);

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
        IID_PPV_ARGS(mFogRootSignature.GetAddressOf())));
}

void TerrainApp::GenerateSkySphereMesh()
{
    struct SkyVertex
    {
        XMFLOAT3 Pos;
    };

    std::vector<SkyVertex> vertices;
    std::vector<uint16_t> indices;

    const int latBands = 30;
    const int longBands = 30;
    const float radius = 5000.0f;

    for (int lat = 0; lat <= latBands; lat++)
    {
        float theta = lat * XM_PI / latBands;
        float sinTheta = sinf(theta);
        float cosTheta = cosf(theta);

        for (int lon = 0; lon <= longBands; lon++)
        {
            float phi = lon * 2.0f * XM_PI / longBands;
            float sinPhi = sinf(phi);
            float cosPhi = cosf(phi);

            SkyVertex vertex;
            vertex.Pos.x = radius * cosPhi * sinTheta;
            vertex.Pos.y = radius * cosTheta;
            vertex.Pos.z = radius * sinPhi * sinTheta;

            vertices.push_back(vertex);
        }
    }

    for (int lat = 0; lat < latBands; lat++)
    {
        for (int lon = 0; lon < longBands; lon++)
        {
            int first = lat * (longBands + 1) + lon;
            int second = first + longBands + 1;

            indices.push_back((uint16_t)first);
            indices.push_back((uint16_t)second);
            indices.push_back((uint16_t)(first + 1));

            indices.push_back((uint16_t)second);
            indices.push_back((uint16_t)(second + 1));
            indices.push_back((uint16_t)(first + 1));
        }
    }

    mSkyIndexCount = (UINT)indices.size();

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(SkyVertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(uint16_t);

    mSkyVertexBuffer = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, mSkyVertexUploadBuffer);

    mSkyIndexBuffer = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, mSkyIndexUploadBuffer);

    mSkyVBV.BufferLocation = mSkyVertexBuffer->GetGPUVirtualAddress();
    mSkyVBV.StrideInBytes = sizeof(SkyVertex);
    mSkyVBV.SizeInBytes = vbByteSize;

    mSkyIBV.BufferLocation = mSkyIndexBuffer->GetGPUVirtualAddress();
    mSkyIBV.Format = DXGI_FORMAT_R16_UINT;
    mSkyIBV.SizeInBytes = ibByteSize;

    OutputDebugStringA(("Sky sphere created: " + std::to_string(vertices.size()) + 
                        " vertices, " + std::to_string(indices.size()) + " indices\n").c_str());
}

void TerrainApp::UpdateAtmosphereConstants(const GameTimer& gt)
{
    auto& params = mAtmosphere.GetParameters();

    // Sun direction based on angle
    float sunY = sinf(mSunAngle);
    float sunXZ = cosf(mSunAngle);
    
    XMFLOAT3 sunDir = { sunXZ * sinf(mSunAzimuth), sunY, sunXZ * cosf(mSunAzimuth) };
    
    // Normalize
    XMVECTOR sunVec = XMLoadFloat3(&sunDir);
    sunVec = XMVector3Normalize(sunVec);
    XMStoreFloat3(&sunDir, sunVec);
    
    // Update pass constants with atmosphere data
    // (will be copied in UpdateConstants)
}

void TerrainApp::BuildSceneRenderTarget()
{
    // Create intermediate render target for scene (before fog)
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

    D3D12_CLEAR_VALUE optClear;
    optClear.Format = mBackBufferFormat;
    optClear.Color[0] = 0.0f;
    optClear.Color[1] = 0.0f;
    optClear.Color[2] = 0.0f;
    optClear.Color[3] = 1.0f;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &optClear,
        IID_PPV_ARGS(&mSceneRenderTarget)));

    // Create RTV for scene render target
    mSceneRtvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
        SwapChainBufferCount,
        mRtvDescriptorSize);
    md3dDevice->CreateRenderTargetView(mSceneRenderTarget.Get(), nullptr, mSceneRtvHandle);

    // Create SRV for scene render target (in SRV heap)
    int sceneTexIndex = 1 + TilesX * TilesY;
    mSceneSrvCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), sceneTexIndex, mCbvSrvUavDescriptorSize);
    mSceneSrvGpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart(), sceneTexIndex, mCbvSrvUavDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = mBackBufferFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    md3dDevice->CreateShaderResourceView(mSceneRenderTarget.Get(), &srvDesc, mSceneSrvCpuHandle);

    // Create SRV for depth buffer
    int depthTexIndex = sceneTexIndex + 1;
    CD3DX12_CPU_DESCRIPTOR_HANDLE depthSrvCpuHandle(
        mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), depthTexIndex, mCbvSrvUavDescriptorSize);
    mDepthSrvGpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart(), depthTexIndex, mCbvSrvUavDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MostDetailedMip = 0;
    depthSrvDesc.Texture2D.MipLevels = 1;
    md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &depthSrvDesc, depthSrvCpuHandle);
}

void TerrainApp::CreateRtvAndDsvDescriptorHeaps()
{
    OutputDebugStringA("TerrainApp::CreateRtvAndDsvDescriptorHeaps() called\n");
    
    // +1 for scene render target
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
    rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));
    
    OutputDebugStringA("RTV Heap created successfully\n");

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
    
    OutputDebugStringA("DSV Heap created successfully\n");
}
