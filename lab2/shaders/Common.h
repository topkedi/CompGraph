#pragma once

struct VertexIn
{
  float3 positionWorld : POSITION;
  float2 texCoord : TEXCOORD0;      // Global coords for heightmap
  float2 localTexCoord : TEXCOORD1; // Local coords for color texture
};

struct ObjectVertexIn
{
  float3 positionLocal : POSITION;  // Локальная позиция для объектов
  float2 texCoord : TEXCOORD0;
  float2 localTexCoord : TEXCOORD1;
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
  float4 currentPos : TEXCOORD2;    // Текущая позиция в NDC
  float4 prevPos : TEXCOORD3;       // Предыдущая позиция в NDC
};

// Структура для Multiple Render Targets (Color + Motion Vectors)
struct PixelOut
{
  float4 color : SV_Target0;        // Цвет
  float2 motion : SV_Target1;       // Motion vectors (RG16F)
};

struct PatchTess
{
  float edgeTess[4] : SV_TessFactor;
  float insideTess[2] : SV_InsideTessFactor;
};
