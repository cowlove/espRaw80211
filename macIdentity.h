#pragma once
#include <stdint.h>

// Hardware protocol identities preserve ESP.getEfuseMac()'s little-endian
// integer representation. CSIM context identities are already network order.
// Conversion is its own inverse; do not migrate persisted/wire identities.
inline uint64_t protocolRadioMac(uint64_t value) {
#ifdef CSIM
    return value & 0xffffffffffffULL;
#else
    uint64_t converted = 0;
    for (unsigned i = 0; i < 6; ++i) {
        converted = (converted << 8) | (value & 0xff);
        value >>= 8;
    }
    return converted;
#endif
}
