#pragma once
#include "stdlib_registrar.h"

// Registers the global `task` object (spec/05-stdlib.md, task): spawn,
// spawn_worker, send, receive, self, pending.
void registerTaskAPI(StdlibRegistrar& reg);
