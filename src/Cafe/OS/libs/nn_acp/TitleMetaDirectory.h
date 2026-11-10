#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <fmt/format.h>

namespace nn::acp::TitleMetaDirectory
{
	enum class Result : std::int32_t
	{
		Success = 0,
		InvalidParameter = -201,
		DeviceNotFound = -503,
		TitleNotFound = -504,
	};

	// These are metadata storage-device IDs, not the legacy save-device enum.
	enum class Device : std::uint32_t { Auto = 1, Odd = 2, Mlc = 3, Usb = 4 };

	inline std::string FormatPath(std::uint64_t titleId, Device device)
	{
		const auto high = static_cast<std::uint32_t>(titleId >> 32);
		const auto low = static_cast<std::uint32_t>(titleId);
		const auto type = high & 0xff;
		return fmt::format("/vol/{}/{}/title/{:08x}/{:08x}/meta",
			device == Device::Usb ? "storage_usb01" : "storage_mlc01",
			type == 0x10 || type == 0x30 ? "sys" : "usr", high, low);
	}

	template<class DirectoryExists>
	Result Resolve(std::uint64_t titleId, std::uint32_t deviceId, char* output,
		std::size_t capacity, DirectoryExists&& exists)
	{
		if (!output || !capacity || deviceId < 1 || deviceId > 4)
			return Result::InvalidParameter;
		if (!titleId)
			return Result::TitleNotFound;
		const auto device = static_cast<Device>(deviceId);
		if (device == Device::Odd)
			return Result::DeviceNotFound; // Cemu currently exposes game installations through virtual MLC.
		auto path = FormatPath(titleId, device);
		if (device == Device::Auto && !exists(path.c_str()))
		{
			path = FormatPath(titleId, Device::Usb);
			if (!exists(path.c_str()))
				return Result::TitleNotFound;
		}
		else if (device == Device::Usb && !exists(path.c_str()))
		{
			return exists("/vol/storage_usb01") ? Result::TitleNotFound : Result::DeviceNotFound;
		}
		// Preserve established MLC path construction without adding a new existence requirement.
		if (path.size() >= capacity)
			return Result::InvalidParameter;
		std::memcpy(output, path.c_str(), path.size() + 1);
		return Result::Success;
	}
}
