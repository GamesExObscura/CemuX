#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <vector>

namespace CompatibilityDiagnostics
{
	struct MemoryRange
	{
		std::uint32_t address;
		std::uint32_t size;
	};

	inline std::string_view GetEnvironment(std::string_view name)
	{
		if (name == "CEMU_COMPAT_TRACE")
		{
			static const char* value = std::getenv("CEMU_COMPAT_TRACE");
			return value ? std::string_view{ value } : std::string_view{};
		}
		if (name == "CEMU_COMPAT_RESOURCE")
		{
			static const char* value = std::getenv("CEMU_COMPAT_RESOURCE");
			return value ? std::string_view{ value } : std::string_view{};
		}
		if (name == "CEMU_COMPAT_TEXTURE")
		{
			static const char* value = std::getenv("CEMU_COMPAT_TEXTURE");
			return value ? std::string_view{ value } : std::string_view{};
		}
		return {};
	}

	inline bool TokenEquals(std::string_view left, std::string_view right)
	{
		if (left.size() != right.size())
			return false;
		for (size_t i = 0; i < left.size(); ++i)
		{
			const char a = left[i] >= 'A' && left[i] <= 'Z' ? left[i] + ('a' - 'A') : left[i];
			const char b = right[i] >= 'A' && right[i] <= 'Z' ? right[i] + ('a' - 'A') : right[i];
			if (a != b)
				return false;
		}
		return true;
	}

	inline bool ContainsToken(std::string_view values, std::string_view expected)
	{
		while (!values.empty())
		{
			const size_t delimiter = values.find(',');
			auto token = values.substr(0, delimiter);
			while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
				token.remove_prefix(1);
			while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
				token.remove_suffix(1);
			if (TokenEquals(token, expected) || TokenEquals(token, "all"))
				return true;
			if (delimiter == std::string_view::npos)
				break;
			values.remove_prefix(delimiter + 1);
		}
		return false;
	}

	inline std::uint64_t GetTargetTitleId()
	{
		static const std::uint64_t titleId = []
		{
			const char* value = std::getenv("CEMU_COMPAT_TITLE_ID");
			if (!value || !*value)
				return std::uint64_t{ 0 };
			char* end = nullptr;
			const std::uint64_t parsed = std::strtoull(value, &end, 16);
			return end != value && *end == '\0' ? parsed : std::uint64_t{ 0 };
		}();
		return titleId;
	}

	inline bool IsChannelConfigured(std::string_view channel)
	{
		return GetTargetTitleId() != 0 && ContainsToken(GetEnvironment("CEMU_COMPAT_TRACE"), channel);
	}

	inline bool IsEnabled(std::string_view channel, std::uint64_t currentTitleId)
	{
		return currentTitleId == GetTargetTitleId() && IsChannelConfigured(channel);
	}

	inline std::int64_t GetInteger(const char* name, std::int64_t fallback)
	{
		const char* value = std::getenv(name);
		if (!value || !*value)
			return fallback;
		char* end = nullptr;
		const auto parsed = std::strtoll(value, &end, 10);
		return end != value && *end == '\0' ? parsed : fallback;
	}

	inline std::int64_t ElapsedMilliseconds()
	{
		static const auto startTime = std::chrono::steady_clock::now();
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - startTime).count();
	}

	inline bool MatchesResource(std::string_view path)
	{
		const auto filter = GetEnvironment("CEMU_COMPAT_RESOURCE");
		return !filter.empty() && path.find(filter) != std::string_view::npos;
	}

	inline bool ParseUnsigned(std::string_view value, std::uint32_t& result)
	{
		if (value.empty())
			return false;
		const auto parseResult = std::from_chars(value.data(), value.data() + value.size(), result);
		return parseResult.ec == std::errc{} && parseResult.ptr == value.data() + value.size();
	}

	inline bool MatchesTexture(std::uint32_t width, std::uint32_t height)
	{
		auto filters = GetEnvironment("CEMU_COMPAT_TEXTURE");
		while (!filters.empty())
		{
			const size_t delimiter = filters.find(',');
			auto filter = filters.substr(0, delimiter);
			while (!filter.empty() && (filter.front() == ' ' || filter.front() == '\t'))
				filter.remove_prefix(1);
			while (!filter.empty() && (filter.back() == ' ' || filter.back() == '\t'))
				filter.remove_suffix(1);

			const size_t separator = filter.find_first_of("xX");
			std::uint32_t filterWidth = 0;
			std::uint32_t filterHeight = 0;
			if (separator != std::string_view::npos &&
				ParseUnsigned(filter.substr(0, separator), filterWidth) &&
				ParseUnsigned(filter.substr(separator + 1), filterHeight) &&
				width == filterWidth && height == filterHeight)
			{
				return true;
			}

			if (delimiter == std::string_view::npos)
				break;
			filters.remove_prefix(delimiter + 1);
		}
		return false;
	}

	inline std::mutex s_trackedRangesMutex;
	inline std::vector<MemoryRange> s_trackedTextureRanges;

	inline void TrackTextureRange(std::uint32_t address, std::uint32_t size)
	{
		if (address == 0 || size == 0)
			return;
		std::scoped_lock lock(s_trackedRangesMutex);
		const auto duplicate = std::ranges::find_if(s_trackedTextureRanges,
			[address, size](const MemoryRange& range) { return range.address == address && range.size == size; });
		if (duplicate == s_trackedTextureRanges.end())
			s_trackedTextureRanges.push_back({ address, size });
	}

	inline bool OverlapsTrackedTexture(std::uint32_t address, std::uint32_t size)
	{
		if (address == 0 || size == 0)
			return false;
		const std::uint64_t end = std::uint64_t{ address } + size;
		std::scoped_lock lock(s_trackedRangesMutex);
		return std::ranges::any_of(s_trackedTextureRanges, [address, end](const MemoryRange& range)
		{
			const std::uint64_t rangeEnd = std::uint64_t{ range.address } + range.size;
			return std::uint64_t{ address } < rangeEnd && std::uint64_t{ range.address } < end;
		});
	}
}
