#include "http.h"

namespace net {

const char* errorName(Error e) {
    switch (e) {
        case Error::None:        return "ok";
        case Error::Unreachable: return "unreachable";
        case Error::Timeout:     return "timeout";
        case Error::Tls:         return "tls";
        case Error::Protocol:    return "protocol";
        case Error::Aborted:     return "aborted";
    }
    return "?";
}

} // namespace net
