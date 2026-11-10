#pragma once

#include "Backend.h"
#include <atomic>

namespace nsyshid
{
	class GuitarHeroLiveDevice final : public Device
	{
	public:
		GuitarHeroLiveDevice();
		bool Open() override;
		void Close() override;
		bool IsOpened() override;
		ReadResult Read(ReadMessage* message) override;
		WriteResult Write(WriteMessage* message) override;
		bool GetDescriptor(uint8 type, uint8 index, uint16 lang, uint8* output, uint32 length) override;
		bool SetIdle(uint8 index, uint8 report, uint8 duration) override;
		bool SetProtocol(uint8 index, uint8 protocol) override;
		bool SetReport(ReportMessage* message) override;

	private:
		std::atomic<bool> m_open{false};
	};
}
