#include "util/omp_threads.h"

#if defined(FOCAL_HAVE_OPENMP)
#include <omp.h>
#endif

namespace focal {

#if defined(FOCAL_HAVE_OPENMP)
ScopedOmpThreads::ScopedOmpThreads(int threads) : previous_(omp_get_max_threads()) { omp_set_num_threads(threads); }
ScopedOmpThreads::~ScopedOmpThreads() { omp_set_num_threads(previous_); }
#else
ScopedOmpThreads::ScopedOmpThreads(int) {}
ScopedOmpThreads::~ScopedOmpThreads() {}
#endif

} // namespace focal
