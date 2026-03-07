#pragma once

#include "d3dApp.h"
#include "d3dUtil.h"
#include "Camera.h"
#include "UploadBuffer.h"
#include "MathHelper.h"
#include "QuadTree.h"
#include "Atmosphere.h"
#include <DirectXCollision.h>

struct TerrainVertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT2 TexC;
    DirectX::XMFLOAT2 LocalTexC;
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float Padding1;
    DirectX::XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
    float NearZ = 0.0f;
    float FarZ = 0.0f;
    float TotalTime = 0.0f;
    float DeltaTime = 0.0f;
    float HeightScale = 500.0f;
    float NoiseSeed;
    
    // Atmosphere parameters
    DirectX::XMFLOAT3 SunDirection = { 0.0f, 0.707f, 0.707f };
    float SunIntensity = 20.0f;
    DirectX::XMFLOAT3 RayleighScattering = { 5.8e-6f, 13.5e-6f, 33.1e-6f };
    float PlanetRadius = 6371.0f;
    DirectX::XMFLOAT3 MieScattering = { 21e-6f, 21e-6f, 21e-6f };
    float AtmosphereRadius = 6471.0f;
    float RayleighScaleHeight = 8500.0f;
    float MieScaleHeight = 1200.0f;
    float MieAnisotropy = 0.76f;
    float AtmosphereDensity = 1.0f;
    DirectX::XMFLOAT3 CameraPositionKm = { 0.0f, 0.0f, 0.0f };
    float Exposure = 1.5f;
    int NumSamples = 16;
    int NumLightSamples = 8;
    DirectX::XMFLOAT2 AtmoPad = { 0.0f, 0.0f };
    
    // Fog parameters
    DirectX::XMFLOAT3 FogInscatteringColor = { 0.5f, 0.6f, 0.7f };
    float FogDensity = 0.02f;
    float FogHeightFalloff = 0.2f;
    float FogHeight = 0.0f;
    float FogStartDistance = 0.0f;
    float FogCutoffDistance = 1000.0f;
    float FogMaxOpacity = 1.0f;
    int FogEnabled = 1;
    DirectX::XMFLOAT2 FogPad = { 0.0f, 0.0f };
};

struct TerrainTileInfo
{
    int TileX;
    int TileY;
    int TileSize;
    DirectX::BoundingBox Bounds;
    UINT VertexOffset;
    UINT IndexOffset;
    UINT IndexCount;
    int ColorTextureIndex;
    int NormalTextureIndex;
};

class TerrainApp : public D3DApp
{
public:
    TerrainApp(HINSTANCE hInstance);
    TerrainApp(const TerrainApp& rhs) = delete;
    TerrainApp& operator=(const TerrainApp& rhs) = delete;
    ~TerrainApp();

    virtual bool Initialize() override;

private:
    virtual void OnResize() override;
    virtual void Update(const GameTimer& gt) override;
    virtual void Draw(const GameTimer& gt) override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y) override;
    virtual void OnKeyPressed(const GameTimer& gt, WPARAM key) override;

    void CreateRootSignature();
    void CreateSkyRootSignature();
    void CreateFogRootSignature();
    void CompileShaders();
    void CreatePipelineState();
    void GenerateTerrainMesh();
    void GenerateSkySphereMesh();
    void SetupDescriptorHeaps();
    void LoadTerrainTextures();
    void UpdateConstants(const GameTimer& gt);
    void UpdateAtmosphereConstants(const GameTimer& gt);
    void CullTiles();
    DirectX::BoundingFrustum BuildFrustum() const;
    void BuildSceneRenderTarget();
    void CreateRtvAndDsvDescriptorHeaps();

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mSkyRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mFogRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> mShaders;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    Microsoft::WRL::ComPtr<ID3D12Resource> mTerrainVertexBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mTerrainIndexBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mTerrainVertexUploadBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mTerrainIndexUploadBuffer = nullptr;

    D3D12_VERTEX_BUFFER_VIEW mTerrainVBV;
    D3D12_INDEX_BUFFER_VIEW mTerrainIBV;
    
    // Sky sphere
    Microsoft::WRL::ComPtr<ID3D12Resource> mSkyVertexBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSkyIndexBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSkyVertexUploadBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSkyIndexUploadBuffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW mSkyVBV;
    D3D12_INDEX_BUFFER_VIEW mSkyIBV;
    UINT mSkyIndexCount = 0;
    
    // Scene render target for fog post-process
    Microsoft::WRL::ComPtr<ID3D12Resource> mSceneRenderTarget = nullptr;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mSceneRtvHandle;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mSceneSrvCpuHandle;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mSceneSrvGpuHandle;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mDepthSrvGpuHandle;

    std::vector<TerrainTileInfo> mTiles;
    std::vector<TerrainTileInfo*> mVisibleTiles;
    
    QuadTree mQuadTree;

    std::vector<std::unique_ptr<Texture>> mTextures;
    int mHeightmapSrvIndex = -1;

    std::unique_ptr<UploadBuffer<PassConstants>> mPassCB = nullptr;

    Camera mCamera;
    POINT mLastMousePos;

    static const int TilesX = 4;
    static const int TilesY = 4;
    static const int TileSize = 256;
    static const int PatchesPerTile = 16;
    
    bool mWireframeMode = false;
    float mNoiseSeed = 0.0f;
    
    // Atmosphere
    Atmosphere mAtmosphere;
    float mSunAngle = 0.5f;
    float mSunAzimuth = 0.0f;
    
    // Fog
    bool mFogEnabled = true;
    float mFogDensity = 0.02f;
    float mFogHeightFalloff = 0.2f;
};
