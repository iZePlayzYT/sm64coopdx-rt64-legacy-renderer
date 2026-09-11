//
// RT64
//

#ifndef RT64_MINIMAL

#include "rt64_framegen.h"

#include "FidelityFX-SDK/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.hpp"
#include "FidelityFX-SDK/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.hpp"

#include "rt64_device.h"

namespace {
    D3D12_RESOURCE_STATES ConvertFfxResourceState(uint32_t ffxState) {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        if (ffxState & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) state |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (ffxState & FFX_API_RESOURCE_STATE_COMPUTE_READ) state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (ffxState & FFX_API_RESOURCE_STATE_PIXEL_READ) state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (ffxState & FFX_API_RESOURCE_STATE_COPY_SRC) state |= D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (ffxState & FFX_API_RESOURCE_STATE_COPY_DEST) state |= D3D12_RESOURCE_STATE_COPY_DEST;
        if (ffxState & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        if (ffxState & FFX_API_RESOURCE_STATE_PRESENT) state |= D3D12_RESOURCE_STATE_PRESENT;
        if (ffxState & FFX_API_RESOURCE_STATE_RENDER_TARGET) state |= D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (ffxState & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT) state |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
        return state;
    }
}

// FrameGen::Context

class RT64::FrameGen::Context {
private:
    Device *device;
    bool initialized;
    ffx::Context fgContext;
    bool fgFilled;
    FrameGenPresentCallback userPresentCallback;
    void *userPresentCallbackCtx;

    static ffxReturnCode_t presentCallbackTrampoline(ffxCallbackDescFrameGenerationPresent *ffxParams, void *userCtx) {
        Context *self = static_cast<Context *>(userCtx);
        if ((self != nullptr) && (self->userPresentCallback != nullptr)) {
            FrameGenPresentParams params{};
            params.device = ffxParams->device;
            params.commandList = ffxParams->commandList;
            params.backBufferColor = static_cast<ID3D12Resource *>(ffxParams->currentBackBuffer.resource);
            params.backBufferColorState = ConvertFfxResourceState(ffxParams->currentBackBuffer.state);
            params.uiColor = static_cast<ID3D12Resource *>(ffxParams->currentUI.resource);
            params.uiColorState = ConvertFfxResourceState(ffxParams->currentUI.state);
            params.outputColor = static_cast<ID3D12Resource *>(ffxParams->outputSwapChainBuffer.resource);
            params.outputColorState = ConvertFfxResourceState(ffxParams->outputSwapChainBuffer.state);
            params.isGeneratedFrame = ffxParams->isGeneratedFrame;
            params.frameID = ffxParams->frameID;
            self->userPresentCallback(params, self->userPresentCallbackCtx);
        }

        return FFX_API_RETURN_OK;
    }
public:
    Context(Device *device) {
        assert(device != nullptr);

        this->device = device;
        fgContext = nullptr;
        fgFilled = false;
        userPresentCallback = nullptr;
        userPresentCallbackCtx = nullptr;
        initialized = (device->getD3D12Device() != nullptr);
    }

    void setPresentCallback(FrameGenPresentCallback callback, void *userCtx) {
        userPresentCallback = callback;
        userPresentCallbackCtx = userCtx;
    }

    ~Context() {
        release();
    }

    bool set(int displayWidth, int displayHeight, int renderWidth, int renderHeight) {
        if (!initialized) {
            return false;
        }

        release();

        ffx::CreateBackendDX12Desc backendDesc{};
        backendDesc.device = device->getD3D12Device();

        ffx::CreateContextDescFrameGeneration createDesc{};
        createDesc.flags = 0;
        createDesc.displaySize = { (uint32_t)(displayWidth), (uint32_t)(displayHeight) };
        createDesc.maxRenderSize = { (uint32_t)(renderWidth), (uint32_t)(renderHeight) };
        createDesc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

        ffx::ReturnCode retCode = ffx::CreateContext(fgContext, nullptr, createDesc, backendDesc);
        if (retCode != ffx::ReturnCode::Ok) {
            RT64_LOG_PRINTF("ffx::CreateContext (FrameGeneration) failed: %d\n", (uint32_t)(retCode));
            return false;
        }

        fgFilled = true;

        return true;
    }

    void release() {
        if (fgFilled) {
            device->waitForGPU();

            ffx::DestroyContext(fgContext);
            fgFilled = false;
        }
    }

    bool isInitialized() const {
        return initialized && fgFilled;
    }

    void configure(void *swapChain, bool enabled, uint64_t frameID) {
        if (!fgFilled) {
            return;
        }

        ffx::ConfigureDescFrameGeneration configDesc{};
        configDesc.swapChain = swapChain;
        configDesc.frameGenerationEnabled = enabled;
        configDesc.allowAsyncWorkloads = false;
        configDesc.frameID = frameID;

        if ((userPresentCallback != nullptr) && enabled) {
            configDesc.presentCallback = &Context::presentCallbackTrampoline;
            configDesc.presentCallbackUserContext = this;
        }

        ffx::ReturnCode retCode = ffx::Configure(fgContext, configDesc);
        if (retCode != ffx::ReturnCode::Ok) {
            RT64_LOG_PRINTF("ffx::Configure (FrameGeneration) failed: %d\n", (uint32_t)(retCode));
        }
    }

    void dispatchPrepare(ID3D12Resource *depth, ID3D12Resource *motionVectors, int renderWidth, int renderHeight,
        float jitterX, float jitterY, float deltaTimeMs, float nearPlane, float farPlane, float fovYRadians,
        const RT64_VECTOR3 &cameraPosition, const RT64_VECTOR3 &cameraUp, const RT64_VECTOR3 &cameraRight,
        const RT64_VECTOR3 &cameraForward, uint64_t frameID, bool reset)
    {
        if (!fgFilled) {
            return;
        }

        ffx::DispatchDescFrameGenerationPrepareV2 prepareDesc{};
        prepareDesc.frameID = frameID;
        prepareDesc.flags = 0;
        prepareDesc.commandList = device->getD3D12CommandList();
        prepareDesc.renderSize = { (uint32_t)(renderWidth), (uint32_t)(renderHeight) };
        prepareDesc.jitterOffset = { jitterX, jitterY };
        prepareDesc.motionVectorScale = { 1.0f, 1.0f };
        prepareDesc.frameTimeDelta = deltaTimeMs;
        prepareDesc.reset = reset;
        prepareDesc.cameraNear = nearPlane;
        prepareDesc.cameraFar = farPlane;
        prepareDesc.cameraFovAngleVertical = fovYRadians;
        prepareDesc.viewSpaceToMetersFactor = 1.0f;
        prepareDesc.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_PIXEL_READ);
        prepareDesc.motionVectors = ffxApiGetResourceDX12(motionVectors, FFX_API_RESOURCE_STATE_PIXEL_READ);
        prepareDesc.cameraPosition[0] = cameraPosition.x;
        prepareDesc.cameraPosition[1] = cameraPosition.y;
        prepareDesc.cameraPosition[2] = cameraPosition.z;
        prepareDesc.cameraUp[0] = cameraUp.x;
        prepareDesc.cameraUp[1] = cameraUp.y;
        prepareDesc.cameraUp[2] = cameraUp.z;
        prepareDesc.cameraRight[0] = cameraRight.x;
        prepareDesc.cameraRight[1] = cameraRight.y;
        prepareDesc.cameraRight[2] = cameraRight.z;
        prepareDesc.cameraForward[0] = cameraForward.x;
        prepareDesc.cameraForward[1] = cameraForward.y;
        prepareDesc.cameraForward[2] = cameraForward.z;

        ffx::ReturnCode retCode = ffx::Dispatch(fgContext, prepareDesc);
        if (retCode != ffx::ReturnCode::Ok) {
            RT64_LOG_PRINTF("ffx::Dispatch (FrameGenerationPrepare) failed: %d\n", (uint32_t)(retCode));
        }
    }

    void dispatchGeneration(ID3D12GraphicsCommandList *commandList, ID3D12Resource *presentColor, ID3D12Resource *output,
        int displayWidth, int displayHeight, uint64_t frameID, bool reset)
    {
        if (!fgFilled) {
            return;
        }

        ffx::DispatchDescFrameGeneration dispatchDesc{};
        dispatchDesc.commandList = commandList;
        dispatchDesc.presentColor = ffxApiGetResourceDX12(presentColor, FFX_API_RESOURCE_STATE_PRESENT);
        dispatchDesc.outputs[0] = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatchDesc.numGeneratedFrames = 1;
        dispatchDesc.reset = reset;
        dispatchDesc.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatchDesc.minMaxLuminance[0] = 0.0f;
        dispatchDesc.minMaxLuminance[1] = 0.0f;
        dispatchDesc.generationRect = { 0, 0, displayWidth, displayHeight };
        dispatchDesc.frameID = frameID;

        ffx::ReturnCode retCode = ffx::Dispatch(fgContext, dispatchDesc);
        if (retCode != ffx::ReturnCode::Ok) {
            RT64_LOG_PRINTF("ffx::Dispatch (FrameGeneration) failed: %d\n", (uint32_t)(retCode));
        }
    }
};

// FrameGen

RT64::FrameGen::FrameGen(Device *device) {
    ctx = new Context(device);
}

RT64::FrameGen::~FrameGen() {
    delete ctx;
}

bool RT64::FrameGen::set(int displayWidth, int displayHeight, int renderWidth, int renderHeight) {
    return ctx->set(displayWidth, displayHeight, renderWidth, renderHeight);
}

void RT64::FrameGen::release() {
    ctx->release();
}

bool RT64::FrameGen::isInitialized() const {
    return ctx->isInitialized();
}

void RT64::FrameGen::configure(void *swapChain, bool enabled, uint64_t frameID) {
    ctx->configure(swapChain, enabled, frameID);
}

void RT64::FrameGen::setPresentCallback(FrameGenPresentCallback callback, void *userCtx) {
    ctx->setPresentCallback(callback, userCtx);
}

void RT64::FrameGen::dispatchPrepare(ID3D12Resource *depth, ID3D12Resource *motionVectors, int renderWidth, int renderHeight,
    float jitterX, float jitterY, float deltaTimeMs, float nearPlane, float farPlane, float fovYRadians,
    const RT64_VECTOR3 &cameraPosition, const RT64_VECTOR3 &cameraUp, const RT64_VECTOR3 &cameraRight,
    const RT64_VECTOR3 &cameraForward, uint64_t frameID, bool reset)
{
    ctx->dispatchPrepare(depth, motionVectors, renderWidth, renderHeight, jitterX, jitterY, deltaTimeMs, nearPlane, farPlane,
        fovYRadians, cameraPosition, cameraUp, cameraRight, cameraForward, frameID, reset);
}

void RT64::FrameGen::dispatchGeneration(ID3D12GraphicsCommandList *commandList, ID3D12Resource *presentColor, ID3D12Resource *output,
    int displayWidth, int displayHeight, uint64_t frameID, bool reset)
{
    ctx->dispatchGeneration(commandList, presentColor, output, displayWidth, displayHeight, frameID, reset);
}

#endif
