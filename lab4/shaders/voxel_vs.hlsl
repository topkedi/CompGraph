cbuffer PassConstants : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;

    float3 gEyePosW;
    float gIsoLevel;

    float gTexScale;
    float gAmbient;
    float2 gPadding0;

    float3 gLightDir;
    float gPadding1;
};

struct VSIn
{
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
};

struct PSIn
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
};

PSIn VS(VSIn vin)
{
    PSIn vout;
    vout.PosW = vin.PosW;
    vout.NormalW = normalize(vin.NormalW);
    float4 viewPos = mul(float4(vin.PosW, 1.0f), gView);
    vout.PosH = mul(viewPos, gProj);
    return vout;
}
