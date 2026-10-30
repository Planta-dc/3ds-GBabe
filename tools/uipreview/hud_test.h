/**
 * The strip's inputs, stood in for the uipreview binaries.
 *
 * hud_draw() reads the emulator's state - emu_loaded(), emu_get_info(), g_cfg
 * and g_perf - and neither emu_gambatte.cpp nor config.c/main.c is linked
 * into these host binaries: they are the drawing code's test bench, not a
 * front-end. This header gives them the smallest honest stand-ins, a loaded
 * CGB cartridge and the perf numbers the caller sets, so the strip can be
 * drawn and checked on the host exactly as the 3DS draws it.
 *
 * Exactly one translation unit per binary includes this, so the definitions
 * below are not duplicated within a binary.
 *
 * NOT part of the 3DS build.
 */
#pragma once

#include "emu.h"
#include "gb3ds.h"

gbabe_config g_cfg;
gbabe_perf   g_perf;

static const emu_info g_test_cart = {
	"Pokemon Crystal", /* title    */
	true,              /* cgb      */
	5,                 /* mbc      */
	16,                /* rom_banks (2 Mbit) */
	8,                 /* ram_banks */
	0,                 /* auto_hash */
};

bool emu_loaded(void)
{
	return true;
}

const emu_info *emu_get_info(void)
{
	return &g_test_cart;
}
