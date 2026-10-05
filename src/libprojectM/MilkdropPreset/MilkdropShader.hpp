/**
 * @file MilkdropShader
 * @brief Holds a warp or composite shader of Milkdrop presets.
 *
 * This class wraps the conversion from HLSL shader code to GLSL and also manages the
 * drawing.
 */
#pragma once

#include "BlurTexture.hpp"

#include <Renderer/Shader.hpp>
#include <Renderer/TextureManager.hpp>

#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace libprojectM {
namespace MilkdropPreset {

class PerFrameContext;
class PresetState;

/**
 * @brief Holds a warp or composite shader of Milkdrop presets.
 * Also does the required shader translation from HLSL to GLSL using hlslparser.
 */
class MilkdropShader
{
public:
    enum class ShaderType
    {
        WarpShader,     //!< Warp shader
        CompositeShader //!< Composite shader
    };

    /**
     * constructor.
     * @param type The preset shader type.
     */
    explicit MilkdropShader(ShaderType type);

    /**
     * @brief Translates and compiles the shader code.
     * @param presetShaderCode The preset shader code.
     */
    void LoadCode(const std::string& presetShaderCode);

    /**
     * @brief Loads the required texture references into the shader.
     * Binds the underlying shader program.
     * @param presetState The preset state to pull the values and textures from.
     */
    void LoadTexturesAndCompile(PresetState& presetState);

    /**
     * @brief Loads all required shader variables into the uniforms.
     * Binds the underlying shader program.
     * @param presetState The preset state to pull the values from.
     * @param perFrameContext The per-frame context with dynamically calculated values.
     */
    void LoadVariables(const PresetState& presetState, const PerFrameContext& perFrameContext);

    /**
     * @brief Returns the contained shader.
     * @return The shader program wrapper.
     */
    auto Shader() -> Renderer::Shader&;

    /**
     * @brief Whether PrepareTranslations can do anything yet.
     * It needs MilkdropStaticShaders, which only a thread with a GL context may create, so it waits for the render
     * thread to have loaded a preset shader first.
     */
    static auto StaticShadersReady() -> bool;

    /**
     * @brief Translates a preset's warp and composite shaders ahead of time, for a later load to use.
     *
     * Safe on any thread, concurrently with rendering: it touches no GL object, no texture manager and no projectM
     * instance. The results go to a small shared cache that TranspileHLSLShader consults, keyed by every input of
     * the translation, so a load uses one only if translating then would have produced exactly the same text.
     * The texture declarations are predicted from the sampler names; a wrong prediction only means the load
     * translates for itself, as it always did.
     *
     * @param warpShaderVersion The preset's warp shader version, as PresetState reads it.
     * @param warpShaderCode The preset's warp shader code.
     * @param compositeShaderVersion The preset's composite shader version, as PresetState reads it.
     * @param compositeShaderCode The preset's composite shader code.
     * @return True if at least one translation is now in the cache.
     */
    static auto PrepareTranslations(int warpShaderVersion, const std::string& warpShaderCode,
                                    int compositeShaderVersion, const std::string& compositeShaderCode) -> bool;

private:
    /**
     * @brief Prepares the shader code to be translated into GLSL.
     * @param type The shader type.
     * @param program The program code to work on.
     */
    static void PreprocessPresetShader(ShaderType type, std::string& program);

    /**
     * @brief Searches for sampler references in the program.
     * @param program The program code to work on.
     * @param samplerNames Receives the sampler names referenced.
     * @param maxBlurLevel Receives the blur level the program needs.
     */
    static void GetReferencedSamplers(const std::string& program,
                                      std::set<std::string>& samplerNames,
                                      BlurTexture::BlurLevel& maxBlurLevel);

    /**
     * @brief Translates the HLSL shader into GLSL and compiles it.
     * @param presetState The preset state to pull the blur textures from.
     * @param program The shader to transpile.
     */
    void TranspileHLSLShader(const PresetState& presetState, std::string& program);

    /**
     * @brief Translates preprocessed HLSL into GLSL. Pure: no GL, no textures, no state.
     * @throws Renderer::ShaderException if the shader cannot be translated.
     */
    static auto TranslateToGlsl(ShaderType type,
                                const std::string& program,
                                const std::set<std::string>& samplerDeclarations,
                                const std::set<std::string>& texSizeDeclarations,
                                int glslGeneratorVersion) -> std::string;

    /**
     * @brief The cache key for a translation: every input of TranslateToGlsl.
     */
    static auto TranslationKey(ShaderType type,
                               int glslGeneratorVersion,
                               const std::set<std::string>& samplerDeclarations,
                               const std::set<std::string>& texSizeDeclarations,
                               const std::string& program) -> std::string;

    /**
     * @brief The declarations LoadTexturesAndCompile would collect, predicted without a texture manager.
     * @param randomTexturesFound Whether the texture manager is assumed to find a file for each random texture.
     * @param randomSlots Random texture slots already taken by an earlier shader of the same preset.
     */
    static void PredictDeclarations(std::set<std::string>& samplerNames,
                                    BlurTexture::BlurLevel& maxBlurLevel,
                                    bool randomTexturesFound,
                                    std::map<int, std::pair<std::string, bool>>& randomSlots,
                                    std::set<std::string>& samplerDeclarations,
                                    std::set<std::string>& texSizeDeclarations);

    /**
     * @brief The random texture slot a sampler name refers to, 0 to 15, or -1 if it is not a random texture.
     */
    static auto RandomTextureSlot(const std::string& samplerName) -> int;

    /**
     * @brief Updates the requested blur level if higher than before.
     * Also adds the required samplers.
     * @param requestedLevel The requested blur level.
     * @param samplerNames The sampler names to add the blur samplers to.
     * @param maxBlurLevel The blur level to raise.
     */
    static void UpdateMaxBlurLevel(BlurTexture::BlurLevel requestedLevel,
                                   std::set<std::string>& samplerNames,
                                   BlurTexture::BlurLevel& maxBlurLevel);

    ShaderType m_type{ShaderType::WarpShader}; //!< Type of this shader.
    std::string m_fragmentShaderCode;          //!< The original preset fragment shader code.
    std::string m_preprocessedCode;            //!< The preprocessed preset shader code.

    std::set<std::string> m_samplerNames;                                        //!< All sampler names referenced in the shader code.
    std::vector<Renderer::TextureSamplerDescriptor> m_mainTextureDescriptors;              //!< Descriptors for all main texture references.
    std::vector<Renderer::TextureSamplerDescriptor> m_textureSamplerDescriptors;           //!< Descriptors of all referenced samplers in the shader code.
    BlurTexture::BlurLevel m_maxBlurLevelRequired{BlurTexture::BlurLevel::None}; //!< Max blur level of main texture required by this shader.

    std::array<float, 4> m_randValues{};               //!< Random values which don't change every frame.
    std::array<glm::vec3, 20> m_randTranslation{};     //!< Random translation vectors which don't change every frame.
    std::array<glm::vec3, 20> m_randRotationCenters{}; //!< Random rotation center vectors which don't change every frame.
    std::array<glm::vec3, 20> m_randRotationSpeeds{};  //!< Random rotation speeds which don't change every frame.

    Renderer::Shader m_shader;
};

} // namespace MilkdropPreset
} // namespace libprojectM
