// Stop the D3D GPU-wait loop from pinning a host core.
//
// rex_sub_821E6AC8 is the body of the D3D library's "wait for the GPU" loop:
// it burns a few cycles (4 x 8 db16cyc), checks the device's GPU progress
// pointer (device+10896) against the last value it saw, and returns 1 (keep
// waiting) or, after 5000 ms without progress, calls the GPU-hang handler
// (rex_sub_821EFAF0) and returns 0. Its caller loops on it until the GPU
// catches up.
//
// On the 360 that spin is harmless. Here, whenever the host GPU is the
// bottleneck, the waiting guest thread spins at full clock for most of every
// frame - measured on Linux as ~24% of all process CPU in this one function,
// plus the clock and lock polling around it. On a laptop APU the CPU and iGPU
// share one power budget, so that spinning core takes power the GPU could use.
//
// The override runs the original check unchanged, then sleeps briefly before
// returning, so each "still waiting" iteration costs a short sleep instead of
// a busy spin. The GPU finishing is noticed at most ac6_gpu_wait_sleep_us late,
// negligible against a 16-33 ms frame. Return value and guest state are
// untouched. On Windows the default is a plain yield: its sleep granularity is
// too coarse for sub-millisecond sleeps without changing the timer resolution.

#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/ppc.h>

#if defined(_WIN32)
#define AC6_GPU_WAIT_SLEEP_DEFAULT 0
#else
#define AC6_GPU_WAIT_SLEEP_DEFAULT 100
#endif

REXCVAR_DEFINE_INT32(ac6_gpu_wait_sleep_us, AC6_GPU_WAIT_SLEEP_DEFAULT, "AC6/Performance",
                     "While the game waits for the GPU, sleep this many microseconds between "
                     "checks instead of spinning a CPU core (0 = just yield). Saves CPU power, "
                     "which laptop iGPUs can use for higher clocks.");

PPC_EXTERN_FUNC(__imp__rex_sub_821E6AC8);  // D3D GPU-wait loop body

PPC_FUNC_IMPL(rex_sub_821E6AC8) {
  PPC_FUNC_PROLOGUE();
  __imp__rex_sub_821E6AC8(ctx, base);

  const int32_t sleep_us = REXCVAR_GET(ac6_gpu_wait_sleep_us);
  if (sleep_us > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  } else {
    std::this_thread::yield();
  }
}
