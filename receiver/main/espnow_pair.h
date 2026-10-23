#ifndef ESPNOW_PAIR_H
#define ESPNOW_PAIR_H

// "Add machine": while unpaired and not on the transmitter's Wi-Fi, broadcasts
// PAIR_REQ on channels 1-13 every ~3 s and applies the PAIR_CFG reply.
void espnow_pair_start(void);

#endif // ESPNOW_PAIR_H
