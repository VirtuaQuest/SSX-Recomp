#pragma once

// Guest address of a kernel variable export, created on first use. Returns 0 for unknown variables.
uint32_t GetKernelVariable(std::string_view name);

// Starts the thread that keeps KeTimeStampBundle current.
void StartTimeStampBundleUpdates();
