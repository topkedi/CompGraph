struct VertexOut
{
    float3 PosW;
    float3 NormalW;
    float2 Padding;
};

Texture3D<float> gDensity : register(t0);
RWStructuredBuffer<VertexOut> gVertices : register(u0);
RWByteAddressBuffer gVertexCounter : register(u1);

cbuffer ChunkConstants : register(b0)
{
    float3 gWorldOrigin;
    float gVoxelSize;

    uint gDimX;
    uint gDimY;
    uint gDimZ;
    float gIsoLevel;

    uint gMaxVertexCount;
    float3 gPadding;
};

cbuffer MarchingTables : register(b1)
{
    int4 gEdgeTable[64];
    int4 gTriTable[256][4];
};

static const int3 kCornerOffset[8] =
{
    int3(0, 0, 0),
    int3(1, 0, 0),
    int3(1, 1, 0),
    int3(0, 1, 0),
    int3(0, 0, 1),
    int3(1, 0, 1),
    int3(1, 1, 1),
    int3(0, 1, 1)
};

static const int2 kEdgeCorners[12] =
{
    int2(0, 1),
    int2(1, 2),
    int2(2, 3),
    int2(3, 0),
    int2(4, 5),
    int2(5, 6),
    int2(6, 7),
    int2(7, 4),
    int2(0, 4),
    int2(1, 5),
    int2(2, 6),
    int2(3, 7)
};

float LoadDensity(int3 coord)
{
    int3 maxCoord = int3(gDimX - 1, gDimY - 1, gDimZ - 1);
    int3 clamped = clamp(coord, int3(0, 0, 0), maxCoord);
    return gDensity.Load(int4(clamped, 0));
}

float DensityAt(float3 voxelPos)
{
    int3 nearest = int3(round(voxelPos));
    return LoadDensity(nearest);
}

float3 DensityGradient(float3 voxelPos)
{
    float dx = DensityAt(voxelPos + float3(1.0, 0.0, 0.0)) - DensityAt(voxelPos - float3(1.0, 0.0, 0.0));
    float dy = DensityAt(voxelPos + float3(0.0, 1.0, 0.0)) - DensityAt(voxelPos - float3(0.0, 1.0, 0.0));
    float dz = DensityAt(voxelPos + float3(0.0, 0.0, 1.0)) - DensityAt(voxelPos - float3(0.0, 0.0, 1.0));

    float3 g = float3(dx, dy, dz);
    float lenSq = dot(g, g);
    if (lenSq < 1e-8)
    {
        return float3(0.0, 1.0, 0.0);
    }

    return normalize(g);
}

float3 InterpolateEdge(float3 p0, float3 p1, float d0, float d1)
{
    float denom = d1 - d0;
    float t = 0.5;

    if (abs(denom) > 1e-6)
    {
        t = saturate((gIsoLevel - d0) / denom);
    }

    return lerp(p0, p1, t);
}

int GetTriIndex(int cubeIndex, int triIndex)
{
    int4 packed = gTriTable[cubeIndex][triIndex / 4];
    int component = triIndex & 3;

    if (component == 0)
    {
        return packed.x;
    }

    if (component == 1)
    {
        return packed.y;
    }

    if (component == 2)
    {
        return packed.z;
    }

    return packed.w;
}

int GetEdgeMask(int cubeIndex)
{
    int4 packed = gEdgeTable[cubeIndex / 4];
    int component = cubeIndex & 3;

    if (component == 0)
    {
        return packed.x;
    }

    if (component == 1)
    {
        return packed.y;
    }

    if (component == 2)
    {
        return packed.z;
    }

    return packed.w;
}

[numthreads(8, 8, 8)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (gDimX - 1) ||
        dispatchThreadId.y >= (gDimY - 1) ||
        dispatchThreadId.z >= (gDimZ - 1))
    {
        return;
    }

    float cornerDensity[8];
    float3 cornerVoxelPos[8];
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        int3 sampleCoord = int3(dispatchThreadId) + kCornerOffset[i];
        cornerDensity[i] = LoadDensity(sampleCoord);

        cornerVoxelPos[i] = float3(sampleCoord);
    }

    int cubeIndex = 0;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        if (cornerDensity[i] < gIsoLevel)
        {
            cubeIndex |= (1 << i);
        }
    }

    int edgeMask = GetEdgeMask(cubeIndex);
    if (edgeMask == 0)
    {
        return;
    }

    float3 edgeWorldPos[12];
    float3 edgeNormal[12];

    [unroll]
    for (int edge = 0; edge < 12; ++edge)
    {
        if ((edgeMask & (1 << edge)) == 0)
        {
            continue;
        }

        int2 corners = kEdgeCorners[edge];

        float3 voxelPos = InterpolateEdge(
            cornerVoxelPos[corners.x],
            cornerVoxelPos[corners.y],
            cornerDensity[corners.x],
            cornerDensity[corners.y]);

        edgeWorldPos[edge] = gWorldOrigin + voxelPos * gVoxelSize;
        edgeNormal[edge] = DensityGradient(voxelPos);
    }

    [unroll]
    for (int tri = 0; tri < 5; ++tri)
    {
        int index0 = GetTriIndex(cubeIndex, tri * 3 + 0);
        if (index0 < 0)
        {
            break;
        }

        int index1 = GetTriIndex(cubeIndex, tri * 3 + 1);
        int index2 = GetTriIndex(cubeIndex, tri * 3 + 2);

        float3 p0 = edgeWorldPos[index0];
        float3 p1 = edgeWorldPos[index1];
        float3 p2 = edgeWorldPos[index2];

        float3 n0 = edgeNormal[index0];
        float3 n1 = edgeNormal[index1];
        float3 n2 = edgeNormal[index2];

        float3 faceNormal = normalize(cross(p1 - p0, p2 - p0));
        float3 avgGradient = normalize(n0 + n1 + n2);

        if (dot(faceNormal, avgGradient) < 0.0)
        {
            float3 tmpPos = p1;
            p1 = p2;
            p2 = tmpPos;

            float3 tmpNormal = n1;
            n1 = n2;
            n2 = tmpNormal;
        }

        uint baseVertex = 0;
        gVertexCounter.InterlockedAdd(0, 3, baseVertex);

        if (baseVertex + 2 >= gMaxVertexCount)
        {
            return;
        }

        VertexOut v0;
        v0.PosW = p0;
        v0.NormalW = normalize(n0);
        v0.Padding = float2(0.0, 0.0);

        VertexOut v1;
        v1.PosW = p1;
        v1.NormalW = normalize(n1);
        v1.Padding = float2(0.0, 0.0);

        VertexOut v2;
        v2.PosW = p2;
        v2.NormalW = normalize(n2);
        v2.Padding = float2(0.0, 0.0);

        gVertices[baseVertex + 0] = v0;
        gVertices[baseVertex + 1] = v1;
        gVertices[baseVertex + 2] = v2;
    }
}
