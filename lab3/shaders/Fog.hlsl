//=============================================================================
// Fog.hlsl - Exponential Height Fog Post-Process
// Плотность спадает с высотой: d = GlobalDensity * exp(-HeightFalloff * z)
//=============================================================================

cbuffer cbPass : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float3 gEyePosW;
    float Padding1;
    float2 gRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float gHeightScale;
    float gNoiseSeed;
    
    // Atmosphere parameters
    float3 gSunDirection;
    float gSunIntensity;
    float3 gRayleighScattering;
    float gPlanetRadius;
    float3 gMieScattering;
    float gAtmosphereRadius;
    float gRayleighScaleHeight;
    float gMieScaleHeight;
    float gMieAnisotropy;
    float gAtmosphereDensity;
    float3 gCameraPositionKm;
    float gExposure;
    int gNumSamples;
    int gNumLightSamples;
    float2 gAtmoPad;
    
    // Fog parameters
    float3 gFogInscatteringColor;
    float gFogDensity;
    float gFogHeightFalloff;
    float gFogHeight;
    float gFogStartDistance;
    float gFogCutoffDistance;
    float gFogMaxOpacity;
    int gFogEnabled;
    float2 gFogPad;
};

Texture2D gSceneTexture : register(t0);
Texture2D gDepthTexture : register(t1);

SamplerState gsamPointClamp : register(s0);
SamplerState gsamLinearClamp : register(s1);

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

// Fullscreen triangle (no vertex buffer needed)
VertexOut VS(uint vertexID : SV_VertexID)
{
    VertexOut vout;
    
    // Generate fullscreen triangle
    vout.TexC = float2((vertexID << 1) & 2, vertexID & 2);
    vout.PosH = float4(vout.TexC * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    
    return vout;
}

// Reconstruct world position from depth
float3 ReconstructWorldPos(float2 texCoord, float depth)
{
    // Convert to NDC
    float4 ndcPos;
    ndcPos.x = texCoord.x * 2.0f - 1.0f;
    ndcPos.y = (1.0f - texCoord.y) * 2.0f - 1.0f;
    ndcPos.z = depth;
    ndcPos.w = 1.0f;
    
    // Transform to world space
    float4x4 invViewProj = mul(gProj, gView);
    invViewProj = transpose(invViewProj);
    
    float4 worldPos = mul(ndcPos, invViewProj);
    worldPos /= worldPos.w;
    
    return worldPos.xyz;
}

// Exponential Height Fog calculation
float ComputeExponentialHeightFog(float3 worldPos)
{
    float3 rayOrigin = gEyePosW;
    float3 rayDir = worldPos - rayOrigin;
    float rayLength = length(rayDir);
    
    if (rayLength < 0.001f)
        return 0.0f;
    
    rayDir /= rayLength;
    
    // Apply start distance
    float effectiveRayLength = max(0.0f, rayLength - gFogStartDistance);
    if (effectiveRayLength <= 0.0f)
        return 0.0f;
    
    // Clamp to cutoff distance
    effectiveRayLength = min(effectiveRayLength, gFogCutoffDistance);
    
    float rayDirectionY = rayDir.y;
    
    // Prevent division by zero
    float falloff = max(-127.0f, gFogHeightFalloff * rayDirectionY);
    
    // Line integral of exponential height fog
    float lineIntegral;
    if (abs(falloff) > 0.001f)
    {
        lineIntegral = (1.0f - exp(-falloff)) / falloff;
    }
    else
    {
        // Taylor expansion around 0
        lineIntegral = 1.0f - 0.5f * falloff;
    }
    
    // Density at ray origin considering height
    float rayOriginDensity = gFogDensity * exp(-gFogHeightFalloff * (rayOrigin.y - gFogHeight));
    
    // Final fog integral
    float fogAmount = rayOriginDensity * lineIntegral * effectiveRayLength;
    
    // Convert to opacity using exponential falloff
    float fogFactor = 1.0f - saturate(exp(-fogAmount));
    
    // Apply max opacity
    fogFactor = min(fogFactor, gFogMaxOpacity);
    
    return fogFactor;
}

float4 PS(VertexOut pin) : SV_Target
{
    // Sample scene color
    float3 sceneColor = gSceneTexture.Sample(gsamLinearClamp, pin.TexC).rgb;
    
    if (!gFogEnabled)
        return float4(sceneColor, 1.0f);
    
    // Sample depth
    float depth = gDepthTexture.Sample(gsamPointClamp, pin.TexC).r;
    
    // Skip sky (depth = 1.0)
    if (depth >= 0.9999f)
        return float4(sceneColor, 1.0f);
    
    // Reconstruct world position
    float3 worldPos = ReconstructWorldPos(pin.TexC, depth);
    
    // Compute fog
    float fogFactor = ComputeExponentialHeightFog(worldPos);
    
    // Compute inscattering color
    float3 viewDir = normalize(worldPos - gEyePosW);
    float sunDot = max(0.0f, dot(viewDir, gSunDirection));
    
    // Add some sun color to fog when looking towards sun
    float3 inscatteringColor = gFogInscatteringColor;
    inscatteringColor += float3(0.3f, 0.2f, 0.1f) * pow(sunDot, 8.0f) * gSunIntensity * 0.02f;
    
    // Blend scene with fog
    float3 finalColor = lerp(sceneColor, inscatteringColor, fogFactor);
    
    return float4(finalColor, 1.0f);
}
