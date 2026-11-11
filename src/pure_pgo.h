// pure-nx (2026-11-07): profile collection of the PGO-instrumented Switch build (PURE_PGO=generate), see pure_pgo.cpp.
// From renut-nx's app/src/banjo_pgo.h (sa2-nx's sa2_pgo.h, same project family). In every other build these do nothing.

#pragma once

namespace pure::pgo {

#if defined(PURE_PGO_GENERATE)
// Picks this session's file (pgo/pure_pgo_<N>.stream next to the NRO) and starts the dumps every minute.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
#else
inline void Start() {}
inline void DumpNow() {}
#endif

}  // namespace pure::pgo
