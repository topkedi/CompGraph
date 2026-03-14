#pragma once

#include <DirectXCollision.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

struct NoiseSettings
{
    float Frequency = 0.01f;
    int Octaves = 4;
    float Lacunarity = 2.0f;
    float Gain = 0.5f;
    float Amplitude = 1.0f;
};

struct VoxelWorldSettings
{
    int ChunkCountX = 5;
    int ChunkCountZ = 5;

    int DimX = 33;
    int DimY = 100;
    int DimZ = 33;

    float VoxelSize = 3.0f;
    float IsoLevel = 0.0f;

    float SeaLevel = 0.0f;
    float VerticalOrigin = -50.0f;

    NoiseSettings TerrainNoise = { 0.0045f, 5, 2.0f, 0.5f, 1.0f };
    NoiseSettings CaveNoise = { 0.012f, 2, 2.0f, 0.5f, 1.0f };
};

struct VoxelChunk
{
    UINT LinearIndex = 0;
    int GridX = 0;
    int GridZ = 0;

    DirectX::XMFLOAT3 WorldOrigin = { 0.0f, 0.0f, 0.0f };
    float VoxelSize = 1.0f;

    int DimX = 0;
    int DimY = 0;
    int DimZ = 0;
    float IsoLevel = 0.0f;

    std::vector<float> DensityFloat;
    std::vector<std::uint16_t> DensityHalf;

    Microsoft::WRL::ComPtr<ID3D12Resource> DensityTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> DensityUpload;
    D3D12_RESOURCE_STATES DensityState = D3D12_RESOURCE_STATE_COPY_DEST;
    UINT DensitySrvIndex = 0;

    DirectX::BoundingBox Bounds;
    bool Dirty = true;
};

class VoxelWorld
{
public:
    explicit VoxelWorld(const VoxelWorldSettings& settings);

    const VoxelWorldSettings& Settings() const { return mSettings; }
    VoxelWorldSettings& Settings() { return mSettings; }

    std::vector<VoxelChunk>& Chunks() { return mChunks; }
    const std::vector<VoxelChunk>& Chunks() const { return mChunks; }

    void GenerateAll();

    bool DigSphere(const DirectX::XMFLOAT3& center, float radius, float strength);
    bool SampleDensityAtWorld(const DirectX::XMFLOAT3& worldPos, float& outDensity) const;
    bool RaycastSurface(
        const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction,
        float maxDistance,
        float step,
        DirectX::XMFLOAT3& outHitPos,
        float& outHitDistance) const;

private:
    class PerlinNoise
    {
    public:
        explicit PerlinNoise(std::uint32_t seed = 1337u);
        float Noise(float x, float y, float z) const;

    private:
        static float Fade(float t);
        static float Lerp(float t, float a, float b);
        static float Grad(int hash, float x, float y, float z);

        std::vector<int> mPerm;
    };

    std::size_t Index(const VoxelChunk& chunk, int x, int y, int z) const;

    void RegenerateChunk(VoxelChunk& chunk);
    void RepackHalf(VoxelChunk& chunk);

    float ComputeDensity(const VoxelChunk& chunk, int x, int y, int z) const;
    float FBM3D(const DirectX::XMFLOAT3& p, const NoiseSettings& params) const;
    const VoxelChunk* FindChunkAtWorld(const DirectX::XMFLOAT3& worldPos) const;

private:
    VoxelWorldSettings mSettings;
    std::vector<VoxelChunk> mChunks;
    PerlinNoise mNoise;
};
