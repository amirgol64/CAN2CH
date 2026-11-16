#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FW_VERSION "0.1.0"

#define CAN_FLAG_EXT 0x01
#define CAN_FLAG_RTR 0x02

#define CAN_STD_ID_MAX 0x7FFu
#define CAN_EXT_ID_MAX 0x1FFFFFFFu

typedef struct {
    uint32_t id;
    uint8_t flags;
    uint8_t dlc;
    uint8_t data[8];
} can_frame_t;

/* Approximate length of a frame on the wire, in bits (no stuffing). */
static inline uint32_t can_frame_bits(const can_frame_t *f)
{
    uint32_t bits = (f->flags & CAN_FLAG_EXT) ? 67 : 47;
    if (!(f->flags & CAN_FLAG_RTR)) {
        bits += 8u * (f->dlc > 8 ? 8 : f->dlc);
    }
    return bits;
}
