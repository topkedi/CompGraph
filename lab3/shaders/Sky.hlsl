//=============================================================================
// Sky.hlsl - Sky rendering with atmospheric scattering
// Based on GPU Gems 2, Chapter 16: Accurate Atmospheric Scattering
//=============================================================================

#define PI 3.14159265359

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

struct VertexIn
{
    float3 PosL : POSITION;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosL : POSITION;
};

// Rayleigh phase function
float RayleighPhase(float cosTheta)
{
    return (3.0 / (16.0 * PI)) * (1.0 + cosTheta * cosTheta);
}

// Henyey-Greenstein phase function for Mie scattering
float MiePhase(float cosTheta, float g)
{
    float g2 = g * g;
    float num = (1.0 - g2);
    float denom = 4.0 * PI * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5);
    return num / max(denom, 0.0001);
}

// Compute atmospheric scattering
float3 ComputeAtmosphericScattering(float3 rayDir)
{
    float3 sunDir = normalize(gSunDirection);
    float cosTheta = dot(rayDir, sunDir);
    
    // View angle from horizon
    float viewY = rayDir.y;
    
    // Sun elevation: positive = day, negative = night
    float sunElevation = sunDir.y;
    
    // Night factor: 0 = full day, 1 = full night
    float nightFactor = saturate((-sunElevation - 0.05) * 5.0);
    float dayFactor = 1.0 - nightFactor;
    
    // Optical depth - increases as we look toward horizon
    float zenithAngle = acos(max(viewY, 0.001));
    float opticalDepthRayleigh = exp(-0.0) / max(cos(zenithAngle), 0.035);
    opticalDepthRayleigh = min(opticalDepthRayleigh, 40.0);
    
    // Mie optical depth (affected by density/pollution)
    float opticalDepthMie = opticalDepthRayleigh * gAtmosphereDensity * 0.1;
    
    // Rayleigh scattering coefficients at sea level
    float3 betaR = float3(5.8e-3, 13.5e-3, 33.1e-3);
    
    // Mie scattering coefficient (haze/pollution)
    float3 betaM = float3(21e-3, 21e-3, 21e-3) * gAtmosphereDensity;
    
    // Phase functions
    float phaseR = RayleighPhase(cosTheta);
    float phaseM = MiePhase(cosTheta, gMieAnisotropy);
    
    // Extinction
    float3 extinction = exp(-(betaR * opticalDepthRayleigh + betaM * opticalDepthMie));
    
    // Calculate sun optical depth based on elevation angle
    float sunZenithCos = max(sunElevation, 0.001);
    float sunOpticalDepth;
    
    if (sunElevation > 0.0)
    {
        sunOpticalDepth = 1.0 / (sunZenithCos + 0.15 * pow(max(93.885 - degrees(acos(sunZenithCos)), 0.1), -1.253));
    }
    else
    {
        sunOpticalDepth = 40.0;
    }
    sunOpticalDepth = min(sunOpticalDepth, 40.0);
    
    // Sun transmittance
    float3 sunTransmittance = exp(-(betaR * sunOpticalDepth * 1.5 + betaM * sunOpticalDepth * 0.15));
    sunTransmittance *= dayFactor;
    
    // Sunset factor for color tinting
    float sunsetFactor = saturate(1.0 - sunElevation * 3.0) * dayFactor;
    sunsetFactor = sunsetFactor * sunsetFactor;
    
    // Boost red/orange at sunset
    float3 sunsetBoost = float3(1.5, 1.1, 0.7);
    float3 tintedTransmittance = sunTransmittance * lerp(float3(1.0, 1.0, 1.0), sunsetBoost, sunsetFactor);
    
    // In-scattering
    float3 rayleighScatter = betaR * phaseR * (1.0 - extinction);
    float3 mieScatter = betaM * phaseM * (1.0 - exp(-opticalDepthMie));
    
    float3 sunColor = tintedTransmittance * gSunIntensity;
    float3 inscatter = (rayleighScatter + mieScatter) * sunColor;
    
    // Warm horizon glow during sunset
    float horizonGlow = exp(-abs(viewY) * 3.0) * sunsetFactor * dayFactor;
    float3 warmGlow = float3(1.0, 0.4, 0.1) * horizonGlow * gSunIntensity * 0.15;
    inscatter += warmGlow * tintedTransmittance;
    
    // Twilight glow at horizon
    float twilightFactor = saturate(1.0 - abs(sunElevation + 0.1) * 10.0) * saturate(-sunElevation * 10.0);
    float twilightHorizon = exp(-abs(viewY) * 2.0) * twilightFactor;
    float3 twilightColor = float3(0.3, 0.15, 0.1) * twilightHorizon * gSunIntensity * 0.1;
    inscatter += twilightColor;
    
    // Night sky color
    float3 nightSky = float3(0.005, 0.007, 0.015);
    float nightHorizonBrightness = exp(-abs(viewY) * 2.0) * 0.5 + 0.5;
    nightSky *= nightHorizonBrightness;
    
    // Daytime ambient sky
    float ambientScale = lerp(0.1, 0.03, sunsetFactor);
    float3 daySky = float3(0.05, 0.1, 0.2) * (1.0 - extinction) * gSunIntensity * ambientScale;
    
    // Blend between day and night sky
    float3 ambientSky = lerp(daySky, nightSky, nightFactor);
    
    float3 skyColor = inscatter + ambientSky;
    
    // Ground color when looking down
    if (viewY < 0.0)
    {
        float groundFade = saturate(-viewY * 3.0);
        float3 dayGround = float3(0.4, 0.35, 0.3) * tintedTransmittance * gSunIntensity * 0.05;
        float3 nightGround = float3(0.01, 0.01, 0.015);
        float3 groundColor = lerp(dayGround, nightGround, nightFactor);
        skyColor = lerp(skyColor, groundColor, groundFade);
    }
    
    return skyColor;
}

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    
    vout.PosL = vin.PosL;
    
    // Center sky sphere on camera
    float4 posW = float4(vin.PosL + gEyePosW, 1.0f);
    
    // Set z = w so that z/w = 1 (skydome always on far plane)
    vout.PosH = mul(posW, gViewProj).xyww;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float3 rayDir = normalize(pin.PosL);
    
    // Compute atmospheric scattering
    float3 color = ComputeAtmosphericScattering(rayDir);
    
    // Add sun disk
    float3 sunDir = normalize(gSunDirection);
    float sunDot = dot(rayDir, sunDir);
    float sunElevation = sunDir.y;
    
    // Night factor
    float nightFactor = saturate((-sunElevation - 0.05) * 5.0);
    float dayFactor = 1.0 - nightFactor;
    
    // Only show sun when above horizon
    if (sunElevation > -0.05)
    {
        // Sunset factor for color tinting
        float sunsetFactor = saturate(1.0 - sunElevation * 3.0) * dayFactor;
        sunsetFactor = sunsetFactor * sunsetFactor;
        
        // Sun disk color
        float3 sunDiskColor = lerp(float3(1.0, 0.98, 0.95), float3(1.0, 0.5, 0.2), sunsetFactor);
        
        // Sun disk
        float sunDisk = smoothstep(0.9997, 0.9999, sunDot);
        float3 sunColor = sunDiskColor * gSunIntensity * 0.15 * sunDisk * dayFactor;
        
        // Sun corona/glow
        float3 glowTint = lerp(float3(1.0, 0.9, 0.7), float3(1.0, 0.6, 0.3), sunsetFactor);
        float sunGlow = pow(max(sunDot, 0.0), 512.0) * gSunIntensity * 0.3;
        sunGlow += pow(max(sunDot, 0.0), 64.0) * gSunIntensity * 0.05;
        float3 glowColor = glowTint * sunGlow * dayFactor;
        
        color += sunColor + glowColor;
    }
    
    // Tone mapping
    color = color / (color + 1.0);
    
    // Exposure adjustment
    color = 1.0 - exp(-gExposure * color * 2.0);
    
    // Gamma correction
    color = pow(max(color, 0.0), 1.0 / 2.2);
    
    return float4(color, 1.0);
}
