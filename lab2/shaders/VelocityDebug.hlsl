//***************************************************************************************
// VelocityDebug.hlsl - Визуализация velocity buffer с красным оверлеем
// 
// Показывает обычную сцену с красным наложением на движущихся пикселях
// Интенсивность красного зависит от величины velocity
//***************************************************************************************

Texture2D gVelocityBuffer : register(t0);  // Motion vector buffer
Texture2D gSceneTexture : register(t1);    // Текущая сцена
SamplerState gsamPointClamp : register(s0);

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

// Fullscreen triangle vertex shader
VertexOut VS(uint vid : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = float2((vid << 1) & 2, vid & 2);
    vout.PosH = float4(vout.TexC * float2(2, -2) + float2(-1, 1), 0, 1);
    return vout;
}

// Pixel shader - показывает сцену с красным оверлеем на движущихся пикселях
float4 PS(VertexOut pin) : SV_Target
{
    // Сэмплируем velocity
    float2 velocity = gVelocityBuffer.Sample(gsamPointClamp, pin.TexC).rg;
    
    // Сэмплируем цвет сцены
    float3 sceneColor = gSceneTexture.Sample(gsamPointClamp, pin.TexC).rgb;
    
    // DEBUG: Проверяем что scene texture работает
    // Если видишь нормальную сцену - scene texture работает
    // Если видишь черный экран - scene texture не передается
    
    // Вычисляем величину velocity
    float velocityMag = length(velocity);
    
    // Порог для фильтрации TAA jitter
    float threshold = 0.001f;  // Низкий порог для теста
    
    // Если velocity выше порога, добавляем красный оверлей
    if (velocityMag > threshold)
    {
        // Масштабируем величину velocity для визуализации
        float intensity = saturate(velocityMag * 100.0f);
        
        // Смешиваем цвет сцены с красным
        float3 redOverlay = float3(1.0f, 0.0f, 0.0f);
        sceneColor = lerp(sceneColor, redOverlay, intensity * 0.5f);
    }
    
    return float4(sceneColor, 1.0f);
}
