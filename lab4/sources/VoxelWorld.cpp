#include "VoxelWorld.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

using namespace DirectX;
using namespace DirectX::PackedVector;

VoxelWorld::PerlinNoise::PerlinNoise(std::uint32_t seed)
{
    mPerm.resize(512);

    std::vector<int> values(256);
    std::iota(values.begin(), values.end(), 0);

    std::mt19937 rng(seed);
    std::shuffle(values.begin(), values.end(), rng);

    for (int i = 0; i < 256; ++i)
    {
        mPerm[i] = values[i];
        mPerm[i + 256] = values[i];
    }
}

float VoxelWorld::PerlinNoise::Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float VoxelWorld::PerlinNoise::Lerp(float t, float a, float b)
{
    return a + t * (b - a);
}

float VoxelWorld::PerlinNoise::Grad(int hash, float x, float y, float z)
{
    const int h = hash & 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    const float a = (h & 1) == 0 ? u : -u;
    const float b = (h & 2) == 0 ? v : -v;
    return a + b;
}

float VoxelWorld::PerlinNoise::Noise(float x, float y, float z) const
{
    const int X = static_cast<int>(std::floor(x)) & 255;
    const int Y = static_cast<int>(std::floor(y)) & 255;
    const int Z = static_cast<int>(std::floor(z)) & 255;

    x -= std::floor(x);
    y -= std::floor(y);
    z -= std::floor(z);

    const float u = Fade(x);
    const float v = Fade(y);
    const float w = Fade(z);

    const int A = mPerm[X] + Y;
    const int AA = mPerm[A] + Z;
    const int AB = mPerm[A + 1] + Z;
    const int B = mPerm[X + 1] + Y;
    const int BA = mPerm[B] + Z;
    const int BB = mPerm[B + 1] + Z;

    return Lerp(w,
        Lerp(v,
            Lerp(u, Grad(mPerm[AA], x, y, z), Grad(mPerm[BA], x - 1.0f, y, z)),
            Lerp(u, Grad(mPerm[AB], x, y - 1.0f, z), Grad(mPerm[BB], x - 1.0f, y - 1.0f, z))),
        Lerp(v,
            Lerp(u, Grad(mPerm[AA + 1], x, y, z - 1.0f), Grad(mPerm[BA + 1], x - 1.0f, y, z - 1.0f)),
            Lerp(u, Grad(mPerm[AB + 1], x, y - 1.0f, z - 1.0f), Grad(mPerm[BB + 1], x - 1.0f, y - 1.0f, z - 1.0f))));
}

VoxelWorld::VoxelWorld(const VoxelWorldSettings& settings)
    : mSettings(settings)
{
    const float spanX = static_cast<float>(mSettings.DimX - 1) * mSettings.VoxelSize;
    const float spanY = static_cast<float>(mSettings.DimY - 1) * mSettings.VoxelSize;
    const float spanZ = static_cast<float>(mSettings.DimZ - 1) * mSettings.VoxelSize;

    const int halfX = mSettings.ChunkCountX / 2;
    const int halfZ = mSettings.ChunkCountZ / 2;

    mChunks.reserve(static_cast<std::size_t>(mSettings.ChunkCountX * mSettings.ChunkCountZ));

    for (int z = 0; z < mSettings.ChunkCountZ; ++z)
    {
        for (int x = 0; x < mSettings.ChunkCountX; ++x)
        {
            VoxelChunk chunk;
            chunk.LinearIndex = static_cast<UINT>(mChunks.size());
            chunk.GridX = x;
            chunk.GridZ = z;

            chunk.WorldOrigin = XMFLOAT3(
                static_cast<float>(x - halfX) * spanX,
                mSettings.VerticalOrigin,
                static_cast<float>(z - halfZ) * spanZ);

            chunk.VoxelSize = mSettings.VoxelSize;
            chunk.DimX = mSettings.DimX;
            chunk.DimY = mSettings.DimY;
            chunk.DimZ = mSettings.DimZ;
            chunk.IsoLevel = mSettings.IsoLevel;

            const std::size_t voxelCount =
                static_cast<std::size_t>(chunk.DimX) *
                static_cast<std::size_t>(chunk.DimY) *
                static_cast<std::size_t>(chunk.DimZ);

            chunk.DensityFloat.resize(voxelCount, 0.0f);
            chunk.DensityHalf.resize(voxelCount, 0u);

            const XMFLOAT3 minBound = chunk.WorldOrigin;
            const XMFLOAT3 maxBound(
                chunk.WorldOrigin.x + spanX,
                chunk.WorldOrigin.y + spanY,
                chunk.WorldOrigin.z + spanZ);

            const XMFLOAT3 center(
                (minBound.x + maxBound.x) * 0.5f,
                (minBound.y + maxBound.y) * 0.5f,
                (minBound.z + maxBound.z) * 0.5f);

            const XMFLOAT3 extents(
                (maxBound.x - minBound.x) * 0.5f,
                (maxBound.y - minBound.y) * 0.5f,
                (maxBound.z - minBound.z) * 0.5f);

            chunk.Bounds = BoundingBox(center, extents);
            chunk.Dirty = true;

            mChunks.push_back(std::move(chunk));
        }
    }
}

void VoxelWorld::GenerateAll()
{
    for (VoxelChunk& chunk : mChunks)
    {
        RegenerateChunk(chunk);
    }
}

bool VoxelWorld::DigSphere(const XMFLOAT3& center, float radius, float strength)
{
    if (radius <= 0.0f)
    {
        return false;
    }

    const BoundingSphere digSphere(center, radius);
    const float radiusSq = radius * radius;
    bool anyModified = false;

    for (VoxelChunk& chunk : mChunks)
    {
        if (!chunk.Bounds.Intersects(digSphere))
        {
            continue;
        }

        bool chunkModified = false;

        for (int z = 0; z < chunk.DimZ; ++z)
        {
            for (int y = 0; y < chunk.DimY; ++y)
            {
                for (int x = 0; x < chunk.DimX; ++x)
                {
                    const bool outerBoundary =
                        (chunk.GridX == 0 && x == 0) ||
                        (chunk.GridX == mSettings.ChunkCountX - 1 && x == chunk.DimX - 1) ||
                        (chunk.GridZ == 0 && z == 0) ||
                        (chunk.GridZ == mSettings.ChunkCountZ - 1 && z == chunk.DimZ - 1);

                    if (outerBoundary || y == 0)
                    {
                        continue;
                    }

                    const XMFLOAT3 voxelPos(
                        chunk.WorldOrigin.x + static_cast<float>(x) * chunk.VoxelSize,
                        chunk.WorldOrigin.y + static_cast<float>(y) * chunk.VoxelSize,
                        chunk.WorldOrigin.z + static_cast<float>(z) * chunk.VoxelSize);

                    const float dx = voxelPos.x - center.x;
                    const float dy = voxelPos.y - center.y;
                    const float dz = voxelPos.z - center.z;
                    const float distSq = dx * dx + dy * dy + dz * dz;

                    if (distSq >= radiusSq)
                    {
                        continue;
                    }

                    const float dist = std::sqrt(distSq);
                    const float falloff = 1.0f - (dist / radius);

                    const std::size_t idx = Index(chunk, x, y, z);
                    chunk.DensityFloat[idx] += strength * falloff;
                    chunkModified = true;
                }
            }
        }

        if (chunkModified)
        {
            RepackHalf(chunk);
            chunk.Dirty = true;
            anyModified = true;
        }
    }

    return anyModified;
}

const VoxelChunk* VoxelWorld::FindChunkAtWorld(const XMFLOAT3& worldPos) const
{
    for (const VoxelChunk& chunk : mChunks)
    {
        const float minX = chunk.WorldOrigin.x;
        const float minY = chunk.WorldOrigin.y;
        const float minZ = chunk.WorldOrigin.z;

        const float maxX = minX + static_cast<float>(chunk.DimX - 1) * chunk.VoxelSize;
        const float maxY = minY + static_cast<float>(chunk.DimY - 1) * chunk.VoxelSize;
        const float maxZ = minZ + static_cast<float>(chunk.DimZ - 1) * chunk.VoxelSize;

        if (worldPos.x >= minX && worldPos.x <= maxX &&
            worldPos.y >= minY && worldPos.y <= maxY &&
            worldPos.z >= minZ && worldPos.z <= maxZ)
        {
            return &chunk;
        }
    }

    return nullptr;
}

bool VoxelWorld::SampleDensityAtWorld(const XMFLOAT3& worldPos, float& outDensity) const
{
    const VoxelChunk* chunk = FindChunkAtWorld(worldPos);
    if (chunk == nullptr)
    {
        outDensity = 1.0f;
        return false;
    }

    const float fx = (worldPos.x - chunk->WorldOrigin.x) / chunk->VoxelSize;
    const float fy = (worldPos.y - chunk->WorldOrigin.y) / chunk->VoxelSize;
    const float fz = (worldPos.z - chunk->WorldOrigin.z) / chunk->VoxelSize;

    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, chunk->DimX - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, chunk->DimY - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(fz)), 0, chunk->DimZ - 1);

    const int x1 = (std::min)(x0 + 1, chunk->DimX - 1);
    const int y1 = (std::min)(y0 + 1, chunk->DimY - 1);
    const int z1 = (std::min)(z0 + 1, chunk->DimZ - 1);

    const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
    const float tz = std::clamp(fz - static_cast<float>(z0), 0.0f, 1.0f);

    const float c000 = chunk->DensityFloat[Index(*chunk, x0, y0, z0)];
    const float c100 = chunk->DensityFloat[Index(*chunk, x1, y0, z0)];
    const float c010 = chunk->DensityFloat[Index(*chunk, x0, y1, z0)];
    const float c110 = chunk->DensityFloat[Index(*chunk, x1, y1, z0)];
    const float c001 = chunk->DensityFloat[Index(*chunk, x0, y0, z1)];
    const float c101 = chunk->DensityFloat[Index(*chunk, x1, y0, z1)];
    const float c011 = chunk->DensityFloat[Index(*chunk, x0, y1, z1)];
    const float c111 = chunk->DensityFloat[Index(*chunk, x1, y1, z1)];

    const float c00 = c000 + (c100 - c000) * tx;
    const float c10 = c010 + (c110 - c010) * tx;
    const float c01 = c001 + (c101 - c001) * tx;
    const float c11 = c011 + (c111 - c011) * tx;

    const float c0 = c00 + (c10 - c00) * ty;
    const float c1 = c01 + (c11 - c01) * ty;

    outDensity = c0 + (c1 - c0) * tz;
    return true;
}

bool VoxelWorld::RaycastSurface(
    const XMFLOAT3& origin,
    const XMFLOAT3& direction,
    float maxDistance,
    float step,
    XMFLOAT3& outHitPos,
    float& outHitDistance) const
{
    if (maxDistance <= 0.0f || step <= 0.0f)
    {
        return false;
    }

    XMVECTOR dirVec = XMVector3Normalize(XMLoadFloat3(&direction));
    XMFLOAT3 dir;
    XMStoreFloat3(&dir, dirVec);

    const float iso = mSettings.IsoLevel;

    float prevDensity = 1.0f;
    SampleDensityAtWorld(origin, prevDensity);

    for (float t = step; t <= maxDistance; t += step)
    {
        const XMFLOAT3 samplePos(
            origin.x + dir.x * t,
            origin.y + dir.y * t,
            origin.z + dir.z * t);

        float currDensity = 1.0f;
        SampleDensityAtWorld(samplePos, currDensity);

        const float a = prevDensity - iso;
        const float b = currDensity - iso;

        if ((a > 0.0f && b <= 0.0f) || (a < 0.0f && b >= 0.0f))
        {
            const float denom = prevDensity - currDensity;
            float alpha = 0.5f;
            if (std::abs(denom) > 1e-6f)
            {
                alpha = std::clamp((prevDensity - iso) / denom, 0.0f, 1.0f);
            }

            const float hitT = (t - step) + step * alpha;
            outHitDistance = hitT;
            outHitPos = XMFLOAT3(
                origin.x + dir.x * hitT,
                origin.y + dir.y * hitT,
                origin.z + dir.z * hitT);
            return true;
        }

        prevDensity = currDensity;
    }

    return false;
}

std::size_t VoxelWorld::Index(const VoxelChunk& chunk, int x, int y, int z) const
{
    return (static_cast<std::size_t>(z) * static_cast<std::size_t>(chunk.DimY) + static_cast<std::size_t>(y)) *
        static_cast<std::size_t>(chunk.DimX) + static_cast<std::size_t>(x);
}

void VoxelWorld::RegenerateChunk(VoxelChunk& chunk)
{
    for (int z = 0; z < chunk.DimZ; ++z)
    {
        for (int y = 0; y < chunk.DimY; ++y)
        {
            for (int x = 0; x < chunk.DimX; ++x)
            {
                const std::size_t idx = Index(chunk, x, y, z);
                chunk.DensityFloat[idx] = ComputeDensity(chunk, x, y, z);
            }
        }
    }

    RepackHalf(chunk);
    chunk.Dirty = true;
}

void VoxelWorld::RepackHalf(VoxelChunk& chunk)
{
    for (std::size_t i = 0; i < chunk.DensityFloat.size(); ++i)
    {
        chunk.DensityHalf[i] = XMConvertFloatToHalf(chunk.DensityFloat[i]);
    }
}

float VoxelWorld::ComputeDensity(const VoxelChunk& chunk, int x, int y, int z) const
{
    const float worldX = chunk.WorldOrigin.x + static_cast<float>(x) * chunk.VoxelSize;
    const float worldY = chunk.WorldOrigin.y + static_cast<float>(y) * chunk.VoxelSize;
    const float worldZ = chunk.WorldOrigin.z + static_cast<float>(z) * chunk.VoxelSize;

    const float spanX = static_cast<float>(mSettings.DimX - 1) * mSettings.VoxelSize;
    const float spanZ = static_cast<float>(mSettings.DimZ - 1) * mSettings.VoxelSize;

    const float worldMinX = -static_cast<float>(mSettings.ChunkCountX / 2) * spanX;
    const float worldMinZ = -static_cast<float>(mSettings.ChunkCountZ / 2) * spanZ;
    const float worldMaxX = worldMinX + spanX * static_cast<float>(mSettings.ChunkCountX);
    const float worldMaxZ = worldMinZ + spanZ * static_cast<float>(mSettings.ChunkCountZ);

    const float topNoise = FBM3D(XMFLOAT3(worldX, 0.0f, worldZ), mSettings.TerrainNoise);
    const float topSurfaceY = mSettings.SeaLevel + 22.0f + topNoise * 28.0f;

    const float densityTop = worldY - topSurfaceY;
    const float densityBottom = mSettings.VerticalOrigin - worldY;
    const float densitySideX = (std::max)(worldMinX - worldX, worldX - worldMaxX);
    const float densitySideZ = (std::max)(worldMinZ - worldZ, worldZ - worldMaxZ);

    float densitySolid = (std::max)((std::max)(densityTop, densityBottom), (std::max)(densitySideX, densitySideZ));

    const float caveNoise = FBM3D(XMFLOAT3(worldX, worldY, worldZ), mSettings.CaveNoise);
    float densityCaves = 0.08f - std::abs(caveNoise);
    const float caveDepth = std::clamp((topSurfaceY - worldY - 10.0f) / 36.0f, 0.0f, 1.0f);
    densityCaves *= caveDepth;

    densitySolid = (std::max)(densitySolid, densityCaves);
    return densitySolid;
}

float VoxelWorld::FBM3D(const XMFLOAT3& p, const NoiseSettings& params) const
{
    float frequency = params.Frequency;
    float amplitude = params.Amplitude;

    float value = 0.0f;
    float sumAmplitude = 0.0f;

    for (int octave = 0; octave < params.Octaves; ++octave)
    {
        value += mNoise.Noise(p.x * frequency, p.y * frequency, p.z * frequency) * amplitude;
        sumAmplitude += amplitude;

        frequency *= params.Lacunarity;
        amplitude *= params.Gain;
    }

    if (sumAmplitude > 0.0f)
    {
        value /= sumAmplitude;
    }

    return value;
}
