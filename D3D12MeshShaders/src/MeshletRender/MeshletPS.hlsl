//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

struct Constants
{
    float4x4 World;
    float4x4 WorldView;
    float4x4 WorldViewProj;
    uint     DrawMeshlets;
};

struct VertexOut
{
    float4 PositionHS   : SV_Position;
    float3 PositionVS   : POSITION0;
    float3 PositionWS   : POSITION1;
    float3 Normal       : NORMAL0;
    float2 TexCoord     : TEXCOORD0;
    uint   MeshletIndex : COLOR0;
};

ConstantBuffer<Constants> Globals : register(b0);
Texture2D<float4> DiffuseTexture : register(t4);
Texture2D<float4> NormalTexture : register(t5);
Texture2D<float4> AOTexture : register(t6);
SamplerState LinearSampler : register(s0);

float4 main(VertexOut input) : SV_TARGET
{
    float3 lightDir = -normalize(float3(1, -1, 1));
    float3 diffuse;
    float shininess;
    
    if (Globals.DrawMeshlets)
    {
        uint idx = input.MeshletIndex;
        diffuse = float3(float(idx & 1), float(idx & 3) / 4, float(idx & 7) / 8);
        shininess = 16.0;
    }
    else
    {
        diffuse = DiffuseTexture.Sample(LinearSampler, input.TexCoord).rgb;
        diffuse *= AOTexture.Sample(LinearSampler, input.TexCoord).r;
        shininess = 64.0;
    }

    float3 normal = normalize(input.Normal);
    float3 viewDir = -normalize(input.PositionVS);
    float3 halfVec = normalize(lightDir + viewDir);

    float diffuseTerm = saturate(dot(normal, lightDir));
    float specTerm = pow(saturate(dot(normal, halfVec)), shininess);
    specTerm = diffuseTerm > 0.0 ? specTerm : 0.0;

    float3 color = (diffuseTerm + specTerm + 0.1) * diffuse;
    return float4(color, 1);
}
