#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <utility>

namespace coreinit::ConsoleEula
{
	struct Settings
	{
		std::uint8_t agreement{};
		std::uint32_t version{};
	};

	class Store
	{
	public:
		explicit Store(std::filesystem::path path) : m_path(std::move(path)) {}
		std::optional<Settings> Read() const
		{
			std::scoped_lock lock(s_mutex);
			return ReadLocked();
		}
		bool Update(std::optional<std::uint8_t> agreement, std::optional<std::uint32_t> version) const
		{
			std::scoped_lock lock(s_mutex);
			auto value = ReadLocked();
			if (!value) return false;
			if (agreement) value->agreement = *agreement;
			if (version) value->version = *version;
			std::array<std::uint8_t, 12> bytes{'C','E','U','L',1,value->agreement,0,0};
			for (unsigned i=0; i<4; ++i) bytes[8+i] = static_cast<std::uint8_t>(value->version >> (24-8*i));
			std::error_code error;
			std::filesystem::create_directories(m_path.parent_path(), error);
			if (error) return false;
			auto temporary = m_path; temporary += ".tmp";
			{
				std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
				if (!file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()) || !file.flush()) return false;
				file.close(); if (!file) return false;
			}
			std::filesystem::rename(temporary, m_path, error);
			if (error) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
			return !error;
		}
	private:
		std::optional<Settings> ReadLocked() const
		{
			std::error_code error;
			const bool exists = std::filesystem::exists(m_path, error);
			if (error) return {};
			if (!exists) return Settings{};
			std::array<std::uint8_t, 12> bytes{};
			std::ifstream file(m_path, std::ios::binary);
			if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || file.peek() != std::char_traits<char>::eof()) return {};
			if (bytes[0]!='C' || bytes[1]!='E' || bytes[2]!='U' || bytes[3]!='L' || bytes[4]!=1 || bytes[6] || bytes[7]) return {};
			Settings result{bytes[5],0};
			for (unsigned i=0; i<4; ++i) result.version = (result.version << 8) | bytes[8+i];
			return result;
		}
		std::filesystem::path m_path;
		static inline std::mutex s_mutex;
	};
}
