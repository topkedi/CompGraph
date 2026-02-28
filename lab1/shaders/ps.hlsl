#include "Common.h"

Texture2D colorTexture : register(t1);  // Color texture at t1 (heightmap is at t0)
SamplerState samplerState : register(s0);

float4 PS(PixelIn input) : SV_TARGET
{
  // Sample terrain color from texture
  float4 baseColor = colorTexture.Sample(samplerState, input.localTexCoord);
  
  // Make terrain light brown color
  // Mix with warm brown tone
  float3 brownTint = float3(0.85f, 0.75f, 0.60f);
  baseColor.rgb = lerp(baseColor.rgb, brownTint, 0.5f);
  baseColor.rgb *= 1.15f; // Brighten
  
  // Calculate lighting using surface normal
  float3 surfaceNormal = normalize(input.normal);
  
  // Light direction (sun position)
  float3 sunDirection = normalize(float3(0.3f, -1.0f, 0.3f));
  
  // Calculate diffuse component
  float diffuseStrength = saturate(dot(surfaceNormal, -sunDirection));
  
  // Ambient lighting component
  float ambientStrength = 0.45f;
  
  // Final lighting calculation
  float totalLight = ambientStrength + diffuseStrength * 0.55f;
  
  // Apply lighting to terrain color
  float4 outputColor = baseColor * totalLight;
  outputColor.a = 1.0f;
  
  return outputColor;
}
