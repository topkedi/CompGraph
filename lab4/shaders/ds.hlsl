#include "Common.h"

cbuffer PassConstants : register(b0)
{
  matrix gView;
  matrix gProj;
  matrix gViewProj;
  float3 gEyePosW;
  float gPadding1;
  float2 gRenderTargetSize;
  float gNearZ;
  float gFarZ;
  float gTotalTime;
  float gDeltaTime;
  float gHeightScale;
  float gNoiseSeed;
};

Texture2D heightMap : register(t0);
SamplerState heightSampler : register(s0);

[domain("quad")]
PixelIn DS(PatchTess patchTess, float2 uv : SV_DomainLocation, const OutputPatch<DomainIn, 4> quad)
{
  PixelIn pout;

  pout.positionWorld = lerp(
    lerp(quad[0].positionWorld, quad[1].positionWorld, uv.x),
    lerp(quad[2].positionWorld, quad[3].positionWorld, uv.x),
    uv.y);

  pout.texCoord = lerp(
    lerp(quad[0].texCoord, quad[1].texCoord, uv.x),
    lerp(quad[2].texCoord, quad[3].texCoord, uv.x),
    uv.y);

  pout.localTexCoord = lerp(
    lerp(quad[0].localTexCoord, quad[1].localTexCoord, uv.x),
    lerp(quad[2].localTexCoord, quad[3].localTexCoord, uv.x),
    uv.y);

  float heightValue = heightMap.SampleLevel(heightSampler, pout.texCoord, 0).r;
  pout.positionWorld.y = heightValue * gHeightScale;

  float texelSize = 1.0f / 2048.0f;
  
  float hL = heightMap.SampleLevel(heightSampler, pout.texCoord + float2(-texelSize, 0), 0).r * gHeightScale;
  float hR = heightMap.SampleLevel(heightSampler, pout.texCoord + float2(texelSize, 0), 0).r * gHeightScale;
  float hD = heightMap.SampleLevel(heightSampler, pout.texCoord + float2(0, texelSize), 0).r * gHeightScale;
  float hU = heightMap.SampleLevel(heightSampler, pout.texCoord + float2(0, -texelSize), 0).r * gHeightScale;
  
  float3 tangent = normalize(float3(2.0f * texelSize * 2048.0f, hR - hL, 0));
  float3 bitangent = normalize(float3(0, hU - hD, 2.0f * texelSize * 2048.0f));
  pout.normal = normalize(cross(bitangent, tangent));

  pout.positionProjection = mul(float4(pout.positionWorld, 1.0f), gView);
  pout.positionProjection = mul(pout.positionProjection, gProj);

  return pout;
}
