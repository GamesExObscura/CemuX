#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>

namespace nn::act
{
	inline bool ReadAccountImage(const std::filesystem::path& mlc, std::uint32_t persistentId,
		std::uint32_t type, std::span<std::uint8_t> output, std::uint32_t& size)
	{
		size = 0;
		if (!persistentId || type > 8) return false;
		char account[9], filename[16];
		std::snprintf(account, sizeof(account), "%08x", persistentId);
		std::snprintf(filename, sizeof(filename), "miiimg%02u.dat", type);
		std::ifstream file(mlc / "usr/save/system/act" / account / filename, std::ios::binary | std::ios::ate);
		if (!file) return false;
		const auto length = file.tellg();
		if (length <= 0 || length > 1024 * 1024) return false;
		size = static_cast<std::uint32_t>(length);
		if (output.size() < size) return false;
		file.seekg(0);
		return static_cast<bool>(file.read(reinterpret_cast<char*>(output.data()), size));
	}
}
