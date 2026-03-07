//***************************************************************************************
// TemporalAA.cpp
//***************************************************************************************

#include "TemporalAA.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

TemporalAA::TemporalAA(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
    md3dDevice = device;
    mWidth = width;
    mHeight = height;
    mFormat = format;

    mViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    mScissorRect = { 0, 0, (int)width, (int)height };

    BuildResource();
}

UINT TemporalAA::Width() const
{
    return mWidth;
}

UINT TemporalAA::Height() const
{
    return mHeight;
}

ID3D12Resource* TemporalAA::Resource()
{
    return mTAAOutput.Get();
}

ID3D12Resource* TemporalAA::HistoryResource()
{
    return mHistoryBuffer.Get();
}

CD3DX12_GPU_DESCRIPTOR_HANDLE TemporalAA::Srv() const
{
    return mhGpuSrv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE TemporalAA::Rtv() const
{
    return mhCpuRtv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE TemporalAA::HistorySrv() const
{
    return mhHistoryGpuSrv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE TemporalAA::HistoryRtv() const
{
    return mhHistoryCpuRtv;
}

D3D12_VIEWPORT TemporalAA::Viewport() const
{
    return mViewport;
}

D3D12_RECT TemporalAA::ScissorRect() const
{
    return mScissorRect;
}

void TemporalAA::BuildDescriptors(
    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
    CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv,
    UINT srvDescriptorSize,
    UINT rtvDescriptorSize)
{
    mhCpuSrv = hCpuSrv;
    mhGpuSrv = hGpuSrv;
    mhCpuRtv = hCpuRtv;
    
    mhHistoryCpuSrv = hCpuSrv.Offset(1, srvDescriptorSize);
    mhHistoryGpuSrv = hGpuSrv.Offset(1, srvDescriptorSize);
    mhHistoryCpuRtv = hCpuRtv.Offset(1, rtvDescriptorSize);

    BuildDescriptors();
}

void TemporalAA::OnResize(UINT newWidth, UINT newHeight)
{
    if ((mWidth != newWidth) || (mHeight != newHeight))
    {
        mWidth = newWidth;
        mHeight = newHeight;

        mViewport = { 0.0f, 0.0f, (float)mWidth, (float)mHeight, 0.0f, 1.0f };
        mScissorRect = { 0, 0, (int)mWidth, (int)mHeight };

        BuildResource();
    }
}

XMFLOAT2 TemporalAA::GetJitter(int frameIndex)
{
    // Halton sequence (2,3) for 8-sample pattern
    static const XMFLOAT2 haltonSequence[8] = {
        XMFLOAT2(0.5f, 0.333333f),
        XMFLOAT2(0.25f, 0.666667f),
        XMFLOAT2(0.75f, 0.111111f),
        XMFLOAT2(0.125f, 0.444444f),
        XMFLOAT2(0.625f, 0.777778f),
        XMFLOAT2(0.375f, 0.222222f),
        XMFLOAT2(0.875f, 0.555556f),
        XMFLOAT2(0.0625f, 0.888889f)
    };
    
    int index = frameIndex % 8;
    
    // Normal scale for production use
    float scale = 1.0f;
    return XMFLOAT2(
        (haltonSequence[index].x - 0.5f) * scale,
        (haltonSequence[index].y - 0.5f) * scale
    );
}

void TemporalAA::BuildDescriptors()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = mFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = mFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;

    md3dDevice->CreateShaderResourceView(mTAAOutput.Get(), &srvDesc, mhCpuSrv);
    md3dDevice->CreateRenderTargetView(mTAAOutput.Get(), &rtvDesc, mhCpuRtv);
    
    md3dDevice->CreateShaderResourceView(mHistoryBuffer.Get(), &srvDesc, mhHistoryCpuSrv);
    md3dDevice->CreateRenderTargetView(mHistoryBuffer.Get(), &rtvDesc, mhHistoryCpuRtv);
}

void TemporalAA::BuildResource()
{
    D3D12_RESOURCE_DESC texDesc;
    ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mWidth;
    texDesc.Height = mHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = mFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    float clearColor[] = { 0.85f, 0.92f, 0.98f, 1.0f };
    CD3DX12_CLEAR_VALUE optClear(mFormat, clearColor);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &optClear,
        IID_PPV_ARGS(&mTAAOutput)));

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &optClear,
        IID_PPV_ARGS(&mHistoryBuffer)));
}
