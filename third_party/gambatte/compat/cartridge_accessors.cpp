/**
 * The cartridge accessors Gambatte's libretro glue provides, without the
 * libretro glue.
 *
 * When the core is built with __LIBRETRO__ (which is how it is tested
 * upstream), Memory exposes savedata_ptr()/rtcdata_ptr() etc. through to
 * Cartridge, and the definitions live in libretro/cartridge_libretro.cpp next
 * to a lot of file-stream code GBabe does not want. These four functions are
 * that file's saving-related half, copied unchanged (GPL-2.0, see ../COPYING),
 * so the front-end can own the .sav file itself.
 *
 * Called from GB::savedata_ptr()/savedata_size()/rtcdata_ptr()/rtcdata_size(),
 * never from the emulation loop.
 */
#include "mem/cartridge.h"

/* Cartridge.cpp calls this when an MBC5 rumble cart writes its motor bit. The
 * libretro build forwards it to the front-end's rumble API; there is no rumble
 * on the console this port targets, so it is a no-op. */
void cartridge_set_rumble(unsigned active)
{
	(void)active;
}

namespace gambatte
{
	/* Same table as the core's own battery check (header byte 0x0147). */
	static bool hasBattery(unsigned char headerByte0x147)
	{
		switch (headerByte0x147)
		{
		case 0x03:
		case 0x06:
		case 0x09:
		case 0x0F:
		case 0x10:
		case 0x13:
		case 0x1B:
		case 0x1E:
		case 0xFE:
		case 0xFF:
			return true;
		default:
			return false;
		}
	}

	static bool hasRtc(unsigned char headerByte0x147)
	{
		switch (headerByte0x147)
		{
		case 0xFE: /* huc3 */
		case 0x0F:
		case 0x10:
			return true;
		default:
			return false;
		}
	}

	void *Cartridge::savedata_ptr()
	{
		/* Sachen MMC1 carts have no SRAM, and the byte at 0x0147 is the
		 * descrambled cartridge type, which has no Nintendo meaning. */
		if (isSachen_)
			return 0;
		if (hasBattery(memptrs_.romdata()[0x147]))
			return memptrs_.rambankdata();
		return 0;
	}

	unsigned Cartridge::savedata_size()
	{
		if (isSachen_)
			return 0;
		if (hasBattery(memptrs_.romdata()[0x147]))
			return (unsigned)(memptrs_.rambankdataend() - memptrs_.rambankdata());
		return 0;
	}

	void *Cartridge::rtcdata_ptr()
	{
		if (isSachen_)
			return 0;
		if (hasRtc(memptrs_.romdata()[0x147]))
		{
			if (isHuC3())
				return &huc3_.getBaseTime();
			return &rtc_.getBaseTime();
		}
		return 0;
	}

	unsigned Cartridge::rtcdata_size()
	{
		if (isSachen_)
			return 0;
		if (hasRtc(memptrs_.romdata()[0x147]))
		{
			if (isHuC3())
				return (unsigned)sizeof(huc3_.getBaseTime());
			return (unsigned)sizeof(rtc_.getBaseTime());
		}
		return 0;
	}
}
