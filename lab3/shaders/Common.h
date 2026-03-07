#pragma once

struct VertexIn
{
  float3 positionWorld : POSITION;
  float2 texCoord : TEXCOORD0;      // Global coords for heightmap
  float2 localTexCoord : TEXCOORD1; // Local coords for color texture
};


struct HullIn
{
  float3 positionWorld : POSITION;
  float2 texCoord : TEXCOORD0;
  float2 localTexCoord : TEXCOORD1;
};

struct DomainIn
{
  float3 positionWorld : POSITION;
  float2 texCoord : TEXCOORD0;
  float2 localTexCoord : TEXCOORD1;
};

struct PixelIn
{
  float4 positionProjection : SV_POSITION;
  float3 positionWorld : POSITION;
  float2 texCoord : TEXCOORD0;      // Global coords (for heightmap)
  float2 localTexCoord : TEXCOORD1; // Local coords (for color texture)
  float3 normal : NORMAL;
};

struct PatchTess
{
  float edgeTess[4] : SV_TessFactor;
  float insideTess[2] : SV_InsideTessFactor;
};
