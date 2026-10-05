#include "MilkdropShader.hpp"

#include "PerFrameContext.hpp"
#include "PresetState.hpp"
#include "Utils.hpp"

#include <MilkdropStaticShaders.hpp>

#include <GLSLGenerator.h>
#include <HLSLParser.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace {

// Set once the render thread has created MilkdropStaticShaders, whose constructor queries GL. Before that, a thread
// without a GL context must not call MilkdropStaticShaders::Get().
std::atomic<bool> staticShadersReady{false};

// Shader translations made ahead of time by MilkdropShader::PrepareTranslations, on any thread, for the render
// thread to use instead of translating during a preset switch. Keyed by every input of the translation, so a hit
// is exactly what translating there would have produced. Small and bounded: it holds the presets the app expects
// to load next, and an entry the app did not load simply ages out.
class TranslationCache
{
public:
    static auto Find(const std::string& key, std::string& glsl) -> bool
    {
        std::lock_guard<std::mutex> lock(Mutex());
        for (auto it = Entries().begin(); it != Entries().end(); ++it)
        {
            if (it->first == key)
            {
                glsl = it->second;
                Entries().splice(Entries().begin(), Entries(), it);
                return true;
            }
        }
        return false;
    }

    static auto Contains(const std::string& key) -> bool
    {
        std::lock_guard<std::mutex> lock(Mutex());
        return std::any_of(Entries().begin(), Entries().end(), [&key](const Entry& entry) { return entry.first == key; });
    }

    static void Store(std::string key, std::string glsl)
    {
        std::lock_guard<std::mutex> lock(Mutex());
        Entries().emplace_front(std::move(key), std::move(glsl));
        while (Entries().size() > Capacity)
        {
            Entries().pop_back();
        }
    }

private:
    using Entry = std::pair<std::string, std::string>;

    // Two presets ahead (next and previous), two shaders each, two random-texture answers at most, with room over.
    static constexpr size_t Capacity = 16;

    static auto Mutex() -> std::mutex&
    {
        static std::mutex mutex;
        return mutex;
    }

    static auto Entries() -> std::list<Entry>&
    {
        static std::list<Entry> entries;
        return entries;
    }
};

// What ECMAScript's \s matches, restricted to the characters a preset's shader text can contain.
auto IsEcmaSpace(char c) -> bool
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ECMAScript's "." stops at these, so a ".*" at the end of a pattern runs to the end of the line.
auto EndOfLine(const std::string& text, size_t pos) -> size_t
{
    const size_t end = text.find_first_of("\r\n", pos);
    return end == std::string::npos ? text.size() : end;
}

// Finds the leftmost match of the regex `sampler(2D|3D|)(\s+|\().*` and returns its position, setting length.
// A hand-written scan with exactly that regex's result: std::regex costs about a fifth of a preset load on a
// phone because a new one was compiled on every pass of the loop that calls this.
auto FindSamplerDeclaration(const std::string& text, size_t& length) -> size_t
{
    static const std::string word = "sampler";
    for (size_t start = text.find(word); start != std::string::npos; start = text.find(word, start + 1))
    {
        size_t pos = start + word.size();
        // "2D" and "3D" are tried before the empty alternative, but only one of the three can be followed by
        // whitespace or "(", so the order cannot change the result.
        if (text.compare(pos, 2, "2D") == 0 || text.compare(pos, 2, "3D") == 0)
        {
            pos += 2;
        }
        if (pos >= text.size())
        {
            continue;
        }
        if (text[pos] == '(')
        {
            pos++;
        }
        else if (IsEcmaSpace(text[pos]))
        {
            // \s+ is greedy and ".*" can match nothing, so the whitespace run is always taken whole,
            // line breaks included.
            while (pos < text.size() && IsEcmaSpace(text[pos]))
            {
                pos++;
            }
        }
        else
        {
            continue;
        }
        length = EndOfLine(text, pos) - start;
        return start;
    }
    return std::string::npos;
}

// Finds the leftmost match of the regex `float4\s+texsize_.*`, as FindSamplerDeclaration does for its pattern.
auto FindTexSizeDeclaration(const std::string& text, size_t& length) -> size_t
{
    static const std::string word = "float4";
    static const std::string name = "texsize_";
    for (size_t start = text.find(word); start != std::string::npos; start = text.find(word, start + 1))
    {
        size_t pos = start + word.size();
        const size_t spaceStart = pos;
        while (pos < text.size() && IsEcmaSpace(text[pos]))
        {
            pos++;
        }
        // Backing off the greedy \s+ would leave a whitespace character where "t" is needed, so the whole run
        // is the only way this can match.
        if (pos == spaceStart || text.compare(pos, name.size(), name) != 0)
        {
            continue;
        }
        length = EndOfLine(text, pos + name.size()) - start;
        return start;
    }
    return std::string::npos;
}

} // namespace

namespace libprojectM {
namespace MilkdropPreset {

using libprojectM::MilkdropPreset::MilkdropStaticShaders;

static auto floatRand = []() { return static_cast<float>(rand() % 7381) / 7380.0f; };

MilkdropShader::MilkdropShader(ShaderType type)
    : m_type(type)
    , m_randValues({floatRand(), floatRand(), floatRand(), floatRand()})
{
    unsigned int index = 0;
    do
    {
        for (int i = 0; i < 4; i++)
        {
            float const m_randTranslationMult = 1;
            float const rotMult = 0.9f * powf(index / 8.0f, 3.2f);
            m_randTranslation[index].x = (floatRand() * 2 - 1) * m_randTranslationMult;
            m_randTranslation[index].y = (floatRand() * 2 - 1) * m_randTranslationMult;
            m_randTranslation[index].z = (floatRand() * 2 - 1) * m_randTranslationMult;
            m_randRotationCenters[index].x = floatRand() * 6.28f;
            m_randRotationCenters[index].y = floatRand() * 6.28f;
            m_randRotationCenters[index].z = floatRand() * 6.28f;
            m_randRotationSpeeds[index].x = (floatRand() * 2 - 1) * rotMult;
            m_randRotationSpeeds[index].y = (floatRand() * 2 - 1) * rotMult;
            m_randRotationSpeeds[index].z = (floatRand() * 2 - 1) * rotMult;
            index++;
        }
    } while (index < sizeof(m_randTranslation) / sizeof(m_randTranslation[0]));
}

void MilkdropShader::LoadCode(const std::string& presetShaderCode)
{
    m_fragmentShaderCode = presetShaderCode;
    m_preprocessedCode = m_fragmentShaderCode;

    GetReferencedSamplers(m_preprocessedCode, m_samplerNames, m_maxBlurLevelRequired);
    PreprocessPresetShader(m_type, m_preprocessedCode);

    // PreprocessPresetShader has created MilkdropStaticShaders, on the render thread.
    staticShadersReady.store(true, std::memory_order_release);
}

void MilkdropShader::LoadTexturesAndCompile(PresetState& presetState)
{
    std::locale loc;

    // Now request the textures and descriptors from the texture manager.
    for (const auto& name : m_samplerNames)
    {
        std::string baseName = name;
        if (name.length() > 3 && name.at(2) == '_')
        {
            baseName = name.substr(3);
        }

        std::string lowerCaseName = Utils::ToLower(baseName);

        // The "main" and "blurX" textures are preset-specific and are not managed by TextureManager.
        if (lowerCaseName == "main")
        {
            Renderer::TextureSamplerDescriptor desc(presetState.mainTexture.lock(),
                                                    presetState.renderContext.textureManager->GetSampler(name),
                                                    name,
                                                    "main");
            m_mainTextureDescriptors.push_back(std::move(desc));
            continue;
        }

        // A few presets directly use the (undocumented) sampler name.
        if (lowerCaseName == "blur1")
        {
            UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur1, m_samplerNames, m_maxBlurLevelRequired);
            continue;
        }
        if (lowerCaseName == "blur2")
        {
            UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur2, m_samplerNames, m_maxBlurLevelRequired);
            continue;
        }
        if (lowerCaseName == "blur3")
        {
            UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur3, m_samplerNames, m_maxBlurLevelRequired);
            continue;
        }

        // Random textures need special treatment.
        if (lowerCaseName.length() >= 6 &&
            lowerCaseName.substr(0, 4) == "rand" && std::isdigit(lowerCaseName.at(4), loc) && std::isdigit(lowerCaseName.at(5), loc))
        {
            // First look up the random texture index in the preset state so the texture matches between warp and composite shaders
            int randomSlot = -1;
            try
            {
                randomSlot = std::stoi(lowerCaseName.substr(4, 2));
            }
            catch (...) // Ignore any conversion errors.
            {
            }

            if (randomSlot >= 0 && randomSlot <= 15)
            {
                if (presetState.randomTextureDescriptors.find(randomSlot) != presetState.randomTextureDescriptors.end())
                {
                    // Use existing texture descriptor.
                    m_textureSamplerDescriptors.push_back(presetState.randomTextureDescriptors.at(randomSlot));
                    continue;
                }

                // Slot empty, request a new random texture.
                auto desc = presetState.renderContext.textureManager->GetRandomTexture(name);

                // Also store a copy in preset state!
                presetState.randomTextureDescriptors.insert({randomSlot, desc});

                m_textureSamplerDescriptors.push_back(std::move(desc));
                continue;
            }

            // Fall through if slot number is out of range and treat as normal texture.
        }

        auto desc = presetState.renderContext.textureManager->GetTexture(name);
        m_textureSamplerDescriptors.push_back(std::move(desc));
    }

    // Now that we have the textures, transpile the code.
    TranspileHLSLShader(presetState, m_preprocessedCode);

    // Update blur texture level if shader was compiled successfully.
    presetState.blurTexture.SetRequiredBlurLevel(m_maxBlurLevelRequired);
}

void MilkdropShader::LoadVariables(const PresetState& presetState, const PerFrameContext& perFrameContext)
{
    // These are the inputs: http://www.geisswerks.com/milkdrop/milkdrop_preset_authoring.html#3f6

    auto floatTime = static_cast<float>(presetState.renderContext.time);
    auto timeSincePresetStartWrapped = floatTime - static_cast<int>(floatTime / 10000.0) * 10000;
    auto mipX = logf(static_cast<float>(presetState.renderContext.viewportSizeX)) / logf(2.0f);
    auto mipY = logf(static_cast<float>(presetState.renderContext.viewportSizeY)) / logf(2.0f);
    auto mipAvg = 0.5f * (mipX + mipY);

    BlurTexture::Values blurMin;
    BlurTexture::Values blurMax;
    BlurTexture::GetSafeBlurMinMaxValues(perFrameContext, blurMin, blurMax);

    m_shader.Bind();

    m_shader.SetUniformMat4x4("vertex_transformation", PresetState::orthogonalProjection);

    if (m_type == ShaderType::CompositeShader)
    {
        m_shader.SetUniformFloat("_tone_knee", presetState.renderContext.toneMapKnee);
    }

    m_shader.SetUniformFloat4("rand_frame", {floatRand(),
                                             floatRand(),
                                             floatRand(),
                                             floatRand()});
    m_shader.SetUniformFloat4("rand_preset", {m_randValues[0],
                                              m_randValues[1],
                                              m_randValues[2],
                                              m_randValues[3]});

    m_shader.SetUniformFloat4("_c0", {presetState.renderContext.aspectX,
                                      presetState.renderContext.aspectY,
                                      1.0f / presetState.renderContext.aspectX,
                                      1.0f / presetState.renderContext.aspectY});
    m_shader.SetUniformFloat4("_c1", {0.0,
                                      0.0,
                                      0.0,
                                      0.0});
    m_shader.SetUniformFloat4("_c2", {timeSincePresetStartWrapped,
                                      presetState.renderContext.fps,
                                      presetState.renderContext.frame,
                                      presetState.renderContext.progress});
    m_shader.SetUniformFloat4("_c3", {presetState.audioData.bass,
                                      presetState.audioData.mid,
                                      presetState.audioData.treb,
                                      presetState.audioData.vol});
    m_shader.SetUniformFloat4("_c4", {presetState.audioData.bassAtt,
                                      presetState.audioData.midAtt,
                                      presetState.audioData.trebAtt,
                                      presetState.audioData.volAtt});
    m_shader.SetUniformFloat4("_c5", {blurMax[0] - blurMin[0],
                                      blurMin[0],
                                      blurMax[1] - blurMin[1],
                                      blurMin[1]});
    m_shader.SetUniformFloat4("_c6", {blurMax[2] - blurMin[2],
                                      blurMin[2],
                                      blurMin[0],
                                      blurMax[0]});
    m_shader.SetUniformFloat4("_c7", {presetState.renderContext.viewportSizeX,
                                      presetState.renderContext.viewportSizeY,
                                      1.0f / static_cast<float>(presetState.renderContext.viewportSizeX),
                                      1.0f / static_cast<float>(presetState.renderContext.viewportSizeY)});

    m_shader.SetUniformFloat4("_c8", {0.5f + 0.5f * cosf(floatTime * 0.329f + 1.2f),
                                      0.5f + 0.5f * cosf(floatTime * 1.293f + 3.9f),
                                      0.5f + 0.5f * cosf(floatTime * 5.070f + 2.5f),
                                      0.5f + 0.5f * cosf(floatTime * 20.051f + 5.4f)});

    m_shader.SetUniformFloat4("_c9", {0.5f + 0.5f * sinf(floatTime * 0.329f + 1.2f),
                                      0.5f + 0.5f * sinf(floatTime * 1.293f + 3.9f),
                                      0.5f + 0.5f * sinf(floatTime * 5.070f + 2.5f),
                                      0.5f + 0.5f * sinf(floatTime * 20.051f + 5.4f)});

    m_shader.SetUniformFloat4("_c10", {0.5f + 0.5f * cosf(floatTime * 0.0050f + 2.7f),
                                       0.5f + 0.5f * cosf(floatTime * 0.0085f + 5.3f),
                                       0.5f + 0.5f * cosf(floatTime * 0.0133f + 4.5f),
                                       0.5f + 0.5f * cosf(floatTime * 0.0217f + 3.8f)});

    m_shader.SetUniformFloat4("_c11", {0.5f + 0.5f * sinf(floatTime * 0.0050f + 2.7f),
                                       0.5f + 0.5f * sinf(floatTime * 0.0085f + 5.3f),
                                       0.5f + 0.5f * sinf(floatTime * 0.0133f + 4.5f),
                                       0.5f + 0.5f * sinf(floatTime * 0.0217f + 3.8f)});

    m_shader.SetUniformFloat4("_c12", {mipX,
                                       mipY,
                                       mipAvg,
                                       0});
    m_shader.SetUniformFloat4("_c13", {blurMin[1],
                                       blurMax[1],
                                       blurMin[2],
                                       blurMax[2]});


    std::array<glm::mat4, 24> tempMatrices{};

    // write matrices
    for (int i = 0; i < 20; i++)
    {
        glm::mat4 const rotationX = glm::rotate(glm::mat4(1.0f), m_randRotationCenters[i].x + m_randRotationSpeeds[i].x * floatTime, glm::vec3(1.0f, 0.0f, 0.0f));
        glm::mat4 const rotationY = glm::rotate(glm::mat4(1.0f), m_randRotationCenters[i].y + m_randRotationSpeeds[i].y * floatTime, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 const rotationZ = glm::rotate(glm::mat4(1.0f), m_randRotationCenters[i].z + m_randRotationSpeeds[i].z * floatTime, glm::vec3(0.0f, 0.0f, 1.0f));

        glm::mat4 const randomTranslation = glm::translate(glm::mat4(1.0f), glm::vec3(m_randTranslation[i].x, m_randTranslation[i].y, m_randTranslation[i].z));

        tempMatrices[i] = randomTranslation * rotationX;
        tempMatrices[i] = rotationZ * tempMatrices[i];
        tempMatrices[i] = rotationY * tempMatrices[i];
    }

    // the last 4 are totally random, each frame
    for (int i = 20; i < 24; i++)
    {
        glm::mat4 const rotationX = glm::rotate(glm::mat4(1.0f), floatRand() * 6.28f, glm::vec3(1.0f, 0.0f, 0.0f));
        glm::mat4 const rotationY = glm::rotate(glm::mat4(1.0f), floatRand() * 6.28f, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 const rotationZ = glm::rotate(glm::mat4(1.0f), floatRand() * 6.28f, glm::vec3(0.0f, 0.0f, 1.0f));

        glm::mat4 const randomTranslation = glm::translate(glm::mat4(1.0f), glm::vec3(floatRand(), floatRand(), floatRand()));

        tempMatrices[i] = randomTranslation * rotationX;
        tempMatrices[i] = rotationZ * tempMatrices[i];
        tempMatrices[i] = rotationY * tempMatrices[i];
    }

    m_shader.SetUniformMat3x4("rot_s1", tempMatrices[0]);
    m_shader.SetUniformMat3x4("rot_s2", tempMatrices[1]);
    m_shader.SetUniformMat3x4("rot_s3", tempMatrices[2]);
    m_shader.SetUniformMat3x4("rot_s4", tempMatrices[3]);
    m_shader.SetUniformMat3x4("rot_d1", tempMatrices[4]);
    m_shader.SetUniformMat3x4("rot_d2", tempMatrices[5]);
    m_shader.SetUniformMat3x4("rot_d3", tempMatrices[6]);
    m_shader.SetUniformMat3x4("rot_d4", tempMatrices[7]);
    m_shader.SetUniformMat3x4("rot_f1", tempMatrices[8]);
    m_shader.SetUniformMat3x4("rot_f2", tempMatrices[9]);
    m_shader.SetUniformMat3x4("rot_f3", tempMatrices[10]);
    m_shader.SetUniformMat3x4("rot_f4", tempMatrices[11]);
    m_shader.SetUniformMat3x4("rot_vf1", tempMatrices[12]);
    m_shader.SetUniformMat3x4("rot_vf2", tempMatrices[13]);
    m_shader.SetUniformMat3x4("rot_vf3", tempMatrices[14]);
    m_shader.SetUniformMat3x4("rot_vf4", tempMatrices[15]);
    m_shader.SetUniformMat3x4("rot_uf1", tempMatrices[16]);
    m_shader.SetUniformMat3x4("rot_uf2", tempMatrices[17]);
    m_shader.SetUniformMat3x4("rot_uf3", tempMatrices[18]);
    m_shader.SetUniformMat3x4("rot_uf4", tempMatrices[19]);
    m_shader.SetUniformMat3x4("rot_rand1", tempMatrices[20]);
    m_shader.SetUniformMat3x4("rot_rand2", tempMatrices[21]);
    m_shader.SetUniformMat3x4("rot_rand3", tempMatrices[22]);
    m_shader.SetUniformMat3x4("rot_rand4", tempMatrices[23]);

    // set program uniform "_q[a-h]" values (_qa.x, _qa.y, _qa.z, _qa.w, _qb.x, _qb.y ... ) alias q[1-32]
    for (int i = 0; i < QVarCount; i += 4)
    {
        std::string varName = "_q";
        varName.push_back(static_cast<char>('a' + i / 4));
        m_shader.SetUniformFloat4(varName.c_str(), {presetState.frameQVariables[i],
                                                    presetState.frameQVariables[i + 1],
                                                    presetState.frameQVariables[i + 2],
                                                    presetState.frameQVariables[i + 3]});
    }

    // Bind all texture and sampler descriptors. This includes the main and blur textures.
    GLint textureUnit{0};
    for (auto& desc : m_mainTextureDescriptors)
    {
        // Update main texture, swaps every frame.
        desc.Texture(presetState.mainTexture);
        desc.Bind(textureUnit, m_shader);
        textureUnit++;
    }
    presetState.blurTexture.Bind(textureUnit, m_shader);
    for (auto& desc : m_textureSamplerDescriptors)
    {
        if (desc.Empty())
        {
            desc.TryUpdate(*presetState.renderContext.textureManager);
        }
        desc.Bind(textureUnit, m_shader);
        textureUnit++;
    }
}

auto MilkdropShader::Shader() -> Renderer::Shader&
{
    return m_shader;
}

void MilkdropShader::PreprocessPresetShader(ShaderType type, std::string& program)
{

    if (program.length() <= 0)
    {
        throw Renderer::ShaderException("Preset shader is declared, but empty.");
    }

    size_t found;

    // Find "sampler_state" overrides and remove them first, as they're not supported by GLSL.
    // The logic isn't totally fool-proof, but should work in general.
    // Use a comment-stripped copy for searching so commented-out sampler_state blocks are skipped.
    // StripComments preserves string length, so positions map 1:1 to the original.
    std::string stripped = Utils::StripComments(program);
    found = stripped.find("sampler_state");
    while (found != std::string::npos)
    {
        // Now go backwards and find the assignment
        found = stripped.rfind('=', found);
        auto startPos = found;

        // Find closing brace and semicolon
        found = stripped.find('}', found);
        found = stripped.find(';', found);

        if (found != std::string::npos)
        {
            stripped.replace(startPos, found - startPos, "");
        }
        else
        {
            // No closing brace and semicolon.
            break;
        }

        found = stripped.find("sampler_state");
    }

    // replace shader_body with entry point function
    // Use the stripped copy so a commented-out shader_body is not matched.
    found = stripped.find("shader_body");
    if (found != std::string::npos)
    {
        if (type == ShaderType::WarpShader)
        {
            program.replace(int(found), 11, R"(
void PS(float4 _vDiffuse : COLOR,
        float4 _uv : TEXCOORD0,
        float2 _rad_ang : TEXCOORD1,
        out float4 _return_value : COLOR0,
        out float4 _mv_tex_coords : COLOR1)
)");
        }
        else
        {
            program.replace(int(found), 11, R"(
void PS(float4 _vDiffuse : COLOR,
        float2 _uv : TEXCOORD0,
        float2 _rad_ang : TEXCOORD1,
        out float4 _return_value : COLOR)
)");
        }
    }
    else
    {
        throw Renderer::ShaderException("Preset shader is missing \"shader_body\" entry point.");
    }

    // replace the "{" immediately following shader_body with some variable declarations
    found = program.find('{', found);
    if (found != std::string::npos)
    {
        std::string progMain = "{\nfloat3 ret = 0;\n";
        if (type == ShaderType::WarpShader)
        {
            progMain.append("_mv_tex_coords.xy = _uv.xy;\n");
        }
        program.replace(int(found), 1, progMain);
    }
    else
    {
        throw Renderer::ShaderException("Preset shader has no opening braces.");
    }

    // replace "}" with return statement (this can probably be optimized for the GLSL conversion...)
    found = program.rfind('}');
    if (found != std::string::npos)
    {
        if (type == ShaderType::CompositeShader)
        {
            // The composite's result is the last float value before the 8-bit output clamps it. Above the knee,
            // scale the whole colour so its brightest channel follows a shoulder towards 1.0 rather than clipping:
            // the ratio between channels, and so the hue, survives. The shoulder meets the identity with the same
            // slope at the knee, so nothing below it moves. _tone_knee is 0 when this is switched off.
            program.replace(int(found), 1, "if (_tone_knee > 0.0)\n"
                                           "{\n"
                                           "float _tone_peak = max(ret.x, max(ret.y, ret.z));\n"
                                           "if (_tone_peak > _tone_knee)\n"
                                           "{\n"
                                           "float _tone_room = 1.0 - _tone_knee;\n"
                                           "float _tone_rolled = _tone_knee + _tone_room * (1.0 - exp((_tone_knee - _tone_peak) / _tone_room));\n"
                                           "ret *= _tone_rolled / _tone_peak;\n"
                                           "}\n"
                                           "}\n"
                                           "_return_value = float4(ret.xyz, 1.0);\n"
                                           "}\n");
        }
        else
        {
            program.replace(int(found), 1, "_return_value = float4(ret.xyz, 1.0);\n"
                                           "}\n");
        }
    }
    else
    {
        throw Renderer::ShaderException("Preset shader has no closing brace.");
    }

    // Find matching closing brace and cut off excess text after shader's main function
    int bracesOpen = 1;
    size_t pos = found + 1;
    for (; pos < program.length() && bracesOpen > 0; ++pos)
    {
        switch (program.at(pos))
        {
            case '/':
                // Skip line comments until EoL to prevent false counting
                if (pos < program.length() - 1 && program.at(pos + 1) == '/')
                {
                    for (; pos < program.length(); ++pos)
                    {
                        if (program.at(pos) == '\n')
                        {
                            break;
                        }
                    }
                }
                // Skip block comments to prevent false counting
                else if (pos < program.length() - 1 && program.at(pos + 1) == '*')
                {
                    pos += 2;
                    for (; pos < program.length() - 1; ++pos)
                    {
                        if (program.at(pos) == '*' && program.at(pos + 1) == '/')
                        {
                            ++pos; // skip past '/'
                            break;
                        }
                    }
                }
                continue;

            case '{':
                bracesOpen++;
                continue;

            case '}':
                bracesOpen--;
        }
    }

    if (pos < program.length() - 1)
    {
        program.resize(pos);
    }

    std::string fullSource; //!< Full shader source before translation, includes all uniforms etc.

    // First copy the generic "header" into the shader. Includes uniforms and some defines
    // to unwrap the packed 4-element uniforms into single values.
    fullSource.append(MilkdropStaticShaders::Get()->GetPresetShaderHeader());

    if (type == ShaderType::WarpShader)
    {
        fullSource.append("#define rad _rad_ang.x\n"
                          "#define ang _rad_ang.y\n"
                          "#define uv _uv.xy\n"
                          "#define uv_orig _uv.zw\n");
    }
    else
    {
        fullSource.append("#define rad _rad_ang.x\n"
                          "#define ang _rad_ang.y\n"
                          "#define uv _uv.xy\n"
                          "#define uv_orig _uv.xy\n"
                          "#define hue_shader _vDiffuse.xyz\n"
                          "uniform float _tone_knee;\n");
    }

    fullSource.append(program);

    program = fullSource;
}

void MilkdropShader::GetReferencedSamplers(const std::string& program,
                                           std::set<std::string>& samplerNames,
                                           BlurTexture::BlurLevel& maxBlurLevel)
{
    // Look up samplers referenced in the shader program
    samplerNames.clear();

    // "main" should always be present.
    samplerNames.insert("main");

    // Strip comments so that commented-out sampler/texsize declarations are not matched.
    std::string const stripped = Utils::StripComments(program);

    // Search for sampler usage
    auto found = stripped.find("sampler_", 0);
    while (found != std::string::npos)
    {
        found += 8;
        size_t const end = stripped.find_first_of(" ;,\n\r)", found);

        if (end != std::string::npos)
        {
            std::string const sampler = stripped.substr(static_cast<int>(found), static_cast<int>(end - found));
            // Skip "sampler_state", as it's a reserved word and not a sampler.
            if (sampler != "state")
            {
                samplerNames.insert(sampler);
            }
        }

        found = stripped.find("sampler_", found);
    }

    // Also search for texsize usage, some presets don't reference the sampler.
    found = stripped.find("texsize_", 0);
    while (found != std::string::npos)
    {
        found += 8;
        size_t const end = stripped.find_first_of(" ;,.\n\r)", found);

        if (end != std::string::npos)
        {
            std::string const sampler = stripped.substr(static_cast<int>(found), static_cast<int>(end - found));
            samplerNames.insert(sampler);
        }

        found = stripped.find("texsize_", found);
    }

    {
        // Remove duplicate mentions or "randXX" names, keeping the long forms only (first one will determine the actual texture loaded).
        auto samplerName = samplerNames.begin();
        std::locale loc;
        while (samplerName != samplerNames.end())
        {
            std::string lowerCaseName = Utils::ToLower(*samplerName);
            if (lowerCaseName.length() == 6 &&
                lowerCaseName.substr(0, 4) == "rand" && std::isdigit(lowerCaseName.at(4), loc) && std::isdigit(lowerCaseName.at(5), loc))
            {
                auto additionalName = samplerName;
                additionalName++;
                if (additionalName != samplerNames.end())
                {
                    std::string addLowerCaseName = Utils::ToLower(*additionalName);
                    if (addLowerCaseName.length() > 7 &&
                        addLowerCaseName.substr(0, 6) == lowerCaseName &&
                        addLowerCaseName[6] == '_')
                    {
                        samplerName = samplerNames.erase(samplerName);
                    }
                }
            }
            samplerName++;
        }
    }

    if (stripped.find("GetBlur3") != std::string::npos)
    {
        UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur3, samplerNames, maxBlurLevel);
    }
    else if (stripped.find("GetBlur2") != std::string::npos)
    {
        UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur2, samplerNames, maxBlurLevel);
    }
    else if (stripped.find("GetBlur1") != std::string::npos)
    {
        UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur1, samplerNames, maxBlurLevel);
    }
    else
    {
        maxBlurLevel = BlurTexture::BlurLevel::None;
    }
}

void MilkdropShader::TranspileHLSLShader(const PresetState& presetState, std::string& program)
{
    // Collect unique samplers and texsize uniforms
    std::set<std::string> samplerDeclarations;
    std::set<std::string> texSizeDeclarations;
    for (const auto& desc : m_mainTextureDescriptors)
    {
        samplerDeclarations.insert(desc.SamplerDeclaration());
        texSizeDeclarations.insert(desc.TexSizeDeclaration());
    }
    for (const auto& desc : presetState.blurTexture.GetDescriptorsForBlurLevel(m_maxBlurLevelRequired))
    {
        samplerDeclarations.insert(desc.SamplerDeclaration());
        // No texsize_blur1 etc.
    }
    for (const auto& desc : m_textureSamplerDescriptors)
    {
        samplerDeclarations.insert(desc.SamplerDeclaration());
        texSizeDeclarations.insert(desc.TexSizeDeclaration());
    }

    // A translation prepared ahead of time by PrepareTranslations is used only when it was made from exactly
    // these inputs; otherwise translate here, as before.
    const auto version = static_cast<int>(MilkdropStaticShaders::Get()->GetGlslGeneratorVersion());
    std::string glsl;
    if (!TranslationCache::Find(TranslationKey(m_type, version, samplerDeclarations, texSizeDeclarations, program), glsl))
    {
        glsl = TranslateToGlsl(m_type, program, samplerDeclarations, texSizeDeclarations, version);
    }

    // Now we have GLSL source for the preset shader program (hopefully it's valid!)
    // Compile the preset shader fragment shader with the standard vertex shader and cross our fingers.
    if (m_type == ShaderType::WarpShader)
    {
        m_shader.CompileProgram(MilkdropStaticShaders::Get()->GetPresetWarpVertexShader(), glsl);
    }
    else
    {
        m_shader.CompileProgram(MilkdropStaticShaders::Get()->GetPresetCompVertexShader(), glsl);
    }
}

auto MilkdropShader::TranslateToGlsl(ShaderType type,
                                     const std::string& program,
                                     const std::set<std::string>& samplerDeclarations,
                                     const std::set<std::string>& texSizeDeclarations,
                                     int glslGeneratorVersion) -> std::string
{
    std::string shaderTypeString = "composite";
    if (type == ShaderType::WarpShader)
    {
        shaderTypeString = "warp";
    }

    M4::GLSLGenerator generator;
    M4::Allocator allocator;

    M4::HLSLTree tree(&allocator);
    M4::HLSLParser parser(&allocator, &tree);

    // Preprocess define macros
    std::string sourcePreprocessed;
    if (!parser.ApplyPreprocessor("", program.c_str(), program.size(), sourcePreprocessed))
    {
        throw Renderer::ShaderException("Error translating HLSL " + shaderTypeString + " shader: Preprocessing failed.\nSource:\n" + program);
    }

    // Remove previous shader declarations
    // ToDo: Quite some presets declare a sampler_state{} struct to change the wrap mode.
    //       The below code causes invalid syntax as it leaves part of the expression.
    //       Leaving it in causes HLSLParser to add "sampler_XYZ = sampler2D( <unknown expression> );"
    //       in the main() function, which is also bad...
    size_t matchLength{};
    for (size_t pos; (pos = FindSamplerDeclaration(sourcePreprocessed, matchLength)) != std::string::npos;)
    {
        sourcePreprocessed.erase(pos, matchLength);
    }

    // Remove previous texsize declarations
    for (size_t pos; (pos = FindTexSizeDeclaration(sourcePreprocessed, matchLength)) != std::string::npos;)
    {
        sourcePreprocessed.erase(pos, matchLength);
    }

    // Now insert them on top.
    for (const auto& texSizeDeclaration : texSizeDeclarations)
    {
        sourcePreprocessed.insert(0, texSizeDeclaration);
    }
    for (const auto& samplerDeclaration : samplerDeclarations)
    {
        sourcePreprocessed.insert(0, samplerDeclaration);
    }

    // Transpile from HLSL (aka preset shader aka DirectX shader) to GLSL (aka OpenGL shader lang)
    // First, parse HLSL into a tree
    if (!parser.Parse("", sourcePreprocessed.c_str(), sourcePreprocessed.size()))
    {
        throw Renderer::ShaderException("Error translating HLSL " + shaderTypeString + " shader: HLSL parsing failed.\nSource:\n" + sourcePreprocessed);
    }

    // Then generate GLSL from the resulting parser tree
    if (!generator.Generate(&tree, M4::GLSLGenerator::Target_FragmentShader,
                            static_cast<M4::GLSLGenerator::Version>(glslGeneratorVersion),
                            "PS", M4::GLSLGenerator::Options(M4::GLSLGenerator::Flag_AlternateNanPropagation)))
    {
        throw Renderer::ShaderException("Error translating HLSL " + shaderTypeString + " shader: GLSL generating failed.\nSource:\n" + sourcePreprocessed);
    }

    return generator.GetResult();
}

void MilkdropShader::UpdateMaxBlurLevel(BlurTexture::BlurLevel requestedLevel,
                                        std::set<std::string>& samplerNames,
                                        BlurTexture::BlurLevel& maxBlurLevel)
{
    if (maxBlurLevel >= requestedLevel)
    {
        return;
    }

    maxBlurLevel = requestedLevel;

    if (maxBlurLevel == BlurTexture::BlurLevel::Blur3)
    {
        samplerNames.insert("blur1");
        samplerNames.insert("blur2");
        samplerNames.insert("blur3");
    }
    else if (maxBlurLevel == BlurTexture::BlurLevel::Blur2)
    {
        samplerNames.insert("blur1");
        samplerNames.insert("blur2");
    }
    else
    {
        samplerNames.insert("blur1");
    }
}

auto MilkdropShader::StaticShadersReady() -> bool
{
    return staticShadersReady.load(std::memory_order_acquire);
}

auto MilkdropShader::PrepareTranslations(int warpShaderVersion, const std::string& warpShaderCode,
                                         int compositeShaderVersion, const std::string& compositeShaderCode) -> bool
{
    if (!StaticShadersReady())
    {
        return false;
    }

    struct Source {
        ShaderType type;
        std::string code;
        std::set<std::string> samplerNames;
        BlurTexture::BlurLevel maxBlurLevel{BlurTexture::BlurLevel::None};
    };

    // As PerPixelMesh::LoadWarpShader and FinalComposite::LoadCompositeShader choose what to load. A composite
    // shader with no code uses a built-in default, which is small and is left to the render thread.
    std::vector<Source> sources;
    if (warpShaderVersion > 0 && !warpShaderCode.empty())
    {
        sources.push_back({ShaderType::WarpShader, warpShaderCode, {}});
    }
    if (compositeShaderVersion > 0 && !compositeShaderCode.empty())
    {
        sources.push_back({ShaderType::CompositeShader, compositeShaderCode, {}});
    }

    bool referencesRandomTextures = false;
    for (auto it = sources.begin(); it != sources.end();)
    {
        try
        {
            GetReferencedSamplers(it->code, it->samplerNames, it->maxBlurLevel);
            PreprocessPresetShader(it->type, it->code);
        }
        catch (Renderer::ShaderException&)
        {
            // The render thread will fail the same way and fall back as it always has.
            it = sources.erase(it);
            continue;
        }
        for (const auto& name : it->samplerNames)
        {
            referencesRandomTextures = referencesRandomTextures || RandomTextureSlot(name) >= 0;
        }
        ++it;
    }
    if (sources.empty())
    {
        return false;
    }

    const auto version = static_cast<int>(MilkdropStaticShaders::Get()->GetGlslGeneratorVersion());

    // Whether a random texture is found depends on the texture files on disk, which only the texture manager
    // knows. Translate for both answers when it matters; a wrong guess only costs a translation at load time.
    bool prepared = false;
    for (bool randomTexturesFound : {true, false})
    {
        if (!randomTexturesFound && !referencesRandomTextures)
        {
            break;
        }
        std::map<int, std::pair<std::string, bool>> randomSlots;
        for (const auto& source : sources)
        {
            // Each answer starts from what LoadCode left, as the render thread would.
            auto samplerNames = source.samplerNames;
            auto maxBlurLevel = source.maxBlurLevel;
            std::set<std::string> samplerDeclarations;
            std::set<std::string> texSizeDeclarations;
            PredictDeclarations(samplerNames, maxBlurLevel, randomTexturesFound, randomSlots,
                                samplerDeclarations, texSizeDeclarations);
            auto key = TranslationKey(source.type, version, samplerDeclarations, texSizeDeclarations, source.code);
            if (TranslationCache::Contains(key))
            {
                prepared = true;
                continue;
            }
            try
            {
                auto glsl = TranslateToGlsl(source.type, source.code, samplerDeclarations, texSizeDeclarations, version);
                TranslationCache::Store(std::move(key), std::move(glsl));
                prepared = true;
            }
            catch (Renderer::ShaderException&)
            {
                // Left for the render thread to fail on and fall back from, as it always has.
            }
        }
    }
    return prepared;
}

auto MilkdropShader::RandomTextureSlot(const std::string& samplerName) -> int
{
    std::string baseName = samplerName;
    if (samplerName.length() > 3 && samplerName.at(2) == '_')
    {
        baseName = samplerName.substr(3);
    }
    std::string const lowerCaseName = Utils::ToLower(baseName);
    std::locale loc;
    if (lowerCaseName.length() >= 6 &&
        lowerCaseName.substr(0, 4) == "rand" && std::isdigit(lowerCaseName.at(4), loc) && std::isdigit(lowerCaseName.at(5), loc))
    {
        int const slot = std::stoi(lowerCaseName.substr(4, 2));
        return slot <= 15 ? slot : -1;
    }
    return -1;
}

void MilkdropShader::PredictDeclarations(std::set<std::string>& samplerNames,
                                         BlurTexture::BlurLevel& maxBlurLevel,
                                         bool randomTexturesFound,
                                         std::map<int, std::pair<std::string, bool>>& randomSlots,
                                         std::set<std::string>& samplerDeclarations,
                                         std::set<std::string>& texSizeDeclarations)
{
    using Renderer::TextureSamplerDescriptor;

    // The same walk as LoadTexturesAndCompile, answering each texture manager question from what it would say.
    // It must insert into samplerNames as that loop does, because an inserted blur name may or may not be visited
    // after the insertion, and that has to match.
    for (const auto& name : samplerNames)
    {
        std::string baseName = name;
        if (name.length() > 3 && name.at(2) == '_')
        {
            baseName = name.substr(3);
        }
        std::string const lowerCaseName = Utils::ToLower(baseName);

        if (lowerCaseName == "main")
        {
            samplerDeclarations.insert(TextureSamplerDescriptor::SamplerDeclaration(name, false));
            texSizeDeclarations.insert(TextureSamplerDescriptor::TexSizeDeclaration("main"));
            continue;
        }
        if (lowerCaseName == "blur1")
        {
            UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur1, samplerNames, maxBlurLevel);
            continue;
        }
        if (lowerCaseName == "blur2")
        {
            UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur2, samplerNames, maxBlurLevel);
            continue;
        }
        if (lowerCaseName == "blur3")
        {
            UpdateMaxBlurLevel(BlurTexture::BlurLevel::Blur3, samplerNames, maxBlurLevel);
            continue;
        }

        int const randomSlot = RandomTextureSlot(name);
        if (randomSlot >= 0)
        {
            // The warp shader's pick is reused by the composite shader under the warp shader's name.
            auto slot = randomSlots.find(randomSlot);
            if (slot == randomSlots.end())
            {
                slot = randomSlots.insert({randomSlot, {name, randomTexturesFound}}).first;
            }
            if (slot->second.second)
            {
                samplerDeclarations.insert(TextureSamplerDescriptor::SamplerDeclaration(slot->second.first, false));
                texSizeDeclarations.insert(TextureSamplerDescriptor::TexSizeDeclaration(slot->second.first));
            }
            else
            {
                // An empty descriptor declares nothing, but still adds its empty string to both sets.
                samplerDeclarations.insert({});
                texSizeDeclarations.insert({});
            }
            continue;
        }

        // TextureManager::GetTexture always answers, with a placeholder if no file is found. Only the built-in
        // volume noise textures are 3D, and they are looked up by exact name.
        std::string const unqualifiedName = (name.length() <= 3 || name.at(2) != '_') ? name : name.substr(3);
        bool const is3D = unqualifiedName == "noisevol_lq" || unqualifiedName == "noisevol_hq";
        samplerDeclarations.insert(TextureSamplerDescriptor::SamplerDeclaration(name, is3D));
        texSizeDeclarations.insert(TextureSamplerDescriptor::TexSizeDeclaration(unqualifiedName));
    }

    // BlurTexture::GetDescriptorsForBlurLevel, whose textures are named blur1 to blur3 and have no texsize.
    int const blurCount = static_cast<int>(maxBlurLevel);
    for (int blur = 1; blur <= blurCount; blur++)
    {
        samplerDeclarations.insert(TextureSamplerDescriptor::SamplerDeclaration("blur" + std::to_string(blur), false));
    }
}

auto MilkdropShader::TranslationKey(ShaderType type,
                                    int glslGeneratorVersion,
                                    const std::set<std::string>& samplerDeclarations,
                                    const std::set<std::string>& texSizeDeclarations,
                                    const std::string& program) -> std::string
{
    // Everything TranslateToGlsl reads, so that equal keys can only mean equal translations. The separator
    // cannot occur in a declaration, and the program comes last so it needs none.
    std::string key;
    key.append(type == ShaderType::WarpShader ? "warp" : "composite");
    key.append(1, '\x1e');
    key.append(std::to_string(glslGeneratorVersion));
    key.append(1, '\x1e');
    for (const auto& declaration : samplerDeclarations)
    {
        key.append(declaration);
        key.append(1, '\x1f');
    }
    key.append(1, '\x1e');
    for (const auto& declaration : texSizeDeclarations)
    {
        key.append(declaration);
        key.append(1, '\x1f');
    }
    key.append(1, '\x1e');
    key.append(program);
    return key;
}

} // namespace MilkdropPreset
} // namespace libprojectM
