//***************************************************************************************
// MotionVectorVisualize.hlsl - Визуализация velocity texture для отладки
// 
// Velocity Texture (Motion Vector Buffer):
// - Формат: DXGI_FORMAT_R16G16_FLOAT (RG16F)
// - R канал: X компонент velocity (горизонтальное движение в UV пространстве)
// - G канал: Y компонент velocity (вертикальное движение в UV пространстве)
// - Значения: обычно в диапазоне [-0.5, 0.5] для субпиксельных смещений
// 
// В RenderDoc эта текстура будет называться "Velocity Texture"
//***************************************************************************************

Texture2D gMotionVectors : register(t0);
SamplerState gsamPointClamp : register(s0);

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

VertexOut VS(uint vid : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = float2((vid << 1) & 2, vid & 2);
    vout.PosH = float4(vout.TexC * float2(2, -2) + float2(-1, 1), 0, 1);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float2 motionVector = gMotionVectors.Sample(gsamPointClamp, pin.TexC).rg;
    
    // Визуализируем motion vectors как цвет для RenderDoc
    float3 color = float3(0.5, 0.5, 0.5); // Серый фон для нулевых векторов
    
    // Масштабируем для лучшей видимости (velocity обычно очень маленькие значения)
    float scale = 20.0;
    motionVector *= scale;
    
    // Кодируем velocity в цвет:
    // R канал: X компонент motion vector (красный = движение вправо)
    // G канал: -X компонент motion vector (зеленый = движение влево) 
    // B канал: Y компонент motion vector (синий = движение по Y)
    
    color.r = 0.5 + saturate(motionVector.x);   // [0.5, 1.0] для положительного X
    color.g = 0.5 + saturate(-motionVector.x);  // [0.5, 1.0] для отрицательного X
    color.b = 0.5 + saturate(abs(motionVector.y)); // [0.5, 1.0] для любого Y
    
    // Показываем величину motion vector как общую яркость
    float magnitude = length(motionVector);
    if (magnitude > 0.01) // Только если есть заметное движение
    {
        color = lerp(float3(0.2, 0.2, 0.2), color, saturate(magnitude * 2.0));
    }
    
    return float4(color, 1.0);
}