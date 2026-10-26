#pragma once

// Notification ids used by the runtime.
constexpr uint32_t XN_SYS_UI = 0x00000009;
constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;
constexpr uint32_t XN_SYS_INPUTDEVICESCHANGED = 0x00000012;
constexpr uint32_t XN_LIVE_CONNECTIONCHANGED = 0x02000001;

// Queues a notification for every listener interested in its area.
void PostNotification(uint32_t id, uint32_t parameter);
