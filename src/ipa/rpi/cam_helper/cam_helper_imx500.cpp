/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi Ltd
 *
 * cam_helper_imx500.cpp - camera helper for imx500 sensor
 */

#include <algorithm>
#include <assert.h>
#include <cmath>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <libcamera/base/log.h>
#include <libcamera/base/span.h>
#include <libcamera/control_ids.h>

#include "cam_helper.h"
#include "md_parser.h"

using namespace RPiController;
using namespace libcamera;
using libcamera::utils::Duration;

namespace libcamera {
LOG_DECLARE_CATEGORY(IPARPI)
}

/*
 * We care about two gain registers and a pair of exposure registers. Their
 * I2C addresses from the Sony IMX500 datasheet:
 */
constexpr uint32_t expHiReg = 0x0202;
constexpr uint32_t expLoReg = 0x0203;
constexpr uint32_t gainHiReg = 0x0204;
constexpr uint32_t gainLoReg = 0x0205;
constexpr uint32_t frameLengthHiReg = 0x0340;
constexpr uint32_t frameLengthLoReg = 0x0341;
constexpr uint32_t lineLengthHiReg = 0x0342;
constexpr uint32_t lineLengthLoReg = 0x0343;
constexpr uint32_t temperatureReg = 0x013a;
constexpr std::initializer_list<uint32_t> registerList =
	{ expHiReg, expLoReg, gainHiReg, gainLoReg, frameLengthHiReg, frameLengthLoReg,
	  lineLengthHiReg, lineLengthLoReg, temperatureReg };

class CamHelperImx500 : public CamHelper
{
public:
	CamHelperImx500();
	uint32_t gainCode(double gain) const override;
	double gain(uint32_t gainCode) const override;
	void prepare(libcamera::Span<const uint8_t> buffer, Metadata &metadata,
                     ControlList &libcameraMetadata) override;
	std::pair<uint32_t, uint32_t> getBlanking(Duration &exposure, Duration minFrameDuration,
						  Duration maxFrameDuration) const override;
	void getDelays(int &exposureDelay, int &gainDelay,
		       int &vblankDelay, int &hblankDelay) const override;
	bool sensorEmbeddedDataPresent() const override;

private:
	/*
	 * Smallest difference between the frame length and integration time,
	 * in units of lines.
	 */
	static constexpr int frameIntegrationDiff = 22;
	/* Maximum frame length allowable for long exposure calculations. */
	static constexpr int frameLengthMax = 0xffdc;
	/* Largest long exposure scale factor given as a left shift on the frame length. */
	static constexpr int longExposureShiftMax = 7;

	void populateMetadata(const MdParser::RegisterMap &registers,
			      Metadata &metadata) const override;
};

CamHelperImx500::CamHelperImx500()
	: CamHelper(std::make_unique<MdParserSmia>(registerList), frameIntegrationDiff)
{
}

uint32_t CamHelperImx500::gainCode(double gain) const
{
	return static_cast<uint32_t>(1024 - 1024 / gain);
}

double CamHelperImx500::gain(uint32_t gainCode) const
{
	return 1024.0 / (1024 - gainCode);
}

void CamHelperImx500::prepare(libcamera::Span<const uint8_t> buffer, Metadata &metadata,
                              [[maybe_unused]] ControlList &libcameraMetadata)
{
	MdParser::RegisterMap registers;
	DeviceStatus deviceStatus;

	if (metadata.get("device.status", deviceStatus)) {
		LOG(IPARPI, Error) << "DeviceStatus not found from DelayedControls";
		return;
	}

	parseEmbeddedData(buffer, metadata);

	/*
	 * The DeviceStatus struct is first populated with values obtained from
	 * DelayedControls. If this reports frame length is > frameLengthMax,
	 * it means we are using a long exposure mode. Since the long exposure
	 * scale factor is not returned back through embedded data, we must rely
	 * on the existing exposure lines and frame length values returned by
	 * DelayedControls.
	 *
	 * Otherwise, all values are updated with what is reported in the
	 * embedded data.
	 */
	if (deviceStatus.frameLength > frameLengthMax) {
		DeviceStatus parsedDeviceStatus;

		metadata.get("device.status", parsedDeviceStatus);
		parsedDeviceStatus.shutterSpeed = deviceStatus.shutterSpeed;
		parsedDeviceStatus.frameLength = deviceStatus.frameLength;
		metadata.set("device.status", parsedDeviceStatus);

		LOG(IPARPI, Debug) << "Metadata updated for long exposure: "
				   << parsedDeviceStatus;
	}

	/* Inference data comes after 2 lines of embedded data. */
	size_t bytesPerLine = (((mode_.width * mode_.bitdepth) >> 3) + 15) & ~15;
	if (buffer.size() > 2 * bytesPerLine) {
		Span<const uint8_t> outputTensor(buffer.data() + 2 * bytesPerLine,
						 buffer.size() - 2 * bytesPerLine);
		libcameraMetadata.set(libcamera::controls::rpi::Imx500OutputTensor, outputTensor);
		static int frames = 0;

		if (frames++ == 100)
		{ 
			FILE *f = fopen("output_full.dat", "wb");
			fwrite(buffer.data(), buffer.size(), 1, f);
			fclose(f);

			f = fopen("output.dat", "wb");
			fwrite(buffer.data() + 2 * bytesPerLine, buffer.size() - 2 * bytesPerLine, 1, f);
			fclose(f);

		}

		struct DnnHeader {
			uint8_t frameValid;
			uint8_t frameCount;
			uint16_t maxLineLen;
			uint16_t apParamSize;
			uint16_t networkId;
			uint8_t tensorType;
		};

		const uint8_t *src = buffer.data() + 2 * bytesPerLine;
		DnnHeader dnnHeader;

		dnnHeader.frameValid = *(src++);
		dnnHeader.frameCount = *(src++);

		dnnHeader.maxLineLen =  (*(src+1) << 8 |*(src));
		src+=2;
		dnnHeader.apParamSize = (*(src+1) << 8 |*(src));
		src+=2;
		dnnHeader.networkId = (*(src+1) << 8 |*(src));
		src+=2;
		dnnHeader.tensorType = *(src++);
		(void)dnnHeader;
	}
}

std::pair<uint32_t, uint32_t> CamHelperImx500::getBlanking(Duration &exposure,
							   Duration minFrameDuration,
							   Duration maxFrameDuration) const
{
	uint32_t frameLength, exposureLines;
	unsigned int shift = 0;

	auto [vblank, hblank] = CamHelper::getBlanking(exposure, minFrameDuration,
						       maxFrameDuration);

	frameLength = mode_.height + vblank;
	Duration lineLength = hblankToLineLength(hblank);

	/*
	 * Check if the frame length calculated needs to be setup for long
	 * exposure mode. This will require us to use a long exposure scale
	 * factor provided by a shift operation in the sensor.
	 */
	while (frameLength > frameLengthMax) {
		if (++shift > longExposureShiftMax) {
			shift = longExposureShiftMax;
			frameLength = frameLengthMax;
			break;
		}
		frameLength >>= 1;
	}

	if (shift) {
		/* Account for any rounding in the scaled frame length value. */
		frameLength <<= shift;
		exposureLines = CamHelperImx500::exposureLines(exposure, lineLength);
		exposureLines = std::min(exposureLines, frameLength - frameIntegrationDiff);
		exposure = CamHelperImx500::exposure(exposureLines, lineLength);
	}

	return { frameLength - mode_.height, hblank };
}

void CamHelperImx500::getDelays(int &exposureDelay, int &gainDelay,
				int &vblankDelay, int &hblankDelay) const
{
	exposureDelay = 2;
	gainDelay = 2;
	vblankDelay = 3;
	hblankDelay = 3;
}

bool CamHelperImx500::sensorEmbeddedDataPresent() const
{
	return true;
}

void CamHelperImx500::populateMetadata(const MdParser::RegisterMap &registers,
				       Metadata &metadata) const
{
	DeviceStatus deviceStatus;

	deviceStatus.lineLength = lineLengthPckToDuration(registers.at(lineLengthHiReg) * 256 +
							  registers.at(lineLengthLoReg));
	deviceStatus.shutterSpeed = exposure(registers.at(expHiReg) * 256 + registers.at(expLoReg),
					     deviceStatus.lineLength);
	deviceStatus.analogueGain = gain(registers.at(gainHiReg) * 256 + registers.at(gainLoReg));
	deviceStatus.frameLength = registers.at(frameLengthHiReg) * 256 + registers.at(frameLengthLoReg);
	deviceStatus.sensorTemperature = std::clamp<int8_t>(registers.at(temperatureReg), -20, 80);

	metadata.set("device.status", deviceStatus);
}

static CamHelper *create()
{
	return new CamHelperImx500();
}

static RegisterCamHelper reg_imx500("imx500", &create);
