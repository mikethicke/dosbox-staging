// SPDX-FileCopyrightText:  2026 Autoscribe contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "autoscribe.h"

void AUTOSCRIBE_Init(const std::string&) {}
void AUTOSCRIBE_Shutdown() {}
bool AUTOSCRIBE_IsActive() { return false; }
void AUTOSCRIBE_OnFrame() {}
void AUTOSCRIBE_OnFileOpen(uint8_t, const char*) {}
void AUTOSCRIBE_OnFileRead(uint32_t, uint32_t) {}
void AUTOSCRIBE_OnFileWrite(uint32_t, uint32_t) {}
void AUTOSCRIBE_OnFileClose(uint32_t, uint8_t, const char*, uint32_t) {}
