#pragma once

#include "d3dApp.h"
#include "d3dUtil.h"
#include "Camera.h"
#include "UploadBuffer.h"
#include "MathHelper.h"
#include "VoxelWorld.h"
#include "MarchingTables.h"
#include "TemporalAA.h"

#include <DirectXCollision.h>

struct MCVertex
{
    DirectX::XMFLOAT3 PosW;
    DirectX::XMFLOAT3 NormalW;
    DirectX::XMFLOAT2 Padding = { 0.0f, 0.0f };
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();

    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float IsoLevel = 0.0f;

    float TexScale = 0.07f;
    float Ambient = 0.22f;
    DirectX::XMFLOAT2 Padding0 = { 0.0f, 0.0f };

    DirectX::XMFLOAT3 LightDir = { 0.35f, -0.9f, 0.22f };
    float Padding1 = 0.0f;
};

struct MCChunkConstants
{
    DirectX::XMFLOAT3 WorldOrigin = { 0.0f, 0.0f, 0.0f };
    float VoxelSize = 1.0f;

    UINT DimX = 0;
    UINT DimY = 0;
    UINT DimZ = 0;
    float IsoLevel = 0.0f;

    UINT MaxVertexCount = 0;
    DirectX::XMFLOAT3 Padding = { 0.0f, 0.0f, 0.0f };
};

struct TAAConstants
{
    DirectX::XMFLOAT2 JitterOffset = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 ScreenSize = { 0.0f, 0.0f };
    float BlendFactor = 0.1f;
    DirectX::XMFLOAT3 Padding = { 0.0f, 0.0f, 0.0f };
};

class TerrainApp : public D3DApp
{
public:
    TerrainApp(HINSTANCE hInstance);
    TerrainApp(const TerrainApp& rhs) = delete;
    TerrainApp& operator=(const TerrainApp& rhs) = delete;
    ~TerrainApp();

    bool Initialize() override;

private:
    void OnResize() override;
    void Update(const GameTimer& gt) override;
    void Draw(const GameTimer& gt) override;

    void OnMouseDown(WPARAM btnState, int x, int y) override;
    void OnMouseUp(WPARAM btnState, int x, int y) override;
    void OnMouseMove(WPARAM btnState, int x, int y) override;
    void OnKeyPressed(const GameTimer& gt, WPARAM key) override;

    void CreateRootSignatures();
    void CreateTAARootSignature();
    void CompileShaders();
    void CreatePipelineStates();
    void CreateDrawCommandSignature();

    void LoadAlbedoTextures();
    void CreateMarchingResources();
    void CreateChunkDensityTextures();
    void CreateDescriptorHeap();

    void UploadChunkDensity(VoxelChunk& chunk);

    void UpdatePassConstants();
    void UpdateTAAConstants();
    void CullChunks();
    DirectX::BoundingFrustum BuildFrustum() const;

    void RenderChunk(VoxelChunk& chunk);
    void ResolveTAA();
    void ApplyDigging();
    void SetCameraMode(bool enabled);
    void UpdateCameraLookFromCursor();

    void TransitionResource(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES& currentState,
        D3D12_RESOURCE_STATES targetState);

    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 2> GetStaticSamplers();
    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 2> GetTAAStaticSamplers();

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGraphicsRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mTAARootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mDrawCommandSignature = nullptr;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSrvUavHeap = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mOffscreenRtvHeap = nullptr;

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> mShaders;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    std::unique_ptr<Texture> mStoneTexture = nullptr;
    std::unique_ptr<Texture> mGrassTexture = nullptr;

    VoxelWorld mVoxelWorld;
    std::vector<VoxelChunk*> mVisibleChunks;

    std::unique_ptr<UploadBuffer<PassConstants>> mPassCB = nullptr;
    std::unique_ptr<UploadBuffer<MCChunkConstants>> mChunkCB = nullptr;
    std::unique_ptr<UploadBuffer<MarchingTablesConstants>> mTablesCB = nullptr;
    std::unique_ptr<UploadBuffer<TAAConstants>> mTAACB = nullptr;

    std::unique_ptr<TemporalAA> mTemporalAA = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSceneColorBuffer = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mMCVertexBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounterBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDrawArgsBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounterResetUpload = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDrawArgsUpload = nullptr;

    D3D12_VERTEX_BUFFER_VIEW mMCVBV = {};

    D3D12_RESOURCE_STATES mMCVertexState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mCounterState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mDrawArgsState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mSceneColorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mTAAOutputState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mTAAHistoryState = D3D12_RESOURCE_STATE_COMMON;

    UINT mTextureSrvStart = 0;
    UINT mChunkSrvStart = 0;
    UINT mSceneColorSrvIndex = 0;
    UINT mTAAOutputSrvIndex = 0;
    UINT mTAAHistorySrvIndex = 0;
    UINT mVertexUavIndex = 0;
    UINT mCounterUavIndex = 0;
    UINT mSceneColorRtvIndex = 0;
    UINT mTAAOutputRtvIndex = 1;
    UINT mTAAHistoryRtvIndex = 2;

    UINT mMaxMCVertices = 0;

    Camera mCamera;
    POINT mLastMousePos = {};
    bool mCameraModeActive = false;
    bool mIgnoreNextMouseDelta = false;
    bool mCursorHidden = false;

    bool mWireframeMode = false;
    bool mTAAEnabled = false;
    float mTexScale = 0.07f;
    float mMouseSensitivity = 0.12f;
    float mDigRadius = 8.0f;
    float mDigStrength = 7.5f;
    float mDigMaxDistance = 260.0f;
    double mLastDigTime = -1.0;
    double mDigIntervalSeconds = 0.03;
    int mTAAFrameIndex = 0;
};
