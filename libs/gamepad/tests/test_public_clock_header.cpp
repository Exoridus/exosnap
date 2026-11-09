#include "qpc_100ns.h"

static_assert(exosnap::engine::QpcTicksTo100ns(0, 10'000'000) == 0);
static_assert(exosnap::engine::QpcTicksTo100ns(4'089'402'061'399, 10'000'000) == 4'089'402'061'399);
static_assert(exosnap::engine::QpcTicksTo100ns(1'843'200, 3'686'400) == 5'000'000);
static_assert(exosnap::engine::QpcTicksTo100ns(500, 0) == 0);
