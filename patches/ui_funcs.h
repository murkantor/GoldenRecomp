#pragma once
// Game-side UI event contract for recompui. RecompFrontend includes this
// header from the consuming project (see the "TODO: Forced game includes"
// in recompui/src/api/ui_api_events.cpp) so games can extend the UI event
// ABI. The canonical C structs live in recompui's own event_structs.h;
// GoldenEye-specific mod-UI additions go here when we need them.
#include "recompui/event_structs.h"
