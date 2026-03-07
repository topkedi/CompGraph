#include "Common.h"

HullIn VS(VertexIn vin)
{
  HullIn vout;

  vout.positionWorld = vin.positionWorld.xyz;
  vout.texCoord = vin.texCoord;
  vout.localTexCoord = vin.localTexCoord;

  return vout;
}
