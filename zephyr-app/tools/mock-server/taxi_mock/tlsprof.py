"""TLS profiles: different certificates and protocol settings, chosen per connection.

Certificates are made on the first start (data/tls/) with `cryptography`, so nothing else is needed on
Windows. The profile can be changed on the admin page; new connections use it at once.
"""
import datetime
import ipaddress
import json
import ssl
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import NameOID


def _key(kind):
    if kind == "ec":
        return ec.generate_private_key(ec.SECP256R1())
    return rsa.generate_private_key(public_exponent=65537, key_size=4096 if kind == "rsa4096" else 2048)


def _name(cn, padding=""):
    attrs = [x509.NameAttribute(NameOID.COMMON_NAME, cn)]
    if padding:  # long names make the chain bigger (bigchain profile)
        attrs += [x509.NameAttribute(NameOID.ORGANIZATION_NAME, padding[:64]),
                  x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, padding[64:128] or "unit"),
                  x509.NameAttribute(NameOID.LOCALITY_NAME, padding[128:192] or "city")]
    return x509.Name(attrs)


def _cert(subject, key, issuer, issuer_key, *, ca, san=None, days=825, expired=False):
    now = datetime.datetime.now(datetime.timezone.utc)
    if expired:
        nb, na = now - datetime.timedelta(days=800), now - datetime.timedelta(days=30)
    else:
        nb, na = now - datetime.timedelta(days=1), now + datetime.timedelta(days=days)
    b = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(nb).not_valid_after(na)
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if san:
        b = b.add_extension(x509.SubjectAlternativeName(san), critical=False)
    return b.sign(issuer_key, hashes.SHA256())


def _pem_key(k):
    return k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())


def _pem(c):
    return c.public_bytes(serialization.Encoding.PEM)


class TlsProfiles:
    def __init__(self, folder: Path, ips, names=("localhost",)):
        self.dir = Path(folder)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.ips = list(dict.fromkeys(ips))
        self.names = list(names)
        self.san = [x509.DNSName(n) for n in self.names] + [x509.IPAddress(ipaddress.ip_address(i)) for i in self.ips]
        self._ctx = {}
        self.info = {}
        self._make_all()

    # ---------------------------------------------------------------- certificates
    def _files(self, name, builder):
        crt, key, meta = self.dir / f"{name}.crt", self.dir / f"{name}.key", self.dir / f"{name}.meta"
        want = json.dumps({"ips": sorted(self.ips), "names": sorted(self.names), "v": 2})
        if not (crt.exists() and key.exists() and meta.exists() and meta.read_text() == want):
            chain, k = builder()
            crt.write_bytes(b"".join(_pem(c) for c in chain))
            key.write_bytes(_pem_key(k))
            meta.write_text(want)
        return crt, key

    def _self_signed(self, kind, expired=False):
        def build():
            k = _key(kind)
            n = _name("taxi-mock-server")
            return [_cert(n, k, n, k, ca=False, san=self.san, expired=expired)], k
        return build

    def _big_chain(self):
        # root -> 5 intermediates (RSA 4096, long names) -> leaf. The server sends leaf + intermediates.
        pad = "taxi meter test chain " * 10
        root_k = _key("rsa4096")
        root_n = _name("Taxi Test Root CA", pad)
        issuer_n, issuer_k = root_n, root_k
        chain = []
        for i in range(5):
            k = _key("rsa4096")
            n = _name(f"Taxi Test Intermediate {i + 1}", pad)
            chain.append(_cert(n, k, issuer_n, issuer_k, ca=True))
            issuer_n, issuer_k = n, k
        leaf_k = _key("rsa")
        leaf = _cert(_name("taxi-mock-server", pad), leaf_k, issuer_n, issuer_k, ca=False, san=self.san)
        return [leaf] + list(reversed(chain)), leaf_k

    def _make_all(self):
        rsa_files = self._files("rsa", self._self_signed("rsa"))
        files = {
            "rsa": rsa_files,
            "ecdsa": self._files("ecdsa", self._self_signed("ec")),
            "tls12only": rsa_files,
            "tls13only": rsa_files,
            "ecdhe_gcm_only": rsa_files,
            "bigchain": self._files("bigchain", self._big_chain),
            "expired": self._files("expired", self._self_signed("rsa", expired=True)),
        }
        for name, (crt, key) in files.items():
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            if name == "tls12only":
                ctx.maximum_version = ssl.TLSVersion.TLSv1_2
            if name == "tls13only":
                ctx.minimum_version = ssl.TLSVersion.TLSv1_3
            if name == "ecdhe_gcm_only":
                ctx.set_ciphers("ECDHE+AESGCM")
            ctx.load_cert_chain(str(crt), str(key))
            self._ctx[name] = ctx
            pem = Path(crt).read_bytes()
            certs = x509.load_pem_x509_certificates(pem)
            der = sum(len(c.public_bytes(serialization.Encoding.DER)) for c in certs)
            leaf = certs[0]
            self.info[name] = {
                "certificates": len(certs), "chain_bytes": der,
                "key": "EC P-256" if isinstance(leaf.public_key(), ec.EllipticCurvePublicKey) else
                       f"RSA {leaf.public_key().key_size}",
                "valid_until": leaf.not_valid_after_utc.strftime("%Y-%m-%d"),
            }

    def context(self, profile):
        return self._ctx.get(profile) or self._ctx["rsa"]
