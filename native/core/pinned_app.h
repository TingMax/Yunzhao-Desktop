// A pinned bar entry as stored in settings.json ("pins", docs/SETTINGS-PROTOCOL.md).
//
// Upstream 3.0 moved PinnedApp into this file. Yunzhao keeps it in shell/task_model.h, next to
// TaskItem which uses it, so the twelve files that already include that header are untouched;
// this one forwards for the new callers that expect the upstream name.
#pragma once

#include "shell/task_model.h"
