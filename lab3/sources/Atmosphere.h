#pragma once

#include "d3dUtil.h"

// Atmospheric Scattering implementation based on:
// - GPU Gems 2, Chapter 16: Accurate Atmospheric Scattering
// - Sean O'Neil's atmospheric scattering model

class Atmosphere
{
public:
    Atmosphere() = default;
    ~Atmosphere() = default;

    // Atmosphere parameters
    struct Parameters
    {
        // Sun parameters
        DirectX::XMFLOAT3 SunDirection = { 0.0f, 0.707f, 0.707f };
        float SunIntensity = 20.0f;

        // Rayleigh scattering coefficients (wavelength-dependent)
        DirectX::XMFLOAT3 RayleighCoefficients = { 5.8e-6f, 13.5e-6f, 33.1e-6f };
        float RayleighScaleHeight = 8500.0f;

        // Mie scattering coefficients (wavelength-independent)
        DirectX::XMFLOAT3 MieCoefficients = { 21e-6f, 21e-6f, 21e-6f };
        float MieScaleHeight = 1200.0f;
        float MieAnisotropy = 0.76f; // g parameter for Henyey-Greenstein phase function

        // Planet parameters
        float PlanetRadius = 6371.0f;     // km
        float AtmosphereHeight = 100.0f;  // km above planet surface

        // Density multiplier: 1.0 = clean atmosphere, >1.0 = dirty/polluted
        float DensityMultiplier = 1.0f;

        // Rendering parameters
        float Exposure = 1.5f;
        int NumViewSamples = 16;
        int NumLightSamples = 8;
    };

    Parameters& GetParameters() { return mParams; }
    const Parameters& GetParameters() const { return mParams; }

    // Preset atmosphere configurations
    void SetCleanAtmosphere();
    void SetDirtyAtmosphere();
    void SetMarsAtmosphere();
    void SetSunsetAtmosphere();

private:
    Parameters mParams;
};
