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
  float gPadding2;
};

static float minDist = 300;
static float maxDist = 1500;
static float minTess = 1;
static float maxTess = 6;

float ComputeTessellation(float3 pos)
{
  float d = distance(pos, gEyePosW);
  float t = saturate((d - minDist) / (maxDist - minDist));
  float tessLevel = pow(2, lerp(maxTess, minTess, t));
  return max(1.0f, tessLevel);
}

PatchTess ConstantHS(InputPatch<HullIn, 4> patch, uint patchID : SV_PrimitiveID)
{
  PatchTess pt;

  float3 edge0 = 0.5f * (patch[0].positionWorld + patch[2].positionWorld);
  float3 edge1 = 0.5f * (patch[0].positionWorld + patch[1].positionWorld);
  float3 edge2 = 0.5f * (patch[1].positionWorld + patch[3].positionWorld);
  float3 edge3 = 0.5f * (patch[2].positionWorld + patch[3].positionWorld);
  float3 center = 0.25f * (patch[0].positionWorld + patch[1].positionWorld + 
                           patch[2].positionWorld + patch[3].positionWorld);

  pt.edgeTess[0] = ComputeTessellation(edge0);
  pt.edgeTess[1] = ComputeTessellation(edge1);
  pt.edgeTess[2] = ComputeTessellation(edge2);
  pt.edgeTess[3] = ComputeTessellation(edge3);

  pt.insideTess[0] = ComputeTessellation(center);
  pt.insideTess[1] = pt.insideTess[0];

  return pt;
}

[domain("quad")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(64.0f)]
DomainIn HS(InputPatch<HullIn, 4> p, uint i : SV_OutputControlPointID, uint patchId : SV_PrimitiveID)
{
  DomainIn dout;

  dout.positionWorld = p[i].positionWorld;
  dout.texCoord = p[i].texCoord;
  dout.localTexCoord = p[i].localTexCoord;

  return dout;
}
