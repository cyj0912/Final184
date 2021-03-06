#include "MegaPipeline.h"
#include "GBufferRenderer.h"
#include "Resources/ResourceManager.h"

#include <RHIImGuiBackend.h>
#include <ShaderModule.h>
#include <fstream>
#include <imgui.h>
#include <iostream>

namespace Foreground
{

    static RHI::CShaderModule::Ref LoadSPIRV(RHI::CDevice::Ref device, const std::string& path)
    {
        std::string filePath = CResourceManager::Get().FindShader(path);
        std::ifstream file(filePath.c_str(), std::ios::binary | std::ios::ate);
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(size);
        if (file.read(buffer.data(), size))
            return device->CreateShaderModule(buffer.size(), buffer.data());
        return {};
    }

    CMegaPipeline::CMegaPipeline(RHI::CSwapChain::Ref swapChain)
        : SwapChain(swapChain)
        , GBufferRenderer(this)
        , ZOnlyRenderer(this)
        , VoxelizeRenderer(this)
    {
        RHI::CSamplerDesc desc;
        desc.AddressModeU = RHI::ESamplerAddressMode::Clamp;
        desc.AddressModeV = RHI::ESamplerAddressMode::Clamp;
        desc.AddressModeW = RHI::ESamplerAddressMode::Clamp;
        desc.MaxLod = 4;
        GlobalLinearSamplerClamped = RenderDevice->CreateSampler(desc);
        desc.AddressModeU = RHI::ESamplerAddressMode::Wrap;
        desc.AddressModeV = RHI::ESamplerAddressMode::Wrap;
        desc.AddressModeW = RHI::ESamplerAddressMode::Wrap;
        desc.MaxLod = 4;
        GlobalLinearSampler = RenderDevice->CreateSampler(desc);
        desc.MinFilter = RHI::EFilter::Nearest;
        desc.MagFilter = RHI::EFilter::Nearest;
        desc.MipmapMode = RHI::ESamplerMipmapMode::Nearest;
        GlobalNearestSampler = RenderDevice->CreateSampler(desc);
        desc.AnisotropyEnable = true;
        desc.MaxAnisotropy = 16.0f;
        GlobalNiceSampler = RenderDevice->CreateSampler(desc);

        CreateRenderPasses();
        GBufferRenderer.SetRenderPass(GBufferPass);
        ZOnlyRenderer.SetRenderPass(ZOnlyPass);
        VoxelizeRenderer.SetRenderPass(VoxelizationPass);

        RHI::CRHIImGuiBackend::Init(RenderDevice, gtao_color->getRenderPass());

        PipelangContext.CreateLibrary("Internal").Parse();
    }

    void CMegaPipeline::SetSceneView(std::unique_ptr<CSceneView> sceneView,
        std::unique_ptr<CSceneView> shadowView,
        std::unique_ptr<CSceneView> voxelizerSceneView)
    {
        SceneView = std::move(sceneView);
        ShadowSceneView = std::move(shadowView);
        VoxelizerSceneView = std::move(voxelizerSceneView);

        prevProj.PrevModelView = SceneView->GetViewConstants().ViewMat;
        prevProj.PrevProjection = SceneView->GetViewConstants().ProjMat;
    }

    void CMegaPipeline::Resize()
    {
        std::cout << "Resizing" << std::endl;

        SwapChain->AutoResize();
        uint32_t w, h;
        SwapChain->GetSize(w, h);

        GBufferPass = nullptr;

        gtao_visibility = nullptr;
        gtao_blur = nullptr;
        gtao_color = nullptr;
        lighting_deferred = nullptr;

        CreateGBufferPass(w, h);
        CreateScreenPass();

        initial = true;
    }

    struct alignas(16) PerLightConstants
    {
        tc::Color color;
        tc::Vector3 position;
    };

    struct alignas(16) LightLists
    {
        PerLightConstants lights[100];
        int numLights;
    };

    void getAllLights(LightLists * lightsPoint, LightLists * lightsDirectioanl, CSceneNode * root)
    {
        tc::Vector3 position = root->GetWorldPosition();
        tc::Vector3 direction = root->GetWorldTransform() * tc::Vector4(0.0, 0.0, -1.0, 0.0);
        for (auto L : root->GetLights())
        {
            if (L->getType() == ELightType::Point)
            {
                lightsPoint->lights[lightsPoint->numLights] = { L->getLuminance(), position };
                lightsPoint->numLights++;
            }
            if (L->getType() == ELightType::Directional)
            {
                lightsDirectioanl->lights[lightsDirectioanl->numLights] = { L->getLuminance(),
                                                                            direction };
                lightsDirectioanl->numLights++;
            }
        }

        for (CSceneNode* child : root->GetChildren())
        {
            getAllLights(lightsPoint, lightsDirectioanl, child);
        }
    }

    struct alignas(16) ExtendedMatricesConstants
    {
        tc::Matrix4 InvModelView;
        tc::Matrix4 ShadowView;
        tc::Matrix4 ShadowProjection;
        tc::Matrix4 VoxelView;
        tc::Matrix4 VoxelProjection;
    };

    struct EngineCommonMiscs
    {
        tc::Vector2 resolution;
        uint32_t frameCount;
        float frameTime;
    };

    void CMegaPipeline::Render()
    {
        // Or render some test image?
        if (!SceneView || !VoxelImage)
            return;

        if (!SwapChain->AcquireNextImage())
        {
            return;
        }

        SwapChain->GetSize(width, height);

        // Does culling and stuff
        SceneView->GetCameraNode()->GetScene()->UpdateAccelStructure();
        SceneView->PrepareToRender();

        ShadowSceneView->PrepareToRender();

        VoxelizerSceneView->PrepareToRender();

        auto cmdList = RenderQueue->CreateCommandList();
        cmdList->Enqueue();

        CSceneNode* root = SceneView->GetCameraNode()->GetScene()->GetRootNode();
        LightLists pointLightLists, directionalLightLists;
        pointLightLists.numLights = 0;
        directionalLightLists.numLights = 0;
        getAllLights(&pointLightLists, &directionalLightLists, root);

        auto passCtx = cmdList->CreateParallelRenderContext(
            GBufferPass,
            { RHI::CClearValue(0.0f, 0.0f, 0.0f, 0.0f), RHI::CClearValue(0.0f, 0.0f, 0.0f, 0.0f),
              RHI::CClearValue(0.0f, 0.0f, 0.0f, 0.0f), RHI::CClearValue(1.0f, 0) });
        auto ctx = passCtx->CreateRenderContext(0);
        GBufferRenderer.RenderList(*ctx, SceneView->GetVisiblePrimModelMatrix(),
            SceneView->GetVisiblePrimitiveList());
        ctx->FinishRecording();
        passCtx->FinishRecording();

        passCtx = cmdList->CreateParallelRenderContext(ZOnlyPass, { RHI::CClearValue(1.0f, 0) });
        ctx = passCtx->CreateRenderContext(0);
        ZOnlyRenderer.RenderList(*ctx, ShadowSceneView->GetVisiblePrimModelMatrix(),
            ShadowSceneView->GetVisiblePrimitiveList());
        ctx->FinishRecording();
        passCtx->FinishRecording();

        auto copyCtx = cmdList->CreateCopyContext();
        copyCtx->ClearImage(*VoxelImage, { 0, 0, 0, 0 }, { 0, 1, 0, 1 });
        if (initial)
        {
            initial = false;
            copyCtx->ClearImage(*taaImageA, { 0, 0, 0, 0 }, { 0, 1, 0, 1 });
            copyCtx->ClearImage(*taaImageB, { 0, 0, 0, 0 }, { 0, 1, 0, 1 });
            copyCtx->ClearImage(*indirectImage, { 0, 0, 0, 0 }, { 0, 1, 0, 1 });
            copyCtx->ClearImage(*indirectTemporalImage, { 0, 0, 0, 0 }, { 0, 1, 0, 1 });
        }
        else
        {
            copyCtx->CopyImage(
                *taaImageA, *taaImageB,
                { RHI::CImageCopy {
                    { 0, 0, 1 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 0, 0 }, { width, height, 1 } } });
            copyCtx->CopyImage(
                *indirectImage, *indirectTemporalImage,
                { RHI::CImageCopy {
                    { 0, 0, 1 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 0, 0 }, { width, height, 1 } } }); 
        }
        copyCtx->FinishRecording();

        passCtx = cmdList->CreateParallelRenderContext(VoxelizationPass, {});
        ctx = passCtx->CreateRenderContext(0);
        VoxelizeRenderer.RenderList(*ctx, VoxelizerSceneView->GetVisiblePrimModelMatrix(),
            VoxelizerSceneView->GetVisiblePrimitiveList());
        ctx->FinishRecording();
        passCtx->FinishRecording();

        gtao_visibility->beginRender(cmdList);
        gtao_visibility->setSampler("s", GlobalLinearSampler);
        gtao_visibility->setImageView("t_albedo", GBuffer0);
        gtao_visibility->setImageView("t_normals", GBuffer1);
        gtao_visibility->setImageView("t_depth", GBufferDepth);
        gtao_visibility->setStruct("GlobalConstants", sizeof(CViewConstants),
            &SceneView->GetViewConstants());
        gtao_visibility->blit2d();
        gtao_visibility->endRender();

        gtao_blur->beginRender(cmdList);
        gtao_blur->setSampler("s", GlobalLinearSampler);
        gtao_blur->setImageView("t_ao", gtao_visibility->getRTViews()[0]);
        gtao_blur->blit2d();
        gtao_blur->endRender();

        ExtendedMatricesConstants matricesConstants;
        matricesConstants.InvModelView = SceneView->GetViewConstants().ViewMat.Inverse();
        matricesConstants.ShadowProjection = ShadowSceneView->GetViewConstants().ProjMat;
        matricesConstants.ShadowView = ShadowSceneView->GetViewConstants().ViewMat;
        matricesConstants.VoxelProjection = VoxelizerSceneView->GetViewConstants().ProjMat;
        matricesConstants.VoxelView = VoxelizerSceneView->GetViewConstants().ViewMat;

        EngineCommonMiscs miscs;
        miscs.frameCount = frameCount;
        miscs.resolution = tc::Vector2(width, height);

        lighting_indirect->beginRender(cmdList);
        lighting_indirect->setSampler("s", GlobalLinearSampler);
        lighting_indirect->setImageView("t_depth", GBufferDepth);
        lighting_indirect->setImageView("t_normals", GBuffer1);
        lighting_indirect->setImageView("t_shadow", ShadowDepth);
        lighting_indirect->setImageView("voxels", VoxelBuffer);
        lighting_indirect->setImageView("temporal", indirectTemporal);
        lighting_indirect->setStruct("GlobalConstants", sizeof(CViewConstants),
            &SceneView->GetViewConstants());
        lighting_indirect->setStruct("ExtendedMatrices", sizeof(ExtendedMatricesConstants),
            &matricesConstants);
        lighting_indirect->setStruct("prevProj", sizeof(PreviousProjections), &prevProj);
        lighting_indirect->setStruct("Sun", sizeof(PerLightConstants),
            &directionalLightLists.lights[0]);
        lighting_indirect->setStruct("EngineCommonMiscs", sizeof(EngineCommonMiscs), &miscs);
        lighting_indirect->blit2d();
        lighting_indirect->endRender();

        indirect_blurX->beginRender(cmdList);
        indirect_blurX->setSampler("s", GlobalLinearSamplerClamped);
        indirect_blurX->setImageView("t_depth", GBufferDepth);
        indirect_blurX->setImageView("t_indirect", lighting_indirect->getRTViews()[0]);
        indirect_blurX->setStruct("EngineCommonMiscs", sizeof(EngineCommonMiscs), &miscs);
        indirect_blurX->blit2d();
        indirect_blurX->endRender();

        indirect_blurY->beginRender(cmdList);
        indirect_blurY->setSampler("s", GlobalLinearSamplerClamped);
        indirect_blurY->setImageView("t_depth", GBufferDepth);
        indirect_blurY->setImageView("t_indirect", indirect_blurX->getRTViews()[0]);
        indirect_blurY->setStruct("EngineCommonMiscs", sizeof(EngineCommonMiscs), &miscs);
        indirect_blurY->blit2d();
        indirect_blurY->endRender();

        lighting_deferred->beginRender(cmdList);
        lighting_deferred->setSampler("s", GlobalLinearSampler);
        lighting_deferred->setImageView("t_albedo", GBuffer0);
        lighting_deferred->setImageView("t_normals", GBuffer1);
        lighting_deferred->setImageView("t_material", GBuffer2);
        lighting_deferred->setImageView("t_depth", GBufferDepth);
        lighting_deferred->setImageView("t_shadow", ShadowDepth);
        lighting_deferred->setStruct("GlobalConstants", sizeof(CViewConstants),
            &SceneView->GetViewConstants());
        lighting_deferred->setStruct("pointLights", sizeof(LightLists), &pointLightLists);
        lighting_deferred->setStruct("directionalLights", sizeof(LightLists), &directionalLightLists);
        lighting_deferred->setStruct("ExtendedMatrices", sizeof(ExtendedMatricesConstants),
            &matricesConstants);
        lighting_deferred->blit2d();
        lighting_deferred->endRender();

        gtao_color->beginRender(cmdList);
        gtao_color->setSampler("s", GlobalLinearSamplerClamped);
        gtao_color->setImageView("t_albedo", GBuffer0);
        gtao_color->setImageView("t_ao", gtao_blur->getRTViews()[0]);
        gtao_color->setImageView("t_depth", GBufferDepth);
        gtao_color->setImageView("t_lighting", lighting_deferred->getRTViews()[0]);
        gtao_color->setImageView("t_shadow", ShadowDepth);
        gtao_color->setImageView("t_indirect", indirect_blurY->getRTViews()[0]);
        gtao_color->setImageView("taaBuffer", taaImageView);
        gtao_color->setStruct("GlobalConstants", sizeof(CViewConstants),
            &SceneView->GetViewConstants());
        gtao_color->setStruct("ExtendedMatrices", sizeof(ExtendedMatricesConstants),
            &matricesConstants);
        gtao_color->setStruct("prevProj", sizeof(PreviousProjections), &prevProj);
        gtao_color->setStruct("Sun", sizeof(PerLightConstants), &directionalLightLists.lights[0]);
        gtao_color->setStruct("EngineCommonMiscs", sizeof(EngineCommonMiscs),
            &miscs);
        gtao_color->blit2d();

        prevProj.PrevModelView = SceneView->GetViewConstants().ViewMat;
        prevProj.PrevProjection = SceneView->GetViewConstants().ProjMat;

        // Render ImGui at the latest possible time so that we can still use ImGui inside renderer
        ImGui::Render();
        auto* drawData = ImGui::GetDrawData();
        if (drawData)
            RHI::CRHIImGuiBackend::RenderDrawData(drawData, *gtao_color->getContext());

        gtao_color->endRender();

        cmdList->Commit();

        SceneView->FrameFinished();
        ShadowSceneView->FrameFinished();
        VoxelizerSceneView->FrameFinished();

        RHI::CSwapChainPresentInfo info;
        SwapChain->Present(info);

        frameCount++;
    }

    void CMegaPipeline::BindEngineCommonForView(RHI::IRenderContext & context, uint32_t viewIndex)
    {
        auto& lib = PipelangContext.GetLibrary("Internal");
        auto& pb = lib.GetParameterBlock("EngineCommon");
        if (!EngineCommonDS)
            EngineCommonDS = pb.CreateDescriptorSet();

        pb.BindSampler(EngineCommonDS, GlobalNiceSampler, "GlobalNiceSampler");
        pb.BindSampler(EngineCommonDS, GlobalLinearSampler, "GlobalLinearSampler");
        pb.BindSampler(EngineCommonDS, GlobalNearestSampler, "GlobalNearestSampler");

        if (viewIndex == 0)
            pb.BindConstants(EngineCommonDS, &SceneView->GetViewConstants(), sizeof(CViewConstants),
                "GlobalConstants");
        else if (viewIndex == 1)
            pb.BindConstants(EngineCommonDS, &ShadowSceneView->GetViewConstants(),
                sizeof(CViewConstants), "GlobalConstants");
        else if (viewIndex == 2)
            pb.BindConstants(EngineCommonDS, &VoxelizerSceneView->GetViewConstants(),
                sizeof(CViewConstants), "GlobalConstants");

        EngineCommonMiscs miscs;
        miscs.frameCount = frameCount;
        miscs.resolution = tc::Vector2(width, height);

        pb.BindConstants(EngineCommonDS, &miscs,
            sizeof(EngineCommonMiscs), "EngineCommonMiscs");

        context.BindRenderDescriptorSet(0, *EngineCommonDS);

        // TODO: careful! EngineCommonDS will still be in use after leaving this function, what if
        // multiple thread calls this?
    }

    void CMegaPipeline::CreateRenderPasses()
    {
        uint32_t w, h;
        SwapChain->GetSize(w, h);
        CreateShadowBufferPass();
        CreateGBufferPass(w, h);
        CreateScreenPass();
        CreateVoxelizePass();
    }

    void CMegaPipeline::CreateShadowBufferPass()
    {
        using namespace RHI;

        auto depthImage = RenderDevice->CreateImage2D(
            EFormat::D32_SFLOAT_S8_UINT, EImageUsageFlags::DepthStencil | EImageUsageFlags::Sampled,
            2048, 2048);

        CImageViewDesc depthViewDesc;
        depthViewDesc.Format = EFormat::D32_SFLOAT_S8_UINT;
        depthViewDesc.Type = EImageViewType::View2D;
        depthViewDesc.DepthStencilAspect = EDepthStencilAspectFlags::Depth;
        depthViewDesc.Range.Set(0, 1, 0, 1);
        ShadowDepth = RenderDevice->CreateImageView(depthViewDesc, depthImage);

        CRenderPassDesc rpDesc;
        rpDesc.AddAttachment(ShadowDepth, EAttachmentLoadOp::Clear, EAttachmentStoreOp::Store);
        rpDesc.NextSubpass().SetDepthStencilAttachment(0);
        rpDesc.Width = 2048;
        rpDesc.Height = 2048;
        rpDesc.Layers = 1;
        ZOnlyPass = RenderDevice->CreateRenderPass(rpDesc);
    }

    void CMegaPipeline::CreateGBufferPass(uint32_t width, uint32_t height)
    {
        using namespace RHI;

        auto albedoImage = RenderDevice->CreateImage2D(
            EFormat::R8G8B8A8_UNORM, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled, width,
            height);
        auto normalsImage = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_UNORM, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);
        auto matPropImage = RenderDevice->CreateImage2D(
            EFormat::R8G8B8A8_UNORM, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled, width,
            height);
        auto depthImage = RenderDevice->CreateImage2D(
            EFormat::D32_SFLOAT_S8_UINT, EImageUsageFlags::DepthStencil | EImageUsageFlags::Sampled,
            width, height);

        CImageViewDesc albedoViewDesc;
        albedoViewDesc.Format = EFormat::R8G8B8A8_UNORM;
        albedoViewDesc.Type = EImageViewType::View2D;
        albedoViewDesc.Range.Set(0, 1, 0, 1);
        GBuffer0 = RenderDevice->CreateImageView(albedoViewDesc, albedoImage);

        CImageViewDesc normalsViewDesc;
        normalsViewDesc.Format = EFormat::R16G16B16A16_UNORM;
        normalsViewDesc.Type = EImageViewType::View2D;
        normalsViewDesc.Range.Set(0, 1, 0, 1);
        GBuffer1 = RenderDevice->CreateImageView(normalsViewDesc, normalsImage);

        CImageViewDesc matPropViewDesc;
        matPropViewDesc.Format = EFormat::R8G8B8A8_UNORM;
        matPropViewDesc.Type = EImageViewType::View2D;
        matPropViewDesc.Range.Set(0, 1, 0, 1);
        GBuffer2 = RenderDevice->CreateImageView(matPropViewDesc, matPropImage);

        CImageViewDesc depthViewDesc;
        depthViewDesc.Format = EFormat::D32_SFLOAT_S8_UINT;
        depthViewDesc.Type = EImageViewType::View2D;
        depthViewDesc.DepthStencilAspect = EDepthStencilAspectFlags::Depth;
        depthViewDesc.Range.Set(0, 1, 0, 1);
        GBufferDepth = RenderDevice->CreateImageView(depthViewDesc, depthImage);

        CRenderPassDesc rpDesc;
        rpDesc.AddAttachment(GBuffer0, EAttachmentLoadOp::Clear, EAttachmentStoreOp::Store);
        rpDesc.AddAttachment(GBuffer1, EAttachmentLoadOp::Clear, EAttachmentStoreOp::Store);
        rpDesc.AddAttachment(GBuffer2, EAttachmentLoadOp::Clear, EAttachmentStoreOp::Store);
        rpDesc.AddAttachment(GBufferDepth, EAttachmentLoadOp::Clear, EAttachmentStoreOp::Store);
        rpDesc.NextSubpass()
            .AddColorAttachment(0)
            .AddColorAttachment(1)
            .AddColorAttachment(2)
            .SetDepthStencilAttachment(3);
        rpDesc.Width = width;
        rpDesc.Height = height;
        rpDesc.Layers = 1;
        GBufferPass = RenderDevice->CreateRenderPass(rpDesc);
    }

    void CMegaPipeline::CreateVoxelizePass()
    {
        using namespace RHI;

        VoxelImage =
            RenderDevice->CreateImage3D(EFormat::R16G16_UINT, EImageUsageFlags::Storage,
                                                 128, 128, 128);

        CImageViewDesc voxelViewDesc;
        voxelViewDesc.Format = EFormat::R16G16_UINT;
        voxelViewDesc.Type = EImageViewType::View3D;
        voxelViewDesc.Range.Set(0, 1, 0, 1);
        VoxelBuffer = RenderDevice->CreateImageView(voxelViewDesc, VoxelImage);

        CRenderPassDesc rpDesc;
        rpDesc.NextSubpass().ColorAttachments.clear();
        rpDesc.Width = 128;
        rpDesc.Height = 128;
        rpDesc.Layers = 1;
        VoxelizationPass = RenderDevice->CreateRenderPass(rpDesc);
    }

    void CMegaPipeline::CreateScreenPass()
    {
        using namespace RHI;

        auto fbImage = SwapChain->GetImage();
        uint32_t width, height;
        SwapChain->GetSize(width, height);

        auto aoImage = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);
        auto aoImage2 = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);

        auto lightingImage = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);

        indirectImage = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);

        auto indirectImageBlur = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);

        auto indirectImageFinal = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);

        indirectTemporalImage = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT, EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);

        taaImageA = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT,
            EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled,
            width, height);

        taaImageB = RenderDevice->CreateImage2D(
            EFormat::R16G16B16A16_SFLOAT,
            EImageUsageFlags::RenderTarget | EImageUsageFlags::Sampled, width,
            height);

        CImageViewDesc taaImageViewDesc;
        taaImageViewDesc.Format = EFormat::R16G16B16A16_SFLOAT;
        taaImageViewDesc.Type = EImageViewType::View2D;
        taaImageViewDesc.Range.Set(0, 1, 0, 1);
        taaImageView = RenderDevice->CreateImageView(taaImageViewDesc, taaImageB);

        CImageViewDesc indTemporalIVDesc;
        indTemporalIVDesc.Format = EFormat::R16G16B16A16_SFLOAT;
        indTemporalIVDesc.Type = EImageViewType::View2D;
        indTemporalIVDesc.Range.Set(0, 1, 0, 1);
        indirectTemporal = RenderDevice->CreateImageView(indTemporalIVDesc, indirectTemporalImage);

        gtao_visibility = std::shared_ptr<CMaterial>(
            new CMaterial(RenderDevice, "Common/Quad.vert.spv", "GTAO/gtao.frag.spv"));

        gtao_visibility->renderTargets = { { aoImage, EFormat::R16G16B16A16_SFLOAT } };
        gtao_visibility->createPipeline(width, height);

        gtao_blur = std::shared_ptr<CMaterial>(
            new CMaterial(RenderDevice, "Common/Quad.vert.spv", "GTAO/blur.frag.spv"));

        gtao_blur->renderTargets = { { aoImage2, EFormat::R16G16B16A16_SFLOAT } };
        gtao_blur->createPipeline(width, height);

        gtao_color = std::shared_ptr<CMaterial>(
            new CMaterial(RenderDevice, "Common/Quad.vert.spv", "GTAO/color.frag.spv"));

        gtao_color->renderTargets = { { fbImage }, { taaImageA, EFormat::R16G16B16A16_SFLOAT } };
        gtao_color->createPipeline(width, height);

        lighting_deferred = std::shared_ptr<CMaterial>(
            new CMaterial(RenderDevice, "Common/Quad.vert.spv", "Lighting/aggregateLights.frag.spv"));

        lighting_deferred->renderTargets = { { lightingImage, EFormat::R16G16B16A16_SFLOAT } };
        lighting_deferred->createPipeline(width, height);

        lighting_indirect = std::shared_ptr<CMaterial>(
            new CMaterial(RenderDevice, "Common/Quad.vert.spv", "Lighting/indirect.frag.spv"));

        lighting_indirect->renderTargets = { { indirectImage, EFormat::R16G16B16A16_SFLOAT } };
        lighting_indirect->createPipeline(width, height);

        indirect_blurX = std::shared_ptr<CMaterial>(
            new CMaterial(RenderDevice, "Common/Quad.vert.spv", "Lighting/blurX.frag.spv"));

        indirect_blurX->renderTargets = { { indirectImageBlur, EFormat::R16G16B16A16_SFLOAT } };
        indirect_blurX->createPipeline(width, height);

        indirect_blurY = std::shared_ptr<CMaterial>(
            new CMaterial(RenderDevice, "Common/Quad.vert.spv", "Lighting/blurY.frag.spv"));

        indirect_blurY->renderTargets = { { indirectImageFinal, EFormat::R16G16B16A16_SFLOAT } };
        indirect_blurY->createPipeline(width, height);
    }

} /* namespace Foreground */
