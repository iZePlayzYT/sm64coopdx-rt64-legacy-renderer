//
// RT64
//

#pragma once

#include "rt64_common.h"

namespace RT64 {
	class Device;

	struct FrameGenPresentParams {
		void *device;
		void *commandList;
		ID3D12Resource *backBufferColor;
		D3D12_RESOURCE_STATES backBufferColorState;
		ID3D12Resource *uiColor;
		D3D12_RESOURCE_STATES uiColorState;
		ID3D12Resource *outputColor;
		D3D12_RESOURCE_STATES outputColorState;
		bool isGeneratedFrame;
		uint64_t frameID;
	};

	typedef void (*FrameGenPresentCallback)(const FrameGenPresentParams &params, void *userCtx);

	class FrameGen {
	private:
		class Context;
		Context *ctx;
	public:
		FrameGen(Device *device);
		~FrameGen();
		bool set(int displayWidth, int displayHeight, int renderWidth, int renderHeight);
		void release();
		bool isInitialized() const;
		void configure(void *swapChain, bool enabled, uint64_t frameID);
		void setPresentCallback(FrameGenPresentCallback callback, void *userCtx);
		void dispatchPrepare(ID3D12Resource *depth, ID3D12Resource *motionVectors, int renderWidth, int renderHeight,
			float jitterX, float jitterY, float deltaTimeMs, float nearPlane, float farPlane, float fovYRadians,
			const RT64_VECTOR3 &cameraPosition, const RT64_VECTOR3 &cameraUp, const RT64_VECTOR3 &cameraRight,
			const RT64_VECTOR3 &cameraForward, uint64_t frameID, bool reset);
		void dispatchGeneration(ID3D12GraphicsCommandList *commandList, ID3D12Resource *presentColor, ID3D12Resource *output,
			int displayWidth, int displayHeight, uint64_t frameID, bool reset);
	};
};
