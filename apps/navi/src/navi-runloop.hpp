#pragma once

namespace navi {

// Creates MapLibre's main-thread RunLoop once per process and attaches it to the default GMainContext.
void ensure_main_run_loop();

} // namespace navi
