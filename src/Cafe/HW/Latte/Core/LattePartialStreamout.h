#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>

// Tracks the dwords at streamout boundaries that cannot use a full 16-byte tag.
class LattePartialStreamout
{
public:
	template<typename WriteWord>
	void MarkEdges(uint32_t begin, uint32_t end, WriteWord writeWord)
	{
		m_words.erase(m_words.lower_bound(begin), m_words.lower_bound(end));
		for (uint32_t address = begin; address < end; address += 4)
		{
			if ((address & 15) == 0 && end - address >= 16)
			{
				address = end & ~15u;
				if (address == end)
					break;
			}
			const uint32_t marker = 0xF15ECAFEu ^ address;
			m_words[address] = marker;
			writeWord(address, marker);
		}
	}

	bool HasInRange(uint32_t begin, uint32_t end) const
	{
		const auto it = m_words.lower_bound(begin);
		return it != m_words.end() && it->first < end;
	}

	void CopyFrom(const LattePartialStreamout& source, uint32_t begin, uint32_t end)
	{
		if (this == &source)
			return;
		m_words.erase(m_words.lower_bound(begin), m_words.lower_bound(end));
		for (auto it = source.m_words.lower_bound(begin); it != source.m_words.end() && it->first < end; ++it)
			m_words[it->first] = it->second;
	}

	// Preserve tagged GPU words, but upload any words subsequently replaced by the CPU.
	template<typename Upload>
	bool FilterPage(uint32_t base, const uint8_t* bytes, uint32_t size, uint64_t fullTag0, uint64_t fullTag1, Upload upload)
	{
		bool hasStreamout = false;
		bool fullBlock = false;
		uint32_t runBegin = size;
		for (uint32_t offset = 0; offset < size; offset += 4)
		{
			if ((offset & 15) == 0)
			{
				uint64_t tags[2];
				std::memcpy(tags, bytes + offset, sizeof(tags));
				fullBlock = tags[0] == fullTag0 && tags[1] == fullTag1;
			}
			bool preserve = fullBlock;
			if (!preserve)
			{
				auto it = m_words.find(base + offset);
				if (it != m_words.end())
				{
					uint32_t word;
					std::memcpy(&word, bytes + offset, sizeof(word));
					preserve = word == it->second;
					if (!preserve)
						m_words.erase(it);
				}
			}
			if (preserve)
			{
				hasStreamout = true;
				if (runBegin != size)
					upload(runBegin, offset - runBegin);
				runBegin = size;
			}
			else if (runBegin == size)
				runBegin = offset;
		}
		if (runBegin != size)
			upload(runBegin, size - runBegin);
		return hasStreamout;
	}

private:
	std::map<uint32_t, uint32_t> m_words;
};
