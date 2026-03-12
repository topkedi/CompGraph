#include "Common.h"

cbuffer PassConstants : register(b0)
{
  matrix gView;
  matrix gProj;
  matrix gViewProj;
  matrix gPrevViewProj;
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

cbuffer ObjectConstants : register(b1)
{
  matrix gWorld;
  matrix gPrevWorld;
};

PixelIn VS(ObjectVertexIn vin)
{
  PixelIn vout;
  
  // Преобразуем локальную позицию в мировую
  float4 posW = mul(float4(vin.positionLocal, 1.0f), gWorld);
  vout.positionWorld = posW.xyz;
  
  // Преобразуем в проекцию
  vout.positionProjection = mul(posW, gViewProj);
  
  // Для motion vectors: текущая и предыдущая позиции
  vout.currentPos = vout.positionProjection;
  float4 prevPosW = mul(float4(vin.positionLocal, 1.0f), gPrevWorld);
  vout.prevPos = mul(prevPosW, gPrevViewProj);
  
  // Передаем текстурные координаты
  vout.texCoord = vin.texCoord;
  vout.localTexCoord = vin.localTexCoord;
  
  // Простая нормаль (для куба можно вычислить точнее, но для демо подойдет)
  vout.normal = float3(0, 1, 0);
  
  return vout;
}