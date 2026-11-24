#!/usr/bin/env python3
"""Converts a PEM certificate bundle into BearSSL trust anchors as C source.

Usage: gen_anchors.py <bundle.pem> <output.c>

Each certificate contributes its subject name and public key, which is all a
trust anchor needs (BearSSL, "X.509 trust anchors"). Uses only the standard library.
"""
import base64
import re
import sys

OID_RSA = "1.2.840.113549.1.1.1"
OID_EC = "1.2.840.10045.2.1"
CURVES = {  # Values of BR_EC_secp256r1 and friends in bearssl_ec.h.
    "1.2.840.10045.3.1.7": 23,
    "1.3.132.0.34": 24,
    "1.3.132.0.35": 25,
}


def read_tlv(data, offset):
    """Returns (tag, value_start, value_end, next_offset) for the DER element at offset."""
    tag = data[offset]
    length = data[offset + 1]
    position = offset + 2
    if length & 0x80:
        count = length & 0x7F
        if count == 0 or count > 4:
            raise ValueError("unsupported DER length")
        length = int.from_bytes(data[position:position + count], "big")
        position += count
    end = position + length
    if end > len(data):
        raise ValueError("DER element runs past the end")
    return tag, position, end, end


def children(data, start, end):
    """Lists (tag, value_start, value_end, element_start) for each element in a range."""
    items = []
    while start < end:
        element_start = start
        tag, value_start, value_end, start = read_tlv(data, start)
        items.append((tag, value_start, value_end, element_start))
    return items


def oid_text(raw):
    first = raw[0]
    parts = [first // 40, first % 40]
    value = 0
    for byte in raw[1:]:
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            parts.append(value)
            value = 0
    return ".".join(str(part) for part in parts)


def unsigned(raw):
    raw = raw.lstrip(b"\x00")
    return raw or b"\x00"


def parse_certificate(der):
    _, start, end, _ = read_tlv(der, 0)
    certificate = children(der, start, end)
    _, tbs_start, tbs_end, _ = certificate[0]
    fields = children(der, tbs_start, tbs_end)
    if fields[0][0] == 0xA0:  # Optional explicit version.
        fields = fields[1:]
    # serialNumber, signature, issuer, validity, subject, subjectPublicKeyInfo.
    # The stored name is the whole encoded subject, including its tag and length.
    _, _, subject_end, subject_start = fields[4]
    subject = der[subject_start:subject_end]

    _, spki_start, spki_end, _ = fields[5]
    algorithm, key_bits = children(der, spki_start, spki_end)
    algorithm_parts = children(der, algorithm[1], algorithm[2])
    oid = oid_text(der[algorithm_parts[0][1]:algorithm_parts[0][2]])
    key = der[key_bits[1] + 1:key_bits[2]]  # Skip the BIT STRING's unused-bits byte.

    if oid == OID_RSA:
        _, rsa_start, rsa_end, _ = read_tlv(key, 0)
        modulus, exponent = children(key, rsa_start, rsa_end)[:2]
        return subject, ("rsa", unsigned(key[modulus[1]:modulus[2]]),
                         unsigned(key[exponent[1]:exponent[2]]))
    if oid == OID_EC and len(algorithm_parts) > 1:
        curve = oid_text(der[algorithm_parts[1][1]:algorithm_parts[1][2]])
        if curve in CURVES:
            return subject, ("ec", CURVES[curve], key)
    return subject, None


def c_bytes(name, data):
    lines = [f"static const unsigned char {name}[] = {{"]
    for start in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[start:start + 12]) + ",")
    lines.append("};")
    return lines


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    bundle, output = sys.argv[1:]
    pem = open(bundle, encoding="ascii", errors="replace").read()
    blocks = re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", pem, re.S)

    body = []
    anchors = []
    skipped = 0
    for block in blocks:
        try:
            subject, key = parse_certificate(base64.b64decode("".join(block.split())))
        except (ValueError, IndexError):
            key = None
        if key is None:
            skipped += 1
            continue

        index = len(anchors)
        body += c_bytes(f"TA{index}_DN", subject)
        if key[0] == "rsa":
            body += c_bytes(f"TA{index}_RSA_N", key[1]) + c_bytes(f"TA{index}_RSA_E", key[2])
            anchors.append(
                f"    {{ {{ (unsigned char *)TA{index}_DN, sizeof TA{index}_DN }}, BR_X509_TA_CA,\n"
                f"      {{ BR_KEYTYPE_RSA, {{ .rsa = {{ (unsigned char *)TA{index}_RSA_N, "
                f"sizeof TA{index}_RSA_N, (unsigned char *)TA{index}_RSA_E, "
                f"sizeof TA{index}_RSA_E }} }} }} }},")
        else:
            body += c_bytes(f"TA{index}_EC_Q", key[2])
            anchors.append(
                f"    {{ {{ (unsigned char *)TA{index}_DN, sizeof TA{index}_DN }}, BR_X509_TA_CA,\n"
                f"      {{ BR_KEYTYPE_EC, {{ .ec = {{ {key[1]}, (unsigned char *)TA{index}_EC_Q, "
                f"sizeof TA{index}_EC_Q }} }} }} }},")

    if not anchors:
        print("gen_anchors: no usable certificates found", file=sys.stderr)
        return 1

    source = [
        "/*",
        f" * Generated by scripts/gen_anchors.py from {bundle.rsplit('/', 1)[-1]}.",
        f" * {len(anchors)} certificate authorities ({skipped} skipped). Do not edit by hand.",
        " */",
        '#include "tls_anchors.h"',
        "",
    ] + body + [
        "",
        f"static const br_x509_trust_anchor ANCHORS[{len(anchors)}] = {{",
    ] + anchors + [
        "};",
        "",
        "const br_x509_trust_anchor *const tls_trust_anchors = ANCHORS;",
        f"const size_t tls_trust_anchor_count = {len(anchors)};",
        "",
    ]
    with open(output, "w", encoding="ascii") as file:
        file.write("\n".join(source))
    print(f"gen_anchors: wrote {len(anchors)} trust anchors ({skipped} skipped) to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
