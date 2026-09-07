#ifndef OD_SECURITY_H
#define OD_SECURITY_H
#include "protocol.h"
void od_security_reset(void);
int od_security_enabled(void);
void od_secure_command(struct od_session *s, uint8_t *b, size_t len, const uint8_t msd[16],
                       unsigned mtu, od_send_fn send, void *arg);
#endif
