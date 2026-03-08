// Crater Deformation Compute Shader
cbuffer CraterParams : register(b0)
{
    float2 gCenterUV;
    float  gRadiusUV;
    float  gDepth;
};

RWTexture2D<float> gCraterMap : register(u0);

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 texCoord = dispatchThreadID.xy;
    uint width, height;
    gCraterMap.GetDimensions(width, height);
    
    if (texCoord.x >= width || texCoord.y >= height)
        return;
    
    float2 texelUV = (float2(texCoord) + 0.5f) / float2(width, height);
    
    if (gRadiusUV >= 1.0f)
    {
        gCraterMap[texCoord] = gDepth;
        return;
    }
    
    float2 delta = texelUV - gCenterUV;
    float distance = length(delta);
    
    if (distance < gRadiusUV)
    {
        float t = distance / gRadiusUV;
        float falloff = 1.0f - t * t;
        float deformation = gDepth * falloff;
        gCraterMap[texCoord] += deformation;
    }
}
