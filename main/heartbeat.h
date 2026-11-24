#pragma once

#include <stdatomic.h>
#include <stdint.h>

void heartbeat_start(const atomic_bool *connected, const atomic_bool *forwarding,
                     const atomic_uint *link_generation);
const char *heartbeat_status(void);
const char *device_alert_status(void);
int32_t heartbeat_last_success_seconds(void);
