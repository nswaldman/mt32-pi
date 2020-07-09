//
// hd44780.cpp
//
// mt32-pi - A bare-metal Roland MT-32 emulator for Raspberry Pi
// Copyright (C) 2020  Dale Whinham <daleyo@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <cmath>

#include <circle/logger.h>
#include <circle/sched/scheduler.h>

#include "hd44780.h"

// Custom characters for drawing bar graphs
// Empty (0x20) and full blocks (0xFF) already exist in the character set
const u8 CHD44780::CustomCharData[][8] =
{
	{
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b11111
	},
	{
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b11111,
		0b11111
	},
	{
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b11111,
		0b11111,
		0b11111
	},
	{
		0b00000,
		0b00000,
		0b00000,
		0b00000,
		0b11111,
		0b11111,
		0b11111,
		0b11111
	},
	{
		0b00000,
		0b00000,
		0b00000,
		0b11111,
		0b11111,
		0b11111,
		0b11111,
		0b11111
	},
	{
		0b00000,
		0b00000,
		0b11111,
		0b11111,
		0b11111,
		0b11111,
		0b11111,
		0b11111
	},
	{
		0b00000,
		0b11111,
		0b11111,
		0b11111,
		0b11111,
		0b11111,
		0b11111,
		0b11111
	}
};

// Use ASCII space for empty bar, custom chars for 1-7 rows, 0xFF for full bar
const char CHD44780::BarChars[] = { ' ', '\x1', '\x2', '\x3', '\x4', '\x5', '\x6', '\x7', '\xff' };

CHD44780::CHD44780(u8 pRows, u8 pColumns)
	: mScheduler(CScheduler::Get()),
	  mRows(pRows),
	  mColumns(pColumns),

	  mRS(GPIO_PIN_RS, GPIOModeOutput),
	  mRW(GPIO_PIN_RW, GPIOModeOutput),
	  mEN(GPIO_PIN_EN, GPIOModeOutput),
	  mD4(GPIO_PIN_D4, GPIOModeOutput),
	  mD5(GPIO_PIN_D5, GPIOModeOutput),
  	  mD6(GPIO_PIN_D6, GPIOModeOutput),
	  mD7(GPIO_PIN_D7, GPIOModeOutput),

	  mMessageFlag(false),
	  mPartLevels{0}
{
}

void CHD44780::WriteNybble(u8 pNybble)
{
	mD4.Write((pNybble >> 0) & 1);
	mD5.Write((pNybble >> 1) & 1);
	mD6.Write((pNybble >> 2) & 1);
	mD7.Write((pNybble >> 3) & 1);

	// Toggle enable
	mEN.Write(HIGH);
	mScheduler->usSleep(5);
	mEN.Write(LOW);
	mScheduler->usSleep(100);
}

void CHD44780::WriteByte(u8 pByte)
{
	WriteNybble(pByte >> 4);
	WriteNybble(pByte);
}

void CHD44780::WriteCommand(u8 pByte)
{
	// RS = LOW for command mode
	mRS.Write(LOW);
	WriteByte(pByte);
}

void CHD44780::WriteData(u8 pByte)
{
	// RS = HIGH for data mode
	mRS.Write(HIGH);
	WriteByte(pByte);
}

void CHD44780::WriteData(const u8* pBytes, size_t pSize)
{
	// RS = HIGH for data mode
	mRS.Write(HIGH);
	for (size_t i = 0; i < pSize; ++i)
		WriteByte(pBytes[i]);
}

void CHD44780::SetCustomChar(u8 pIndex, const u8 pCharData[8])
{
	assert(pIndex < 8);
	WriteCommand(0x40 | (pIndex << 3));

	for (u8 i = 0; i < 8; ++i)
		WriteData(pCharData[i]);
}

bool CHD44780::Initialize()
{
	assert(mScheduler != nullptr);

	// Bring all pins low
	mRS.Write(LOW);
	mRW.Write(LOW);
	mEN.Write(LOW);
	mD4.Write(LOW);
	mD5.Write(LOW);
	mD6.Write(LOW);
	mD7.Write(LOW);

	// Give the LCD some time to start up
	mScheduler->MsSleep(50);

	// The following algorithm ensures the LCD is in the correct mode no matter what state it's currently in:
	// https://en.wikipedia.org/wiki/Hitachi_HD44780_LCD_controller#Mode_selection
	WriteNybble(0b0011);
	mScheduler->MsSleep(50);
	WriteNybble(0b0011);
	mScheduler->MsSleep(50);
	WriteNybble(0b0011);
	mScheduler->MsSleep(50);

	// Switch to 4-bit mode
	WriteNybble(0b0010);
	mScheduler->MsSleep(50);

	// Turn off
	WriteCommand(0b1000);

	// Clear display
	WriteCommand(0b0001);
	mScheduler->MsSleep(50);

	// Home cursor
	WriteCommand(0b0010);
	mScheduler->MsSleep(2);

	// Function set (4-bit, 2-line)
	WriteCommand(0b101000);

	// Set entry
	WriteCommand(0b0110);

	// Set custom characters
	for (size_t i = 0; i < sizeof(CustomCharData) / sizeof(*CustomCharData); ++i)
		SetCustomChar(i + 1, CustomCharData[i]);

	// Turn on
	WriteCommand(0b1100);

	return true;
}

void CHD44780::Print(const char* pText, u8 pCursorX, u8 pCursorY, bool pClearLine, bool pImmediate)
{
	static u8 rowOffset[] = { 0, u8(0x40), mColumns, u8(0x40 + mColumns) };
	WriteCommand(0x80 | rowOffset[pCursorY] + pCursorX);

	const char* p = pText;
	while (*p)
		WriteData(*p++);

	if (pClearLine)
	{
		while ((p++ - pText) < (mColumns - pCursorX) - 1)
			WriteData(' ');
	}
}

void CHD44780::Clear()
{
	WriteCommand(0b0001);
	mScheduler->MsSleep(50);
}

void CHD44780::SetMessage(const char* pMessage)
{
	strncpy(mMessageText, pMessage, sizeof(mMessageText));
	mMessageFlag = true;
}

void CHD44780::ClearMessage()
{
	mMessageFlag = false;
}


void CHD44780::DrawStatusLine(const CMT32SynthBase* pSynth, u8 pRow)
{
	u32 partStates = pSynth->GetPartStates();
	char buf[TextBufferLength + 1];

	// First 5 parts
	for (u8 i = 0; i < 5; ++i)
	{
		bool state = (partStates >> i) & 1;
		buf[i * 2] = state ? '\xff' : ('1' + i);
		buf[i * 2 + 1] = ' ';
	}

	// Rhythm
	buf[10] = (partStates >> 8) ? '\xff' : 'R';
	buf[11] = ' ';

	// Volume
	sprintf(buf + 12, "|vol:%3d", pSynth->GetMasterVolume());
	Print(buf, 0, pRow);
}

void CHD44780::UpdatePartLevels(const CMT32SynthBase* pSynth)
{
	u32 partStates = pSynth->GetPartStates();
	for (u8 i = 0; i < 9; ++i)
	{
		if ((partStates >> i) & 1)
			mPartLevels[i] = floor(VelocityScale * pSynth->GetVelocityForPart(i)) + 0.5f;
		else if (mPartLevels[i] > 0)
			--mPartLevels[i];
	}
}

void CHD44780::DrawPartLevelsSingle(u8 pRow)
{
	char lineBuf[18 + 1];

	for (u8 i = 0; i < 9; ++i)
	{
		lineBuf[i * 2] = BarChars[mPartLevels[i] / 2];
		lineBuf[i * 2 + 1] = ' ';
	}

	lineBuf[18] = '\0';

	Print(lineBuf, 0, pRow, true);
}

void CHD44780::DrawPartLevelsDouble(u8 pFirstRow)
{
	char line1Buf[18 + 1];
	char line2Buf[18 + 1];

	for (u8 i = 0; i < 9; ++i)
	{
		if (mPartLevels[i] > 8)
		{
			line1Buf[i * 2] = BarChars[mPartLevels[i] - 8];
			line2Buf[i * 2] = BarChars[8];
		}
		else
		{
			line1Buf[i * 2] = BarChars[0];
			line2Buf[i * 2] = BarChars[mPartLevels[i]];
		}

		line1Buf[i * 2 + 1] = line2Buf[i * 2 + 1] = ' ';
	}

	line1Buf[18] = line2Buf[18] = '\0';

	Print(line1Buf, 0, pFirstRow, true);
	Print(line2Buf, 0, pFirstRow + 1, true);
}

void CHD44780::Update(CMT32SynthBase* pSynth)
{
	if (!pSynth)
		return;

	UpdatePartLevels(pSynth);

	if (mRows == 2)
	{
		if (mMessageFlag)
			Print(mMessageText, 0, 0, true);
		else
			DrawStatusLine(pSynth, 0);
		DrawPartLevelsSingle(1);
	}
	else if (mRows == 4)
	{
		if (mMessageFlag)
			Print(mMessageText, 0, 0, false);
		else
			Print(" ", 0, 0, true);
		DrawStatusLine(pSynth, 1);
		DrawPartLevelsDouble(2);
	}
}
