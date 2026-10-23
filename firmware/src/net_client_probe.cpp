// Not part of the firmware image -- `make firmware` compiles it so that
// platform/net_client.h is parsed by the syntax gate rather than only by the
// two-minute PlatformIO build. build_src_filter excludes it from `pio run`.
#include "platform/net_client.h"

namespace {
[[maybe_unused]] void unused() {
    platform::NetOptions opt;
    platform::NetClient c(opt);
    (void)c.tlsConnections();
}
}  // namespace

#include "platform/rsa_signer.h"

namespace {
[[maybe_unused]] void unusedSigner() {
    platform::RsaSigner s("", 0);
    uint8_t out[256];
    (void)s.signRs256(nullptr, 0, out, sizeof out);
    (void)s.signatures();
}
}  // namespace

#include "platform/smalltv_link.h"

namespace {
[[maybe_unused]] void unusedTvLink() {
    platform::SmallTvApConfig c;
    platform::SmallTvLink l(c);
    (void)l.beginAp(6);
    (void)l.discover();
    (void)l.get("/v.json");
    l.poll();
}
}  // namespace

#include "platform/tv_internet.h"

namespace {
[[maybe_unused]] void unusedNat() { (void)platform::enableTvInternet(1); }
}  // namespace

#include "platform/sntp_server.h"

namespace {
[[maybe_unused]] void unusedSntp() {
    platform::SntpServer s;
    (void)s.begin();
    s.poll();
    (void)s.served();
}
}  // namespace
