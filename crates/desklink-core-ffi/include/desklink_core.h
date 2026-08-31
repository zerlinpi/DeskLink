#ifndef DESKLINK_CORE_H
#define DESKLINK_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DESKLINK_CORE_ABI_VERSION 1u
#define DESKLINK_CORE_COMMAND_CAPACITY 8u

#define DESKLINK_CORE_STATUS_OK 0
#define DESKLINK_CORE_STATUS_INVALID_ARGUMENT 1
#define DESKLINK_CORE_STATUS_INVALID_TRANSITION 2
#define DESKLINK_CORE_STATUS_STALE_EVENT 3
#define DESKLINK_CORE_STATUS_PANIC 4

#define DESKLINK_CORE_EVENT_START 1u
#define DESKLINK_CORE_EVENT_SIGNAL_CONNECTED 2u
#define DESKLINK_CORE_EVENT_AUTHENTICATION_ACCEPTED 3u
#define DESKLINK_CORE_EVENT_PEER_CONNECTED 4u
#define DESKLINK_CORE_EVENT_PEER_REPLACED 5u
#define DESKLINK_CORE_EVENT_CONTROL_OPENED 6u
#define DESKLINK_CORE_EVENT_CONTROL_CLOSED 7u
#define DESKLINK_CORE_EVENT_POINTER_OPENED 8u
#define DESKLINK_CORE_EVENT_POINTER_CLOSED 9u
#define DESKLINK_CORE_EVENT_OPERATION_STARTED 10u
#define DESKLINK_CORE_EVENT_OPERATION_TIMED_OUT 11u
#define DESKLINK_CORE_EVENT_CLOSE_REQUESTED 12u
#define DESKLINK_CORE_EVENT_CLOSED 13u

#define DESKLINK_CORE_COMMAND_BEGIN_SIGNALING 1u
#define DESKLINK_CORE_COMMAND_BEGIN_AUTHENTICATION 2u
#define DESKLINK_CORE_COMMAND_BEGIN_NEGOTIATION 3u
#define DESKLINK_CORE_COMMAND_SESSION_CONNECTED 4u
#define DESKLINK_CORE_COMMAND_BEGIN_CLOSE 5u
#define DESKLINK_CORE_COMMAND_SESSION_CLOSED 6u

typedef struct DeskLinkCoreHandle DeskLinkCoreHandle;

typedef struct DeskLinkCoreEvent {
    uint32_t abi_version;
    uint32_t kind;
    uint64_t session_generation;
    uint64_t peer_generation;
    uint64_t control_generation;
    uint64_t pointer_generation;
    uint64_t operation_generation;
} DeskLinkCoreEvent;

typedef struct DeskLinkCoreCommandBuffer {
    uint32_t len;
    uint32_t commands[DESKLINK_CORE_COMMAND_CAPACITY];
} DeskLinkCoreCommandBuffer;

int32_t desklink_core_create(DeskLinkCoreHandle **out_handle);
int32_t desklink_core_destroy(DeskLinkCoreHandle *handle);
int32_t desklink_core_apply(
    DeskLinkCoreHandle *handle,
    const DeskLinkCoreEvent *event,
    DeskLinkCoreCommandBuffer *commands);
int32_t desklink_core_command_buffer_clear(DeskLinkCoreCommandBuffer *commands);

#ifdef __cplusplus
}
#endif

#endif
