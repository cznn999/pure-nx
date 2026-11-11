// The addresses both renderers hook (banjo-nx's tools/renut_gpu_import.py generated this list for N&B). sa2-nx
// (2026-10-20): SA2's. dishonored-nx (2026-10-29): Dishonored's; its own hooks there (src/dishonored_me_hooks.cpp: the
// shader dump and the PM4-ring renderer's notes) call renut_native_sub_X when Renut's renderer is chosen.
// pure-nx (2026-11-06): Pure's: only the shader setup is shared, with the shader dump (src/pure_renut_hooks.cpp); Pure
// has no second renderer, so the draws are Renut's own hooks.
#pragma once
#include <rex/ppc.h>
extern "C" void renut_native_sub_829DD270(PPCContext& ctx, uint8_t* base);  // XGRegisterVertexShader
extern "C" void renut_native_sub_829DCF98(PPCContext& ctx, uint8_t* base);  // XGRegisterPixelShader
