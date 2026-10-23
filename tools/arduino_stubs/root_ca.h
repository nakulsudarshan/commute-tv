// Stub. See README.md -- syntax only. The real one comes from
// tools/fetch_root_ca.py and holds actual certificates.
#pragma once
namespace secrets {
static const char kRootCa_stub[] = "-----BEGIN CERTIFICATE-----\n";
#define COMMUTE_TV_ROOT_CAS { "example.com", secrets::kRootCa_stub }
} // namespace secrets
