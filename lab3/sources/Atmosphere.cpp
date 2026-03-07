#include "Atmosphere.h"

void Atmosphere::SetCleanAtmosphere()
{
    mParams.DensityMultiplier = 1.0f;
    mParams.MieAnisotropy = 0.76f;
    mParams.SunIntensity = 20.0f;
    mParams.Exposure = 1.5f;
}

void Atmosphere::SetDirtyAtmosphere()
{
    // Increased density for hazy/polluted atmosphere
    mParams.DensityMultiplier = 3.0f;
    mParams.MieAnisotropy = 0.6f; // More isotropic scattering
    mParams.SunIntensity = 18.0f;
    mParams.Exposure = 1.2f;
}

void Atmosphere::SetMarsAtmosphere()
{
    // Mars-like thin, dusty atmosphere with red tint
    mParams.DensityMultiplier = 0.3f;
    mParams.MieAnisotropy = 0.8f;
    mParams.SunIntensity = 15.0f;
    mParams.Exposure = 2.0f;
}

void Atmosphere::SetSunsetAtmosphere()
{
    // Enhanced scattering for sunset effect
    mParams.DensityMultiplier = 2.0f;
    mParams.MieAnisotropy = 0.85f;
    mParams.SunIntensity = 25.0f;
    mParams.Exposure = 1.8f;
}
