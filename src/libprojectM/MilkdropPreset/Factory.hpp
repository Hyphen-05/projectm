//
// C++ Interface: MilkdropPresetFactory
//
// Description:
//
//
// Author: Carmelo Piccione <carmelo.piccione@gmail.com>, (C) 2008
//
// Copyright: See COPYING file that comes with this distribution
//
//

#pragma once

#include <PresetFactory.hpp>

#include <memory>

namespace libprojectM {
namespace MilkdropPreset {

class Factory : public PresetFactory
{

public:
    std::unique_ptr<Preset> LoadPresetFromFile(const std::string& filename) override;

    std::unique_ptr<Preset> LoadPresetFromStream(std::istream& data) override;

    /**
     * @brief Does the part of LoadPresetFromFile that needs no GL context, ahead of it.
     * Safe on any thread, concurrently with rendering; see MilkdropPreset::Prepare.
     * @param filename The preset filename or URL, as LoadPresetFromFile will be given it.
     * @return True if anything was prepared.
     */
    static auto PreparePresetFromFile(const std::string& filename) -> bool;

    std::string supportedExtensions() const override
    {
        return ".milk .prjm";
    }

};

} // namespace MilkdropPreset
} // namespace libprojectM
