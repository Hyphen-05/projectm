#include "IdlePreset.hpp"

#include "MilkdropPreset.hpp"

#include <sstream>
#include <string>

namespace libprojectM {
namespace MilkdropPreset {

// The idle preset draws nothing: black, with no logo, waves, borders or motion. It shows while an engine has no
// preset of its own, and an app that shows its own presets has nothing to say in that moment.
std::string IdlePresets::presetText()
{    return R"([preset00]
fRating=0.000000
fGammaAdj=1.000000
fDecay=0.000000
fVideoEchoZoom=1.000000
fVideoEchoAlpha=0.000000
nVideoEchoOrientation=0
nWaveMode=0
bAdditiveWaves=0
bWaveDots=0
bWaveThick=0
bModWaveAlphaByVolume=0
bMaximizeWaveColor=0
bTexWrap=0
bDarkenCenter=0
bRedBlueStereo=0
bBrighten=0
bDarken=0
bSolarize=0
bInvert=0
fWaveAlpha=0.000000
fWaveScale=0.000000
fShader=0.000000
zoom=1.000000
rot=0.000000
warp=0.000000
sx=1.000000
sy=1.000000
wave_r=0.000000
wave_g=0.000000
wave_b=0.000000
ob_size=0.000000
ob_a=0.000000
ib_size=0.000000
ib_a=0.000000
nMotionVectorsX=0.000000
nMotionVectorsY=0.000000
mv_a=0.000000
)";
}

std::unique_ptr<Preset>
IdlePresets::allocate()
{
    std::istringstream in(presetText());
    return std::unique_ptr<Preset>(new MilkdropPreset(in));
}

} // namespace MilkdropPreset
} // namespace libprojectM
