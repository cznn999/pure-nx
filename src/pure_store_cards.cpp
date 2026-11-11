// pure-nx (2026-11-07, as sa2-nx's src/cpu/sa2_hot_guest.cpp): whether the generated code marks the store cards
// (tools/store_cards.py ran, from tools/post_codegen_pure.py, and REX_STORE_CARDS_OFF is not set). Renut proves its
// cached vertex, index and texture copies by the cards only then (banjo_renut_store_cards); without them it hashes
// every copy whole at its first use after each kick (banjo_renut_exact_streams). The generated code's precompiled
// header holds the cards' macros (REX_CARD) once the tool has run.
#include "../generated/default/pure_pch.h"

#if defined(REX_CARD) && !defined(REX_STORE_CARDS_OFF)
extern "C" const bool g_banjo_store_cards = true;
#else
extern "C" const bool g_banjo_store_cards = false;
#endif
