#include "TerrainApp.h"
#include "DDSTextureLoader.h"
#include <sstream>
#include <ctime>
#include <cstdlib>
#include <fstream>

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

    LoadTerrainTextures();
    CreateRootSignature();
    CreateComputeRootSignature();
    CompileShaders();
    GenerateTerrainMesh();
    SetupDescriptorHeaps();
    InitializeCraterMap();
    CreatePipelineState();
    CreateComputePipelineState();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    FlushCommandQueue();

    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(md3dDevice.Get(), 1, true);
    mCraterParamsCB = std::make_unique<UploadBuffer<CraterParams>>(md3dDevice.Get(), 1, true);

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
}

void TerrainApp::Update(const GameTimer& gt)
{
    UpdateConstants(gt);
    CullTiles();
}

void TerrainApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSOs["terrain"].Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

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
    mCommandList->SetGraphicsRootDescriptorTable(1, heightmapHandle);

    CD3DX12_GPU_DESCRIPTOR_HANDLE craterMapHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    craterMapHandle.Offset(mCraterMapSrvIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(2, craterMapHandle);

    mCommandList->IASetVertexBuffers(0, 1, &mTerrainVBV);
    mCommandList->IASetIndexBuffer(&mTerrainIBV);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);

    static int debugFrame = 0;
    if (debugFrame++ % 120 == 0)
    {
        OutputDebugStringA(("Drawing " + std::to_string(mVisibleTiles.size()) + " tiles, textures: " + std::to_string(mTextures.size()) + "\n").c_str());
        XMFLOAT3 pos = mCamera.GetPosition();
        OutputDebugStringA(("Camera pos: " + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z) + "\n").c_str());
    }

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
        float deltaY = -0.15f * static_cast<float>(y - mLastMousePos.y);  // Инвертировано

        mCamera.RotateYaw(deltaX);
        mCamera.RotatePitch(deltaY);
    }

    bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrlPressed)
    {
        // Преобразуем координаты мыши в NDC [-1, 1]
        float mouseNdcX = (2.0f * x) / mClientWidth - 1.0f;
        float mouseNdcY = 1.0f - (2.0f * y) / mClientHeight;

        // Получаем матрицы (они уже транспонированы в Camera)
        // Поэтому нужно транспонировать обратно для правильных вычислений
        XMMATRIX view = XMMatrixTranspose(mCamera.GetViewMatrix());
        XMMATRIX proj = XMMatrixTranspose(mCamera.GetProjectionMatrix());
        
        // Альтернативный метод: используем invProj и invView отдельно
        XMMATRIX invView = XMMatrixInverse(nullptr, view);
        XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
        
        // Точка в clip space
        XMVECTOR rayClip = XMVectorSet(mouseNdcX, mouseNdcY, 1.0f, 1.0f);
        
        // Преобразуем в view space
        XMVECTOR rayView = XMVector4Transform(rayClip, invProj);
        rayView = XMVectorSet(XMVectorGetX(rayView), XMVectorGetY(rayView), 1.0f, 0.0f);
        
        // Преобразуем в world space
        XMVECTOR rayWorld = XMVector4Transform(rayView, invView);
        XMVECTOR rayDir = XMVector3Normalize(rayWorld);

        XMFLOAT3 rayOrigin = mCamera.GetPosition();
        XMFLOAT3 rayDirection;
        XMStoreFloat3(&rayDirection, rayDir);

        // Отладка
        static int debugCounter = 0;
        bool shouldDebug = (++debugCounter % 30 == 0);
        
        if (shouldDebug)
        {
            OutputDebugStringA("\n=== CRATER DEBUG ===\n");
            OutputDebugStringA((std::string("Mouse screen: (" + std::to_string(x) + "," + std::to_string(y) + ")\n").c_str()));
            OutputDebugStringA((std::string("Mouse NDC: (" + std::to_string(mouseNdcX) + "," + std::to_string(mouseNdcY) + ")\n").c_str()));
            OutputDebugStringA((std::string("Camera: (" + std::to_string(rayOrigin.x) + "," + 
                              std::to_string(rayOrigin.y) + "," + std::to_string(rayOrigin.z) + ")\n").c_str()));
            OutputDebugStringA((std::string("Ray dir: (" + std::to_string(rayDirection.x) + "," + 
                              std::to_string(rayDirection.y) + "," + std::to_string(rayDirection.z) + ")\n").c_str()));
        }

        XMFLOAT3 hitPos;
        if (RayTerrainIntersection(rayOrigin, rayDirection, hitPos))
        {
            XMFLOAT2 uv = WorldToUV(hitPos);
            
            if (shouldDebug)
            {
                OutputDebugStringA((std::string("HIT: world(" + std::to_string(hitPos.x) + "," + 
                                  std::to_string(hitPos.y) + "," + std::to_string(hitPos.z) + 
                                  ") UV(" + std::to_string(uv.x) + "," + std::to_string(uv.y) + ")\n").c_str()));
                
                // Проверяем обратное преобразование
                float worldX = uv.x * (TilesX * TileSize);
                float worldZ = (1.0f - uv.y) * (TilesY * TileSize);
                OutputDebugStringA((std::string("UV->World check: (" + std::to_string(worldX) + "," + std::to_string(worldZ) + ")\n").c_str()));
                
                OutputDebugStringA("====================\n\n");
            }
            
            ApplyCraterDeformation(uv);
        }
        else if (shouldDebug)
        {
            OutputDebugStringA("MISS\n====================\n\n");
        }
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
    }
}

void TerrainApp::CreateRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE heightmapRange;
    heightmapRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE craterMapRange;
    craterMapRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

    CD3DX12_DESCRIPTOR_RANGE colorRange;
    colorRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

    CD3DX12_ROOT_PARAMETER rootParams[4];
    rootParams[0].InitAsConstantBufferView(0);
    rootParams[1].InitAsDescriptorTable(1, &heightmapRange, D3D12_SHADER_VISIBILITY_ALL);
    rootParams[2].InitAsDescriptorTable(1, &craterMapRange, D3D12_SHADER_VISIBILITY_ALL);
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

    mShaders["craterCS"] = d3dUtil::CompileShader(L"shaders/CraterDeformation.hlsl", nullptr, "CS", "cs_5_0");
    OutputDebugStringW(L"CS compiled\n");

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

            tile.ColorTextureIndex = 2 + tileY * TilesX + tileX;
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
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 2 + TilesX * TilesY;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
    uavHeapDesc.NumDescriptors = 1;
    uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&mUavDescriptorHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    for (size_t i = 0; i < mTextures.size(); i++)
    {
        auto& tex = mTextures[i];

        if (tex->Resource != nullptr)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = tex->Resource->GetDesc().Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
            srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

            md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, hDescriptor);
        }
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

    // Загружаем CPU копию heightmap для ray casting
    {
        // Открываем файл
        std::ifstream file("Terrain/003/Height_Out.dds", std::ios::binary);
        if (!file.is_open())
        {
            OutputDebugStringA("ERROR: Failed to open heightmap file for CPU copy\n");
            return;
        }

        // Читаем magic number "DDS "
        char magic[4];
        file.read(magic, 4);
        if (magic[0] != 'D' || magic[1] != 'D' || magic[2] != 'S' || magic[3] != ' ')
        {
            OutputDebugStringA("ERROR: Invalid DDS file format\n");
            return;
        }

        // Читаем DDS_HEADER (124 байта)
        struct DDS_HEADER
        {
            UINT dwSize;
            UINT dwFlags;
            UINT dwHeight;
            UINT dwWidth;
            UINT dwPitchOrLinearSize;
            UINT dwDepth;
            UINT dwMipMapCount;
            UINT dwReserved1[11];
            // DDS_PIXELFORMAT (32 байта)
            struct {
                UINT dwSize;
                UINT dwFlags;
                UINT dwFourCC;
                UINT dwRGBBitCount;
                UINT dwRBitMask;
                UINT dwGBitMask;
                UINT dwBBitMask;
                UINT dwABitMask;
            } ddspf;
            UINT dwCaps;
            UINT dwCaps2;
            UINT dwCaps3;
            UINT dwCaps4;
            UINT dwReserved2;
        };

        DDS_HEADER header;
        file.read(reinterpret_cast<char*>(&header), sizeof(DDS_HEADER));

        mHeightmapWidth = header.dwWidth;
        mHeightmapHeight = header.dwHeight;

        OutputDebugStringA(("Heightmap dimensions: " + std::to_string(mHeightmapWidth) + 
                           "x" + std::to_string(mHeightmapHeight) + "\n").c_str());
        OutputDebugStringA(("FourCC: 0x" + std::to_string(header.ddspf.dwFourCC) + 
                           ", RGBBitCount: " + std::to_string(header.ddspf.dwRGBBitCount) + "\n").c_str());

        // Проверяем FourCC для DX10 формата
        if (header.ddspf.dwFourCC == 0x30315844) // "DX10"
        {
            OutputDebugStringA("DX10 format detected, skipping extended header\n");
            // Пропускаем DDS_HEADER_DXT10 (20 байт)
            file.seekg(20, std::ios::cur);
        }

        // Определяем формат - пробуем R16_UNORM (2 байта на пиксель)
        size_t pixelSize = 2; // R16_UNORM
        if (header.ddspf.dwRGBBitCount == 32)
        {
            pixelSize = 4; // R32_FLOAT
            OutputDebugStringA("Using R32_FLOAT format (4 bytes per pixel)\n");
        }
        else if (header.ddspf.dwRGBBitCount == 16)
        {
            pixelSize = 2; // R16_UNORM
            OutputDebugStringA("Using R16_UNORM format (2 bytes per pixel)\n");
        }
        else
        {
            OutputDebugStringA(("Unknown format, defaulting to 2 bytes per pixel\n"));
            pixelSize = 2;
        }

        // Читаем данные пикселей
        size_t dataSize = mHeightmapWidth * mHeightmapHeight * pixelSize;
        std::vector<uint8_t> rawData(dataSize);
        file.read(reinterpret_cast<char*>(rawData.data()), dataSize);
        
        size_t bytesRead = file.gcount();
        OutputDebugStringA(("Read " + std::to_string(bytesRead) + " bytes (expected " + std::to_string(dataSize) + ")\n").c_str());
        
        file.close();

        // Конвертируем в float массив
        mHeightmapData.resize(mHeightmapWidth * mHeightmapHeight);
        
        if (pixelSize == 2)
        {
            // R16_UNORM: конвертируем из uint16 [0, 65535] в float [0, 800]
            const uint16_t* data16 = reinterpret_cast<const uint16_t*>(rawData.data());
            for (size_t i = 0; i < mHeightmapData.size(); ++i)
            {
                // Нормализуем [0, 65535] -> [0, 1] -> [0, 800]
                mHeightmapData[i] = (data16[i] / 65535.0f) * 800.0f;
            }
            OutputDebugStringA("Converted from R16_UNORM to float\n");
        }
        else
        {
            // R32_FLOAT: копируем напрямую
            memcpy(mHeightmapData.data(), rawData.data(), dataSize);
            OutputDebugStringA("Copied R32_FLOAT data directly\n");
        }

        // Отладка: выводим несколько значений высот
        if (mHeightmapData.size() > 0)
        {
            float minH = mHeightmapData[0];
            float maxH = mHeightmapData[0];
            int validCount = 0;
            for (float h : mHeightmapData)
            {
                if (!std::isnan(h) && !std::isinf(h))
                {
                    minH = (std::min)(minH, h);
                    maxH = (std::max)(maxH, h);
                    validCount++;
                }
            }
            OutputDebugStringA(("Valid pixels: " + std::to_string(validCount) + " / " + std::to_string(mHeightmapData.size()) + "\n").c_str());
            OutputDebugStringA(("Height range: " + std::to_string(minH) + 
                               " to " + std::to_string(maxH) + "\n").c_str());
            
            // Выводим первые несколько значений
            OutputDebugStringA("First 10 values: ");
            for (int i = 0; i < 10 && i < (int)mHeightmapData.size(); ++i)
            {
                OutputDebugStringA((std::to_string(mHeightmapData[i]) + " ").c_str());
            }
            OutputDebugStringA("\n");
        }
    }

    auto craterTex = std::make_unique<Texture>();
    craterTex->Name = "cratermap";
    mCraterMapSrvIndex = 1;
    mTextures.push_back(std::move(craterTex));

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

void TerrainApp::CreateComputeRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

    CD3DX12_ROOT_PARAMETER rootParams[2];
    rootParams[0].InitAsConstantBufferView(0);
    rootParams[1].InitAsDescriptorTable(1, &uavRange);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

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
        IID_PPV_ARGS(mComputeRootSignature.GetAddressOf())));
}

void TerrainApp::CreateComputePipelineState()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
    computePsoDesc.pRootSignature = mComputeRootSignature.Get();
    computePsoDesc.CS = { mShaders["craterCS"]->GetBufferPointer(), mShaders["craterCS"]->GetBufferSize() };
    computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ThrowIfFailed(md3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&mPSOs["crater"])));
}

void TerrainApp::InitializeCraterMap()
{
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mCraterMapWidth;
    texDesc.Height = mCraterMapHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&mCraterMap)));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    srvHandle.Offset(mCraterMapSrvIndex, mCbvSrvUavDescriptorSize);
    md3dDevice->CreateShaderResourceView(mCraterMap.Get(), &srvDesc, srvHandle);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(mUavDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    md3dDevice->CreateUnorderedAccessView(mCraterMap.Get(), nullptr, &uavDesc, uavHandle);

    FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CD3DX12_GPU_DESCRIPTOR_HANDLE uavGpuHandle(mUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    mCommandList->ClearUnorderedAccessViewFloat(uavGpuHandle, uavHandle, mCraterMap.Get(), clearColor, 0, nullptr);

    // Transition to pixel shader resource state for rendering
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mCraterMap.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    mTextures[mCraterMapSrvIndex]->Resource = mCraterMap;
    
    OutputDebugStringA("Crater map initialized and transitioned to SRV state\n");
}

void TerrainApp::ApplyCraterDeformation(const XMFLOAT2& uv)
{
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSOs["crater"].Get()));

    // Transition crater map to UAV state
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mCraterMap.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

    CraterParams params;
    params.CenterUV = uv;
    params.RadiusUV = mCraterRadius;
    params.Depth = mCraterDepth;
    mCraterParamsCB->CopyData(0, params);

    ID3D12DescriptorHeap* descriptorHeaps[] = { mUavDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());
    mCommandList->SetComputeRootConstantBufferView(0, mCraterParamsCB->Resource()->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(mUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    mCommandList->SetComputeRootDescriptorTable(1, uavHandle);

    UINT numGroupsX = (mCraterMapWidth + 7) / 8;
    UINT numGroupsY = (mCraterMapHeight + 7) / 8;
    mCommandList->Dispatch(numGroupsX, numGroupsY, 1);

    // Transition back to SRV state
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mCraterMap.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
    FlushCommandQueue();
}

bool TerrainApp::RayTerrainIntersection(const XMFLOAT3& rayOrigin, const XMFLOAT3& rayDir, XMFLOAT3& hitPos)
{
    float terrainSize = (float)(TilesX * TileSize);
    float terrainMinX = 0.0f;
    float terrainMinZ = 0.0f;
    float terrainMaxX = terrainSize;
    float terrainMaxZ = terrainSize;

    // Проверяем что луч имеет ненулевую вертикальную компоненту
    if (fabs(rayDir.y) < 0.0001f)
    {
        return false;
    }
    
    // Если heightmap не загружена, используем простую аппроксимацию
    if (mHeightmapData.empty())
    {
        // Простое пересечение с плоскостью Y=350
        float t = (350.0f - rayOrigin.y) / rayDir.y;
        if (t < 0.0f)
            return false;
            
        XMVECTOR rayOriginVec = XMLoadFloat3(&rayOrigin);
        XMVECTOR rayDirVec = XMLoadFloat3(&rayDir);
        XMVECTOR point = rayOriginVec + t * rayDirVec;
        XMStoreFloat3(&hitPos, point);
        
        // Проверяем границы
        if (hitPos.x < terrainMinX || hitPos.x > terrainMaxX ||
            hitPos.z < terrainMinZ || hitPos.z > terrainMaxZ)
        {
            return false;
        }
        
        hitPos.y = 350.0f;
        return true;
    }
    
    // Используем итеративный метод с реальной heightmap
    // Начинаем с пересечения с плоскостью на средней высоте террейна
    float startHeight = 400.0f; // Средняя высота террейна
    float t = (startHeight - rayOrigin.y) / rayDir.y;
    
    // Если t отрицательное, пробуем с другой высотой
    if (t < 0.0f)
    {
        // Пробуем с минимальной высотой
        startHeight = 0.0f;
        t = (startHeight - rayOrigin.y) / rayDir.y;
        if (t < 0.0f)
        {
            // Пробуем с максимальной высотой
            startHeight = 800.0f;
            t = (startHeight - rayOrigin.y) / rayDir.y;
            if (t < 0.0f)
                return false;
        }
    }
    
    XMVECTOR rayOriginVec = XMLoadFloat3(&rayOrigin);
    XMVECTOR rayDirVec = XMLoadFloat3(&rayDir);
    XMVECTOR intersection = rayOriginVec + t * rayDirVec;
    
    XMFLOAT3 intersectionPos;
    XMStoreFloat3(&intersectionPos, intersection);
    
    // Проверяем границы
    if (intersectionPos.x < terrainMinX || intersectionPos.x > terrainMaxX ||
        intersectionPos.z < terrainMinZ || intersectionPos.z > terrainMaxZ)
    {
        return false;
    }
    
    // Итеративное уточнение (15 итераций для лучшей точности)
    for (int i = 0; i < 15; ++i)
    {
        XMFLOAT2 uv = WorldToUV(intersectionPos);
        float actualHeight = SampleHeightmap(uv.x, uv.y);
        
        // Вычисляем новое t чтобы достичь реальной высоты
        t = (actualHeight - rayOrigin.y) / rayDir.y;
        if (t < 0.0f)
            return false;
        
        intersection = rayOriginVec + t * rayDirVec;
        XMStoreFloat3(&intersectionPos, intersection);
        
        // Проверяем границы после уточнения
        if (intersectionPos.x < terrainMinX || intersectionPos.x > terrainMaxX ||
            intersectionPos.z < terrainMinZ || intersectionPos.z > terrainMaxZ)
        {
            return false;
        }
        
        // Если достаточно близко к поверхности, считаем что попали
        float heightDiff = fabs(intersectionPos.y - actualHeight);
        if (heightDiff < 1.0f)
        {
            hitPos = intersectionPos;
            hitPos.y = actualHeight;
            return true;
        }
    }
    
    // Даже если не сошлось идеально, возвращаем последнюю позицию
    hitPos = intersectionPos;
    return true;
}

XMFLOAT2 TerrainApp::WorldToUV(const XMFLOAT3& worldPos)
{
    float terrainSize = (float)(TilesX * TileSize);
    XMFLOAT2 uv;
    uv.x = worldPos.x / terrainSize;
    uv.y = 1.0f - (worldPos.z / terrainSize);  // Инвертируем V, как в генерации вершин
    uv.x = (std::max)(0.0f, (std::min)(uv.x, 1.0f));
    uv.y = (std::max)(0.0f, (std::min)(uv.y, 1.0f));
    return uv;
}

float TerrainApp::SampleHeightmap(float u, float v)
{
    // Если heightmap не загружена, возвращаем 0
    if (mHeightmapData.empty() || mHeightmapWidth == 0 || mHeightmapHeight == 0)
    {
        return 0.0f;
    }

    // Clamp UV координаты
    u = (std::max)(0.0f, (std::min)(u, 1.0f));
    v = (std::max)(0.0f, (std::min)(v, 1.0f));

    // Конвертируем UV в пиксельные координаты
    float fx = u * (mHeightmapWidth - 1);
    float fy = v * (mHeightmapHeight - 1);

    // Билинейная интерполяция
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = (std::min)(x0 + 1, (int)mHeightmapWidth - 1);
    int y1 = (std::min)(y0 + 1, (int)mHeightmapHeight - 1);

    float tx = fx - x0;
    float ty = fy - y0;

    // Получаем 4 значения высоты
    float h00 = mHeightmapData[y0 * mHeightmapWidth + x0];
    float h10 = mHeightmapData[y0 * mHeightmapWidth + x1];
    float h01 = mHeightmapData[y1 * mHeightmapWidth + x0];
    float h11 = mHeightmapData[y1 * mHeightmapWidth + x1];

    // Билинейная интерполяция
    float h0 = h00 * (1.0f - tx) + h10 * tx;
    float h1 = h01 * (1.0f - tx) + h11 * tx;
    float height = h0 * (1.0f - ty) + h1 * ty;

    return height;
}
