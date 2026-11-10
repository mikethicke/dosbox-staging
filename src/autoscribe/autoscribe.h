// SPDX-FileCopyrightText:  2026 Autoscribe contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Autoscribe link: exposes the rendered frame, emulated memory and an input
// channel to a companion process over a Unix socket plus shared memory.
// See protocol/PROTOCOL.md in the Autoscribe repository. Inactive unless
// `--autoscribe-socket <path>` is given or AUTOSCRIBE_SOCKET is set.

#ifndef DOSBOX_AUTOSCRIBE_H
#define DOSBOX_AUTOSCRIBE_H

#include <string>

void AUTOSCRIBE_Init(const std::string& socket_path);
void AUTOSCRIBE_Shutdown();
bool AUTOSCRIBE_IsActive();

// Called by the renderer after each complete frame has been latched.
void AUTOSCRIBE_OnFrame();

#endif
