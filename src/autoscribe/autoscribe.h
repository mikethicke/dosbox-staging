// SPDX-FileCopyrightText:  2026 Autoscribe contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Autoscribe link: exposes the rendered frame, emulated memory and an input
// channel to a companion process over a Unix socket plus shared memory.
// See protocol/PROTOCOL.md in the Autoscribe repository. Inactive unless
// `--autoscribe-socket <path>` is given or AUTOSCRIBE_SOCKET is set.

#ifndef DOSBOX_AUTOSCRIBE_H
#define DOSBOX_AUTOSCRIBE_H

#include <cstdint>
#include <string>

void AUTOSCRIBE_Init(const std::string& socket_path);
void AUTOSCRIBE_Shutdown();
bool AUTOSCRIBE_IsActive();

// Called by the renderer after each complete frame has been latched.
void AUTOSCRIBE_OnFrame();

// Called by DOS as a program uses a file on a drive, so the companion can
// tell a saved game being written from one being loaded. Opening a file is
// reported at once, since a game may hold a save open long after it has
// loaded it. Reads and writes are counted against the file's handle; closing
// it reports the totals with the file's size and name, even when nothing was
// read or written, so every file reported open is reported closed.
void AUTOSCRIBE_OnFileOpen(uint32_t handle, uint8_t drive, const char* name);
void AUTOSCRIBE_OnFileRead(uint32_t handle, uint32_t bytes);
void AUTOSCRIBE_OnFileWrite(uint32_t handle, uint32_t bytes);
void AUTOSCRIBE_OnFileClose(uint32_t handle, uint8_t drive, const char* name,
                            uint32_t size);

#endif
