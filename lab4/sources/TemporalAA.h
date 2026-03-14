//***************************************************************************************
// TemporalAA.h - Temporal Anti-Aliasing implementation
//***************************************************************************************

#pragma once

#include "d3dUtil.h"

class TemporalAA
{
public:
    TemporalAA(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format);
    
    TemporalAA(const TemporalAA& rhs) = delete;
    TemporalAA& operator=(const TemporalAA& rhs) = delete;
    ~TemporalAA() = default;

    UINT Width() const;
    UINT Height() const;
    ID3D12Resource* Resource();
    ID3D12Resource* HistoryResource();
    
    CD3DX12_GPU_DESCRIPTOR_HANDLE Srv() const;
    CD3DX12_CPU_DESCRIPTOR_HANDLE Rtv() const;
    
    CD3DX12_GPU_DESCRIPTOR_HANDLE HistorySrv() const;
    CD3DX12_CPU_DESCRIPTOR_HANDLE HistoryRtv() const;

    D3D12_VIEWPORT Viewport() const;
    D3D12_RECT ScissorRect() const;

    void BuildDescriptors(
        CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
        CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
        CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv,
        UINT srvDescriptorSize,
        UINT rtvDescriptorSize);

    void OnResize(UINT newWidth, UINT newHeight);
    
    static DirectX::XMFLOAT2 GetJitter(int frameIndex);

private:
    void BuildDescriptors();
    void BuildResource();

private:
    ID3D12Device* md3dDevice = nullptr;

    D3D12_VIEWPORT mViewport;
    D3D12_RECT mScissorRect;

    UINT mWidth = 0;
    UINT mHeight = 0;
    DXGI_FORMAT mFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhGpuSrv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuRtv;
    
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhHistoryCpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhHistoryGpuSrv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhHistoryCpuRtv;

    Microsoft::WRL::ComPtr<ID3D12Resource> mTAAOutput = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mHistoryBuffer = nullptr;
};
