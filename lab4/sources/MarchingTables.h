#pragma once

#include <DirectXMath.h>

struct MarchingTablesConstants
{
    DirectX::XMINT4 EdgeTable[64];
    DirectX::XMINT4 TriTable[256][4];
};

MarchingTablesConstants BuildMarchingTablesConstants();
