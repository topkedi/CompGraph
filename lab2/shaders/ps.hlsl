#include "Common.h"

Texture2D colorTexture : register(t1);  // Color texture at t1 (heightmap is at t0)
SamplerState samplerState : register(s0);

PixelOut PS(PixelIn input)
{
  PixelOut output;
  
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
  output.color = baseColor * totalLight;
  output.color.a = 1.0f;
  
  // Вычисление motion vectors
  // Преобразование из homogeneous coordinates в NDC
  float2 currentNDC = input.currentPos.xy / input.currentPos.w;
  float2 prevNDC = input.prevPos.xy / input.prevPos.w;
  
  // Motion vector в NDC пространстве
  float2 motionNDC = currentNDC - prevNDC;
  
  // Преобразование из NDC [-1,1] в UV [0,1] пространство
  // NDC: X[-1,1], Y[-1,1] -> UV: X[0,1], Y[0,1]
  // Учитываем, что Y инвертирован в UV пространстве
  output.motion.x = motionNDC.x * 0.5;      // X: [-1,1] -> [-0.5,0.5]
  output.motion.y = -motionNDC.y * 0.5;     // Y: [-1,1] -> [0.5,-0.5] (инвертируем)
  
  return output;
}
