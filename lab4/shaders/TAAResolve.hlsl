//***************************************************************************************
// TAAResolve.hlsl - Temporal Anti-Aliasing resolve pass
//***************************************************************************************

cbuffer cbTAA : register(b0)
{
    float2 gJitterOffset;
    float2 gScreenSize;
    float gBlendFactor;
    float3 gPadding;
};

Texture2D gCurrentFrame  : register(t0);
Texture2D gHistoryFrame  : register(t1);

SamplerState gsamPointClamp  : register(s1);
SamplerState gsamLinearClamp : register(s3);

struct VertexOut
{
    float4 PosH  : SV_POSITION;
    float2 TexC  : TEXCOORD;
};

// YCoCg color space for better clamping
float3 RGBToYCoCg(float3 rgb)
{
    float Y = 0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b;
    float Co = 0.5 * rgb.r - 0.5 * rgb.b;
    float Cg = -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b;
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    float Y = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    
    float r = Y + Co - Cg;
    float g = Y + Cg;
    float b = Y - Co - Cg;
    
    return float3(r, g, b);
}

VertexOut VS(uint vid : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = float2((vid << 1) & 2, vid & 2);
    vout.PosH = float4(vout.TexC * float2(2, -2) + float2(-1, 1), 0, 1);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    float2 texelSize = 1.0 / gScreenSize;
    
    // Sample current frame
    float3 currentRGB = gCurrentFrame.SampleLevel(gsamPointClamp, uv, 0).rgb;
    
    // Sample history with bilinear filtering
    float3 historyRGB = gHistoryFrame.SampleLevel(gsamLinearClamp, uv, 0).rgb;
    
    // Gather 3x3 neighborhood in YCoCg space
    float3 minColor = float3(9999, 9999, 9999);
    float3 maxColor = float3(-9999, -9999, -9999);
    float3 m1 = float3(0, 0, 0);
    float3 m2 = float3(0, 0, 0);
    
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUV = uv + float2(x, y) * texelSize;
            float3 sampleRGB = gCurrentFrame.SampleLevel(gsamPointClamp, sampleUV, 0).rgb;
            float3 sampleYCoCg = RGBToYCoCg(sampleRGB);
            
            minColor = min(minColor, sampleYCoCg);
            maxColor = max(maxColor, sampleYCoCg);
            m1 += sampleYCoCg;
            m2 += sampleYCoCg * sampleYCoCg;
        }
    }
    
    // Compute variance
    m1 /= 9.0;
    m2 /= 9.0;
    float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    
    // Variance clipping bounds
    float gamma = 1.0;
    float3 aabbMin = max(minColor, m1 - gamma * sigma);
    float3 aabbMax = min(maxColor, m1 + gamma * sigma);
    
    // Clamp history in YCoCg space
    float3 historyYCoCg = RGBToYCoCg(historyRGB);
    float3 clampedHistoryYCoCg = clamp(historyYCoCg, aabbMin, aabbMax);
    float3 clampedHistoryRGB = YCoCgToRGB(clampedHistoryYCoCg);
    
    // Blend
    float3 finalColor = lerp(clampedHistoryRGB, currentRGB, gBlendFactor);
    
    return float4(max(finalColor, 0.0), 1.0);
}
