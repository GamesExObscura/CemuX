#include "GuitarHeroLive.h"
#include "GuitarHeroLiveReport.h"
#include "input/InputManager.h"

namespace nsyshid
{
	namespace
	{
		// Match libusb: configuration words are parsed to Cafe order; other descriptors stay raw.
		constexpr uint8 kDeviceDescriptor[] = {
			18, 1, 0x00, 0x02, 0, 0, 0, 32, 0xba, 0x12, 0x4b, 0x07, 0x00, 0x01, 0, 0, 0, 1
		};
		constexpr uint8 kReportDescriptor[] = {
			0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00,
			0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x20, 0x09, 0x01,
			0x81, 0x02, 0x95, 0x20, 0x09, 0x02, 0x91, 0x02, 0xc0
		};
		constexpr uint8 kConfigDescriptor[] = {
			9, 2, 0, 41, 1, 1, 0, 0x80, 0x96,
			9, 4, 0, 0, 2, 3, 0, 0, 0,
			9, 0x21, 0x01, 0x11, 0, 1, 0x22, 0, sizeof(kReportDescriptor),
			7, 5, 0x81, 3, 0, 32, 1,
			7, 5, 0x01, 3, 0, 32, 1
		};
		constexpr uint8 kHidDescriptor[] = {
			9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(kReportDescriptor), 0
		};

		ghl::Input SampleInput()
		{
			ghl::Input input;
			if (auto guitar = InputManager::instance().get_guitar_controller())
			{
				const auto state = guitar->ReadState();
				input.white = state.white;
				input.black = state.black;
				input.strumUp = state.strumUp;
				input.strumDown = state.strumDown;
				input.left = state.left;
				input.right = state.right;
				input.start = state.start;
				input.heroPower = state.heroPower;
				input.tv = state.tv;
				input.whammy = state.whammy;
				input.tilt = state.tilt;
				return input;
			}
			// Preserve existing setups until the user creates a dedicated guitar profile.
			if (auto pad = InputManager::instance().get_vpad_controller(0))
			{
				const auto mapped = pad->ReadMappedState();
				const auto down = [&](VPADController::ButtonId id) { return mapped[id] > 0.0f; };
				input.white = {down(VPADController::kButtonId_A), down(VPADController::kButtonId_B), down(VPADController::kButtonId_R)};
				input.black = {down(VPADController::kButtonId_X), down(VPADController::kButtonId_Y), down(VPADController::kButtonId_L)};
				input.strumUp = down(VPADController::kButtonId_Up);
				input.strumDown = down(VPADController::kButtonId_Down);
				input.left = down(VPADController::kButtonId_Left);
				input.right = down(VPADController::kButtonId_Right);
				input.start = down(VPADController::kButtonId_Plus);
				input.heroPower = down(VPADController::kButtonId_Minus);
				input.tv = down(VPADController::kButtonId_StickR);
				float whammy = std::clamp(mapped[VPADController::kButtonId_StickR_Down] -
					mapped[VPADController::kButtonId_StickR_Up], 0.0f, 1.0f);
				if (down(VPADController::kButtonId_ZL)) whammy = 1.0f;
				input.whammy = static_cast<uint8>(128.0f * (1.0f - whammy));
				input.tilt = down(VPADController::kButtonId_ZR) ? 0xff : 0x80;
			}
			return input;
		}
	}

	GuitarHeroLiveDevice::GuitarHeroLiveDevice() : Device(ghl::VendorId, ghl::ProductId, 0, 0, 0)
	{
		m_maxPacketSizeRX = m_maxPacketSizeTX = 32;
	}

	bool GuitarHeroLiveDevice::Open() { m_open = true; return true; }
	void GuitarHeroLiveDevice::Close() { m_open = false; }
	bool GuitarHeroLiveDevice::IsOpened() { return m_open.load(); }

	Device::ReadResult GuitarHeroLiveDevice::Read(ReadMessage* message)
	{
		message->bytesRead = 0;
		if (!IsOpened() || !message->data || message->length == 0)
			return ReadResult::Error;
		const auto report = ghl::Encode(SampleInput());
		const auto count = std::min<size_t>(message->length, report.size());
		memcpy(message->data, report.data(), count);
		message->bytesRead = static_cast<sint32>(count);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return ReadResult::Success;
	}

	Device::WriteResult GuitarHeroLiveDevice::Write(WriteMessage* message)
	{
		message->bytesWritten = 0;
		if (!IsOpened() || !message->data || message->length > 32)
			return WriteResult::Error;
		// Receiver LED/keepalive commands have no physical side effects on a virtual guitar.
		message->bytesWritten = message->length;
		return WriteResult::Success;
	}

	bool GuitarHeroLiveDevice::GetDescriptor(uint8 type, uint8 index, uint16 lang, uint8* output, uint32 length)
	{
		if (!IsOpened() || !output || index != 0) return false;
		const uint8* data = nullptr;
		size_t size = 0;
		switch (type)
		{
		case 1: data = kDeviceDescriptor; size = sizeof(kDeviceDescriptor); break;
		case 2: data = kConfigDescriptor; size = sizeof(kConfigDescriptor); break;
		case 0x21: data = kHidDescriptor; size = sizeof(kHidDescriptor); break;
		case 0x22: data = kReportDescriptor; size = sizeof(kReportDescriptor); break;
		default: return false;
		}
		memset(output, 0, length);
		memcpy(output, data, std::min<size_t>(length, size));
		return true;
	}

	bool GuitarHeroLiveDevice::SetIdle(uint8 index, uint8 report, uint8 duration)
	{
		return IsOpened() && index == 0 && report == 0;
	}

	bool GuitarHeroLiveDevice::SetProtocol(uint8 index, uint8 protocol)
	{
		return IsOpened() && index == 0 && protocol <= 1;
	}

	bool GuitarHeroLiveDevice::SetReport(ReportMessage* message)
	{
		return IsOpened() && message->data && message->length <= 32 &&
			(message->reportType == 2 || message->reportType == 3);
	}
}
