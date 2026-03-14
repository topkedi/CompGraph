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

Texture2D gAlbedoX : register(t0);
Texture2D gAlbedoY : register(t1);
Texture2D gAlbedoZ : register(t2);

SamplerState gsamLinearWrap : register(s0);
SamplerState gsamAnisoWrap : register(s1);

struct PSIn
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
};

float4 PS(PSIn pin) : SV_Target
{
    float3 normalW = normalize(pin.NormalW);

    float2 uvX = pin.PosW.zy * gTexScale;
    float2 uvY = pin.PosW.xz * gTexScale;
    float2 uvZ = pin.PosW.xy * gTexScale;

    float3 sampleX = gAlbedoX.Sample(gsamAnisoWrap, uvX).rgb;
    float3 sampleY = gAlbedoY.Sample(gsamAnisoWrap, uvY).rgb;
    float3 sampleZ = gAlbedoZ.Sample(gsamAnisoWrap, uvZ).rgb;

    float3 weights = abs(normalW);
    float weightSum = max(weights.x + weights.y + weights.z, 1e-5);
    weights /= weightSum;

    float3 triplanarColor = sampleX * weights.x + sampleY * weights.y + sampleZ * weights.z;

    float slope = 1.0f - saturate(abs(normalW.y));
    float rockBlend = smoothstep(0.35f, 0.85f, slope);
    float3 rockColor = 0.5f * (sampleX + sampleZ);

    float3 albedo = lerp(triplanarColor, rockColor, rockBlend * 0.7f);

    float3 lightDir = normalize(-gLightDir);
    float diffuse = saturate(dot(normalW, lightDir));
    float lighting = gAmbient + diffuse * 0.78f;

    float3 color = albedo * lighting;

    return float4(color, 1.0f);
}
