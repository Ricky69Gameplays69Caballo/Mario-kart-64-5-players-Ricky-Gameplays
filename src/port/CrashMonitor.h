#pragma once

#ifdef _WIN32
// The monitor runs in a separate copy of the executable, before game startup.
int CrashMonitor_Run(unsigned long processId, const char* logDirectory);
void CrashMonitor_SetEnabled(bool enabled);
void CrashMonitor_Stop();
const char* CrashMonitor_Status();
#endif
