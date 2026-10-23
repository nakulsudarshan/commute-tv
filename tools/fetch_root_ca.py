#!/usr/bin/env python3
"""
Build firmware/include/root_ca.h from YOUR machine's certificate store.

    python3 tools/fetch_root_ca.py
    python3 tools/fetch_root_ca.py --check      # verify without rewriting

WHY THIS IS NOT JUST `setInsecure()`.

A JWT signed with your Google service-account key goes over one of these
connections. Skipping verification hands that to whoever answers on port 443 --
on a university network you do not administer. The firmware refuses unverified
TLS by default for that reason; this is how you give it something to verify
against.

WHY IT RUNS ON YOUR MACHINE AND NOT IN THE CONTAINER.

The root certificate is normally NOT sent by the server -- the chain stops at
an intermediate, and the client is expected to already have the root. So this
asks each host what chain it presents, then finds the issuer of the topmost
certificate in your system's own CA bundle. That store is the thing your laptop
already trusts, which makes it the honest place to take it from, and it means
you can check every step yourself.

It needs `openssl` (already on Ubuntu) and nothing else.

WHEN TO RERUN IT: if a handshake starts failing on one host and the others are
fine, that host has rotated to a new root. Rerun, reflash. Roots are long-lived
-- this is a once-a-few-years event, not maintenance.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "firmware" / "include" / "root_ca.h"

# The four upstreams, and the host suffix each certificate is good for. Suffix
# rather than exact host because Google answers on several names.
HOSTS = [
    ("googleapis.com", "www.googleapis.com"),
    ("oauth2.googleapis.com", "oauth2.googleapis.com"),
    ("openrouteservice.org", "api.openrouteservice.org"),
    ("open-meteo.com", "api.open-meteo.com"),
    # AVV's host is NOT hard-coded here: the endpoint is covered by clause 7.a
    # of the Nutzungsvereinbarung and this file is not gitignored. It is read
    # from .env at run time instead -- see avv_from_env().
]

BUNDLES = [
    "/etc/ssl/certs/ca-certificates.crt",       # Debian/Ubuntu
    "/etc/pki/tls/certs/ca-bundle.crt",         # Fedora/RHEL
    "/etc/ssl/cert.pem",                        # macOS/BSD
]

PEM_RE = re.compile(
    r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", re.S)


def run(cmd, stdin=None):
    return subprocess.run(cmd, input=stdin, capture_output=True, text=True,
                          timeout=30)


def chain_for(host):
    """Every certificate the server sends, leaf first."""
    r = run(["openssl", "s_client", "-showcerts", "-servername", host,
             "-connect", f"{host}:443"], stdin="")
    if r.returncode != 0 and not r.stdout:
        raise RuntimeError(f"openssl could not reach {host}: {r.stderr.strip()[:200]}")
    certs = PEM_RE.findall(r.stdout)
    if not certs:
        raise RuntimeError(f"{host} presented no certificates")
    return certs


def subject_of(pem):
    r = run(["openssl", "x509", "-noout", "-subject", "-nameopt", "RFC2253"], stdin=pem)
    return r.stdout.strip().removeprefix("subject=").strip()


def issuer_of(pem):
    r = run(["openssl", "x509", "-noout", "-issuer", "-nameopt", "RFC2253"], stdin=pem)
    return r.stdout.strip().removeprefix("issuer=").strip()


def avv_hosts(env_path):
    """The AVV hosts to pin, read from where they actually live.

    Two mistakes led here and both are worth remembering.

    First I asked the user to paste the hostname on the command line; he pasted
    my placeholder and openssl went looking for a machine called
    "the-host-from-your-env". Then I had this read `AVV_BASE_URL` from .env --
    a key I had invented, which has never existed in .env.example. It printed a
    tidy, confident note about a setting that was never a setting.

    The endpoints are in src/hafas/hafas_request.cpp, as `kDemo` and `kProd`,
    and that file is the firmware's own source of truth. So read THEM. A tool
    that copies a constant is a tool that drifts from it.

    Both are pinned regardless of AVV_ENV: two certificates cost a couple of KB
    of flash, and switching demo->prod later should not need a reflash of the
    certificate store as well.
    """
    hosts = []

    src = pathlib.Path(__file__).resolve().parent.parent / "src" / "hafas" / "hafas_request.cpp"
    if src.exists():
        for m in re.finditer(r'k(?:Demo|Prod)\s*=\s*"([^"]+)"', src.read_text()):
            hosts.append(m.group(1))
    if not hosts:
        print(f"note: could not read the AVV endpoints from {src}", file=sys.stderr)

    # An explicit override in .env wins, matching hafas::Endpoint::baseUrlOverride.
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            line = line.strip()
            if line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k.strip() in ("AVV_BASE_URL", "AVV_BASE_URL_OVERRIDE"):
                v = v.strip().strip('"').strip("'")
                if v:
                    hosts.append(v)

    out = []
    for h in hosts:
        h = re.sub(r"^[a-zA-Z][a-zA-Z0-9+.-]*://", "", h)
        h = h.split("/")[0].split(":")[0]
        if h and h not in out:
            out.append(h)
    return out


def find_bundle():
    for b in BUNDLES:
        if pathlib.Path(b).exists():
            return pathlib.Path(b)
    return None


def load_bundle(path):
    """{subject: pem} for every root the machine trusts."""
    out = {}
    for pem in PEM_RE.findall(path.read_text()):
        try:
            out.setdefault(subject_of(pem), pem)
        except Exception:
            continue
    return out


def root_for(host, bundle):
    """The root to pin for `host`, taken from the local store.

    Order matters, and the first rule is the one this script originally missed.

    1. THE TOP CERTIFICATE'S OWN SUBJECT IS A ROOT WE TRUST. This is the
       cross-signing case and it is extremely common: Google presents GTS Root
       R4 *cross-signed by GlobalSign Root CA* for the benefit of old clients,
       so the chain's top looks like an intermediate. But GTS Root R4 is itself
       a self-signed root sitting in the store. Anchoring there is both correct
       and better than the alternative -- trust stores are actively retiring the
       old cross-signers, so pinning GlobalSign would break the day it goes.
    2. The chain stops at an intermediate whose issuer we trust. Ordinary case.
    3. Neither: refuse. Trusting a root because the server offered it is
       circular reasoning, and this whole file exists to avoid exactly that.
    """
    chain = chain_for(host)
    top = chain[-1]
    top_subject, top_issuer = subject_of(top), issuer_of(top)

    if top_subject in bundle:
        how = "cross-signed" if top_subject != top_issuer else "self-signed"
        return bundle[top_subject], top_subject, f"local store, {how}"

    if top_issuer in bundle:
        return bundle[top_issuer], top_issuer, "local store, issuer of the chain top"

    raise RuntimeError(
        f"{host}: nothing in your CA store anchors this chain.\n"
        f"  chain top subject: {top_subject}\n"
        f"  its issuer:        {top_issuer}\n"
        f"Neither is a root you trust. Try:  sudo apt install --reinstall ca-certificates\n"
        f"Refusing to trust a certificate merely because the server sent it.")


def as_c_string(pem):
    lines = [ln for ln in pem.strip().splitlines() if ln.strip()]
    return "\n".join(f'    "{ln}\\n"' for ln in lines)


def render(entries):
    L = [
        "// GENERATED by tools/fetch_root_ca.py -- do not edit by hand.",
        "//",
        "// Root certificates, taken from the CA store of the machine that ran the",
        "// script. The firmware REFUSES unverified TLS by default: a JWT signed with",
        "// a Google service-account key goes over these connections, and handing that",
        "// to whoever answers on port 443 is not a trade worth making.",
        "//",
        "// Rerun the script if one host starts failing its handshake while the others",
        "// are fine -- that host has rotated its root. Roots are long-lived; this is a",
        "// once-in-a-few-years event.",
        "#pragma once",
        "",
        "namespace secrets {",
        "",
    ]
    for host, suffix, subject, pem in entries:
        L.append(f"// {host}")
        L.append(f"//   anchored at: {subject}")
        L.append(f"static const char kRootCa_{ident(suffix)}[] =")
        L.append(as_c_string(pem) + ";")
        L.append("")
    L.append("// Suffix -> certificate, in the shape platform::NetOptions wants.")
    L.append("#define COMMUTE_TV_ROOT_CAS \\")
    rows = [f'    {{ "{suffix}", secrets::kRootCa_{ident(suffix)} }}'
            for _, suffix, _, _ in entries]
    L.append(", \\\n".join(rows))
    L.append("")
    L.append("} // namespace secrets")
    L.append("")
    return "\n".join(L)


def ident(s):
    return re.sub(r"[^A-Za-z0-9]", "_", s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    ap.add_argument("--check", action="store_true",
                    help="fail if the header is missing, without rewriting it")
    ap.add_argument("--extra", action="append", default=[], metavar="SUFFIX=HOST",
                    help="another host, as SUFFIX=HOST. Rarely needed: the AVV "
                         "endpoint is picked up from .env automatically.")
    ap.add_argument("--env", type=pathlib.Path, default=ROOT / ".env",
                    help="where to read an AVV_BASE_URL override (default: ./.env)")
    ap.add_argument("--no-env", action="store_true",
                    help="do not add the AVV endpoints at all")
    a = ap.parse_args()

    if a.check:
        if not a.out.exists():
            print(f"MISSING: {a.out}\n  run: python3 tools/fetch_root_ca.py",
                  file=sys.stderr)
            return 1
        print(f"ok: {a.out.name} exists")
        return 0

    if not shutil.which("openssl"):
        return fail("openssl is not installed:  sudo apt install openssl")

    bundle_path = find_bundle()
    if not bundle_path:
        return fail("no system CA bundle found. On Ubuntu:\n"
                    "  sudo apt install --reinstall ca-certificates")
    print(f"reading trusted roots from {bundle_path}")
    bundle = load_bundle(bundle_path)
    print(f"  {len(bundle)} roots")

    targets = list(HOSTS)

    if not a.no_env:
        found = avv_hosts(a.env)
        for host in found:
            # The full hostname as the suffix: these are specific machines, and
            # a registrable-domain suffix would also match anything else there.
            targets.append((host, host))
            print(f"AVV endpoint: {host}")
        if not found:
            print("note: no AVV endpoints found -- the firmware will refuse AVV "
                  "over TLS", file=sys.stderr)

    for spec in a.extra:
        if "=" not in spec:
            return fail(f"--extra wants SUFFIX=HOST, got {spec!r}")
        suffix, host = spec.split("=", 1)
        targets.append((suffix, host))

    entries, failures = [], []
    for suffix, host in targets:
        try:
            pem, subject, where = root_for(host, bundle)
            entries.append((host, suffix, subject, pem))
            print(f"  {host:32s} -> {subject}  [{where}]")
        except Exception as e:
            failures.append(f"{host}: {e}")
            print(f"  {host:32s} -> FAILED", file=sys.stderr)

    if failures:
        print("\n" + "\n".join(str(f) for f in failures), file=sys.stderr)
        if not entries:
            return 1
        print("\nContinuing with the hosts that worked. The firmware will REFUSE "
              "to connect to the others rather than connect unverified.",
              file=sys.stderr)

    # Two hosts often share a root. Emitting it twice wastes flash on a chip
    # that is at 69% already.
    seen, unique = {}, []
    for host, suffix, subject, pem in entries:
        if pem in seen:
            print(f"  note: {host} shares a root with {seen[pem]}")
        seen.setdefault(pem, host)
        unique.append((host, suffix, subject, pem))

    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(render(unique))
    print(f"\nwrote {a.out}  --  {len(unique)} host(s)")
    print("Now rebuild and flash:  pio run -t upload -d firmware")
    return 0


def fail(msg):
    print(msg, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
