#pragma once
#include <Pipeline.h>
#include <RenderContext.h>
#include <Vector4.h>

namespace Foreground
{

// Header only shader, cannot be compiled individually
class CBasicMaterialShader
{
public:
    // Shader interface: this should mirror the shader code
    struct MaterialConstants
    {
        tc::Vector4 BaseColor;
        tc::Vector4 MetallicRoughness;
        uint32_t UseTextures;
    };

    static void BindMaterialConstants(RHI::IRenderContext& context, MaterialConstants* data);
    static void BindBaseColorTex(RHI::IRenderContext& context, RHI::CImageView& img);
    static void BindMetallicRoughnessTex(RHI::IRenderContext& context, RHI::CImageView& img);

    static std::string GetHeaderName() { return "BasicMaterial.h"; }
};

}
