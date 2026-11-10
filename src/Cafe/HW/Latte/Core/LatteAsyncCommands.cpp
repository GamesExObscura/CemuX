#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/Madden13AtlasReadback.h"
#include "Cafe/HW/Latte/Core/WindWakerDepthPeek.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include <memory>

void LatteThread_Exit();

SlimRWLock swl_gpuAsyncCommands;

struct DepthPeekRequest
{
	uint32 physicalAddress;
	uint32 width;
	uint32 height;
	std::vector<WindWakerDepthPeek::Point> points;
	std::atomic_bool finished{false};
	bool success{false};
};

typedef struct  
{
	uint32 type;
	std::shared_ptr<DepthPeekRequest> depthPeek;
	union
	{
		struct
		{
			MPTR physAddr;
			MPTR mipAddr;
			uint32 swizzle;
			sint32 format;
			sint32 width;
			sint32 height;
			sint32 depth;
			uint32 pitch;
			uint32 slice;
			sint32 dim;
			Latte::E_HWTILEMODE tilemode;
			sint32 aa;
			sint32 level;
		}forceTextureReadback;

		struct
		{
			uint64 shaderBaseHash; 
			uint64 shaderAuxHash; 
			LatteConst::ShaderType shaderType;
		}deleteShader;

		struct
		{
			LatteSurfaceCopyParam src;
			LatteSurfaceCopyParam dst;
			LatteSurfaceCopyRect rect;
		}textureCopy;
		MPTR smashPortraitSource;
	};
}LatteAsyncCommand_t;

#define ASYNC_CMD_FORCE_TEXTURE_READBACK		1
#define ASYNC_CMD_DELETE_SHADER					2
#define ASYNC_CMD_TEXTURE_COPY					3
#define ASYNC_CMD_DEPTH_PEEK                     4
#define ASYNC_CMD_SMASH_PORTRAIT                 5

std::queue<LatteAsyncCommand_t> LatteAsyncCommandQueue;

static void ReadbackSmashPortraitCPUSource(MPTR physicalAddress)
{
	std::vector<LatteTexture*> textures;
	LatteTC_LookupTexturesByPhysAddr(physicalAddress, textures);
	LatteTexture* selected = nullptr;
	for (auto* texture : textures)
		if (texture->physAddress == physicalAddress && texture->width == 320 && texture->height == 320 &&
			texture->pitch == 320 && texture->depth == 1 && texture->dim == Latte::E_DIM::DIM_2D && !texture->isDepth &&
			texture->baseView && texture->baseView->firstMip == 0 && texture->baseView->firstSlice == 0 &&
			texture->format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM &&
			!texture->overwriteInfo.hasResolutionOverwrite &&
			(texture->isUpdatedOnGPU || texture->reloadFromDynamicTextures) &&
			(!selected || texture->lastUpdateEventCounter > selected->lastUpdateEventCounter))
			selected = texture;
	if (!selected)
		return;
	LatteTexture_UpdateDataToLatest(selected);
	LatteTextureReadback_StartTransfer(selected->baseView);
	LatteTextureReadback_UpdateFinishedTransfers(true);
}

void LatteAsyncCommands_queueSmashPortraitReadback(MPTR physicalAddress)
{
	LatteAsyncCommand_t command{};
	command.type = ASYNC_CMD_SMASH_PORTRAIT;
	command.smashPortraitSource = physicalAddress;
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(command);
	swl_gpuAsyncCommands.UnlockWrite();
}

bool WindWakerDepthPeek::ReadPixels(uint32 physicalAddress, uint32 width, uint32 height, std::span<Point> points)
{
	if (!width || !height || width > 4096 || height > 4096 || points.empty() || points.size() > 64)
		return false;
	auto request = std::make_shared<DepthPeekRequest>();
	request->physicalAddress = physicalAddress;
	request->width = width;
	request->height = height;
	request->points.assign(points.begin(), points.end());
	LatteAsyncCommand_t command{};
	command.type = ASYNC_CMD_DEPTH_PEEK;
	command.depthPeek = request;
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(command);
	swl_gpuAsyncCommands.UnlockWrite();
	while (!request->finished.load(std::memory_order_acquire))
	{
		if (Latte_GetStopSignal())
			return false;
		std::this_thread::yield();
	}
	if (request->success)
		std::copy(request->points.begin(), request->points.end(), points.begin());
	return request->success;
}

static void ReadDepthPeek(DepthPeekRequest& request)
{
	std::vector<LatteTexture*> textures;
	LatteTC_LookupTexturesByPhysAddr(request.physicalAddress, textures);
	LatteTexture* selected = nullptr;
	for (auto* texture : textures)
		if (texture->isDepth && texture->format == Latte::E_GX2SURFFMT::R32_FLOAT &&
			texture->width == request.width && texture->height == request.height && texture->depth == 1 &&
			!texture->overwriteInfo.hasResolutionOverwrite &&
			(!selected || texture->lastUpdateEventCounter > selected->lastUpdateEventCounter))
			selected = texture;
	if (selected)
	{
		LatteTexture_UpdateDataToLatest(selected);
		std::unique_ptr<LatteTextureReadbackInfo> readback(g_renderer->texture_createReadback(selected->baseView));
		readback->StartTransfer();
		readback->ForceFinish();
		while (!readback->IsFinished())
			std::this_thread::yield();
		if (const uint8* data = readback->GetData())
		{
			for (auto& point : request.points)
			{
				const uint32 x = WindWakerDepthPeek::Pixel(point.x, request.width, 640);
				const uint32 y = WindWakerDepthPeek::Pixel(point.y, request.height, 480);
				float depth;
				std::memcpy(&depth, data + (y * request.width + x) * sizeof(float), sizeof(float));
				point.depth = WindWakerDepthPeek::Depth24(depth);
			}
			readback->ReleaseData();
			request.success = true;
		}
	}
	request.finished.store(true, std::memory_order_release);
}

void LatteAsyncCommands_queueForceTextureReadback(MPTR physAddr, MPTR mipAddr, uint32 swizzle, sint32 format, sint32 width, sint32 height, sint32 depth, uint32 pitch, uint32 slice, sint32 dim, Latte::E_HWTILEMODE tilemode, sint32 aa, sint32 level)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_FORCE_TEXTURE_READBACK;
	
	asyncCommand.forceTextureReadback.physAddr = physAddr;
	asyncCommand.forceTextureReadback.mipAddr = mipAddr;
	asyncCommand.forceTextureReadback.swizzle = swizzle;
	asyncCommand.forceTextureReadback.format = format;
	asyncCommand.forceTextureReadback.width = width;
	asyncCommand.forceTextureReadback.height = height;
	asyncCommand.forceTextureReadback.depth = depth;
	asyncCommand.forceTextureReadback.pitch = pitch;
	asyncCommand.forceTextureReadback.slice = slice;
	asyncCommand.forceTextureReadback.dim = dim;
	asyncCommand.forceTextureReadback.tilemode = tilemode;
	asyncCommand.forceTextureReadback.aa = aa;
	asyncCommand.forceTextureReadback.level = level;
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommands_queueDeleteShader(uint64 shaderBaseHash, uint64 shaderAuxHash, LatteConst::ShaderType shaderType)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_DELETE_SHADER;

	asyncCommand.deleteShader.shaderBaseHash = shaderBaseHash;
	asyncCommand.deleteShader.shaderAuxHash = shaderAuxHash;
	asyncCommand.deleteShader.shaderType = shaderType;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_TEXTURE_COPY;
	asyncCommand.textureCopy.src = src;
	asyncCommand.textureCopy.dst = dst;
	asyncCommand.textureCopy.rect = rect;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommands_waitUntilAllProcessed()
{
	while (LatteAsyncCommandQueue.empty() == false)
	{
		_mm_pause();
	}
}

/*
 * Called by the GPU command processor frequently
 */
void LatteAsyncCommands_checkAndExecute()
{
	// quick check if queue is empty (requires no lock)
	if (Latte_GetStopSignal())
		LatteThread_Exit();
	if (LatteAsyncCommandQueue.empty())
		return;
	swl_gpuAsyncCommands.LockWrite();
	while (LatteAsyncCommandQueue.empty() == false)
	{
		// get first command in queue
		LatteAsyncCommand_t asyncCommand = LatteAsyncCommandQueue.front();
		swl_gpuAsyncCommands.UnlockWrite();
		if (asyncCommand.type == ASYNC_CMD_DEPTH_PEEK)
		{
			ReadDepthPeek(*asyncCommand.depthPeek);
		}
		else if (asyncCommand.type == ASYNC_CMD_SMASH_PORTRAIT)
		{
			ReadbackSmashPortraitCPUSource(asyncCommand.smashPortraitSource);
		}
		else if (asyncCommand.type == ASYNC_CMD_FORCE_TEXTURE_READBACK)
		{
			const auto& request = asyncCommand.forceTextureReadback;
			const bool maddenAtlas = UseMadden13AtlasReadback(request.width, request.height,
				static_cast<Latte::E_GX2SURFFMT>(request.format)) && request.slice == 0;
			cemu_assert_debug(maddenAtlas || request.level == 0);
			LatteTextureView* textureView = LatteTC_GetTextureSliceViewOrTryCreate(request.physAddr, request.mipAddr,
				static_cast<Latte::E_GX2SURFFMT>(request.format), request.tilemode, request.width, request.height,
				request.depth, request.pitch, maddenAtlas ? request.swizzle : 0, request.slice, request.level);
			if (textureView != nullptr)
			{
				LatteTexture_UpdateDataToLatest(textureView->baseTexture);
				// start transfer
				LatteTextureReadback_StartTransfer(textureView);
				// wait until finished
				LatteTextureReadback_UpdateFinishedTransfers(true);
			}
			else
			{
				cemuLog_logDebug(LogType::Force, "Texture not found for readback");
			}
		}
		else if (asyncCommand.type == ASYNC_CMD_DELETE_SHADER)
		{
			LatteSHRC_RemoveFromCacheByHash(asyncCommand.deleteShader.shaderBaseHash, asyncCommand.deleteShader.shaderAuxHash, asyncCommand.deleteShader.shaderType);
		}
		else if (asyncCommand.type == ASYNC_CMD_TEXTURE_COPY)
		{
			LatteSurfaceCopy_copySurfaceNew(asyncCommand.textureCopy.src, asyncCommand.textureCopy.dst, asyncCommand.textureCopy.rect);
		}
		else
		{
			cemu_assert_unimplemented();
		}
		swl_gpuAsyncCommands.LockWrite();
		LatteAsyncCommandQueue.pop();
	}
	swl_gpuAsyncCommands.UnlockWrite();
}
