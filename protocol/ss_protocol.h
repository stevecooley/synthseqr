/*
 * Synthseqr v3 — link protocol vocabulary.
 *
 * Shared verbatim by the Feather (SAMD51) and the CYD (ESP32). This header is
 * the single definition of the wire contract; neither side may define message
 * types locally.
 *
 * The Feather is authoritative for all sequencer state. The CYD renders,
 * stores, and proposes. That asymmetry is visible here: the CYD sends EVENT
 * messages (requests), the Feather sends STATE messages (facts).
 */

#ifndef SS_PROTOCOL_H
#define SS_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to message layout or semantics. Exchanged
 * in the HELLO handshake; a mismatch must fail loudly rather than degrade. The
 * two boards get flashed independently, so silent skew is the expensive case. */
#define SS_PROTOCOL_VERSION 2

/* Message type. High nibble is the class, low nibble the member. */
typedef enum {
    /* 0x0_ link management */
    SS_MSG_HELLO        = 0x00,  /* both  : version + capability exchange */
    SS_MSG_HELLO_ACK    = 0x01,  /* both  */
    SS_MSG_PING         = 0x02,  /* both  */
    SS_MSG_PONG         = 0x03,  /* both  */
    SS_MSG_NAK          = 0x04,  /* both  : payload[0] = ss_nak_reason */

    /* 0x1_ display, Feather -> CYD */
    SS_MSG_LCD_RAW      = 0x10,  /* phase 1: verbatim 16x2 backpack bytes */

    /* 0x2_ input events, CYD -> Feather (proposals, may be rejected) */
    SS_MSG_EVT_TOUCH    = 0x20,
    SS_MSG_EVT_CONTROL  = 0x21,  /* slider-mode select, transport, etc. */

    /* 0x3_ state broadcast, Feather -> CYD (authoritative) */
    SS_MSG_STATE_FULL   = 0x30,
    SS_MSG_STATE_DELTA  = 0x31,

    /* 0x4_ storage, chunked */
    SS_MSG_STORE_BEGIN  = 0x40,
    SS_MSG_STORE_CHUNK  = 0x41,
    SS_MSG_STORE_END    = 0x42,
    SS_MSG_LOAD_REQ     = 0x43,
    SS_MSG_LOAD_CHUNK   = 0x44,

    /* 0x5_ MIDI relay for BLE. Note data only — clock never crosses this link,
     * because the UART hop plus the ESP32 BLE stack would wreck its timing. */
    SS_MSG_MIDI_IN      = 0x50,  /* CYD -> Feather */
    SS_MSG_MIDI_OUT     = 0x51,  /* Feather -> CYD */

    /* 0x6_ soft power. S1 is a maintained rocker on the Feather's MISO; there
     * is no actual power cutoff, so "off" is a cooperative shutdown into a
     * low-power state. The Feather owns the switch and drives the whole
     * sequence — the CYD only ever answers. */
    SS_MSG_PWR_SHUTDOWN = 0x60,  /* F->C : persist everything now */
    SS_MSG_PWR_SAVED    = 0x61,  /* C->F : payload[0] = ss_pwr_save_status */
    SS_MSG_PWR_SLEEP    = 0x62,  /* F->C : enter low power now */
    SS_MSG_PWR_WAKE     = 0x63,  /* F->C : rocker back on, resume */
    SS_MSG_PWR_ABORT    = 0x64   /* F->C : rocker returned mid-shutdown */
} ss_msg_type_t;

typedef enum {
    SS_PWR_SAVE_OK      = 0x00,
    SS_PWR_SAVE_FAILED  = 0x01,  /* the Feather powers down anyway; it cannot
                                  * stay up waiting on storage it does not own */
    SS_PWR_SAVE_NOTHING = 0x02   /* nothing dirty */
} ss_pwr_save_status_t;

typedef enum {
    SS_NAK_BAD_VERSION  = 0x01,
    SS_NAK_BAD_TYPE     = 0x02,
    SS_NAK_BAD_LENGTH   = 0x03,
    SS_NAK_BUSY         = 0x04,
    SS_NAK_REJECTED     = 0x05   /* well-formed, but the Feather declined it */
} ss_nak_reason_t;

/* Storage chunk payload size. Deliberately small: a pattern save must not
 * monopolise the link or delay a display update. */
#define SS_STORE_CHUNK_BYTES 128

#ifdef __cplusplus
}
#endif

#endif /* SS_PROTOCOL_H */
