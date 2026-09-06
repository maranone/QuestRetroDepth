#include "../snes/snes.hpp"

// Force <atomic> to be parsed at global scope before SPC_DSP.cpp's own
// #include <atomic> runs (it's textually included below from inside
// `namespace SNES {`, since sdsp.cpp opens that namespace before pulling
// SPC_DSP.cpp in). Without this, if <atomic> hasn't already been included
// earlier in this translation unit, its contents get nested under
// SNES::std instead of ::std, which current MSVC's <atomic> implementation
// doesn't tolerate (redefinition/syntax errors around _Atomic_storage).
#include <atomic>

#define DSP_CPP
namespace SNES {

DSP dsp;

#include "SPC_DSP.cpp"

void DSP::power()
{
  spc_dsp.init(smp.apuram);
  spc_dsp.reset();
  clock = 0;
}

void DSP::reset()
{
  spc_dsp.soft_reset();
  clock = 0;
}

static void from_dsp_to_state (uint8 **buf, void *var, size_t size)
{
  memcpy(*buf, var, size);
  *buf += size;
}

static void to_dsp_from_state (uint8 **buf, void *var, size_t size)
{
	memcpy(var, *buf, size);
	*buf += size;
}

void DSP::save_state (uint8 **ptr)
{
	spc_dsp.copy_state(ptr, from_dsp_to_state);
}

void DSP::load_state (uint8 **ptr)
{
	spc_dsp.copy_state(ptr, to_dsp_from_state);
}

DSP::DSP()
{
	clock = 0;
}

}
