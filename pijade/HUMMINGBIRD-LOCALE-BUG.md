# hummingbird: locale-sensitive `toLowerCase()` breaks UR decoding in Turkish/Azeri locales

Status: filed 2026-08-26 as https://github.com/sparrowwallet/sparrow/issues/2064 (opened on the
Sparrow repo rather than hummingbird: the defect is in hummingbird, but that repo has had no push
since 2024-10-24 while Sparrow is actively maintained by the same author, and the user-visible
impact is in Sparrow).
Discovered: 2026-08-26, while debugging piJade xpub export into Sparrow 2.5.3.

## Summary

`URDecoder.parse()` lowercases the incoming UR string with the no-argument
`String.toLowerCase()`, which uses the JVM's **default locale**. In the Turkish and Azeri
locales `"I".toLowerCase()` yields `"ı"` (U+0131, dotless i), not `"i"`. Bytewords has no
minimal pair containing `ı`, so `getMinimalBytewords().indexOf(word)` returns `-1`, the byte
array is corrupted, and `Bytewords.stripChecksum` throws `InvalidChecksumException`.

Every UR part containing an uppercase `I` is therefore rejected on a machine whose locale is
`tr` or `az`. Since uppercase UR is the norm for QR display (it enables the QR
`alphanumeric` mode), this affects real hardware wallets, not a hypothetical encoder:
Blockstream Jade (`main/bcur.c`: `force_uppercase = true`) and SeedSigner
(`BaseFountainQrEncoder.next_part()`: `return self.ur2_encode.next_part().upper()`) both emit
uppercase parts.

The error message is misleading: an unmappable byteword pair is reported as a checksum
failure, which sends debugging toward the encoder rather than the locale.

## Minimal proof

```java
Locale.setDefault(Locale.forLanguageTag("tr"));
System.out.println("LSADHDCXIY".toLowerCase());   // lsadhdcxıy  <- dotless i, unmappable
```

## Full reproducer

```java
UR ur = new UR("crypto-account", cbor);              // any payload
UREncoder enc = new UREncoder(ur, 9, 8, 0);
URDecoder dec = new URDecoder();
for (int i = 0; i < 400; i++) {
    String part = enc.nextPart().toUpperCase(Locale.ROOT);   // what the device displays
    try { dec.receivePart(part); } catch (Exception e) { /* InvalidChecksumException */ }
    if (dec.getResult() != null) break;
}
```

Run with `-Duser.language=en` -> completes. Run with `-Duser.language=tr` -> roughly half the
parts throw `InvalidChecksumException`.

## Measured impact (real device, 13 pure parts, 115-byte crypto-account CBOR)

| Emitter behaviour | tr_TR result |
|---|---|
| Fixed set of 17 parts, looped (Jade) | never completes; stalls at 57.1% forever |
| Continuously streamed parts (SeedSigner) | completes after 17 parts, despite 8 rejections |

A device that emits a fixed fountain set can never recover, because the same parts are
rejected on every loop. This is why the bug reads as "Jade cannot export to Sparrow" while
SeedSigner appears to work: the streaming emitter masks it.

## Affected call sites (checked against master, commit 6f06b2c, 2024-10-22)

| File:line | Call |
|---|---|
| `URDecoder.java:145` | `String lowercased = string.toLowerCase();` |
| `LegacyURDecoder.java:11` | `fragments.add(fragment.toLowerCase());` |
| `LegacyURDecoder.java:57` | `String payload = components[components.length-1].toLowerCase();` |
| `LegacyURDecoder.java:156` | `String[] pieces = payload.toLowerCase().split("of");` |
| `RegistryType.java:66` | `registryType.toString().equals(type.toLowerCase())` |

`URDecoder.java:145` is the one that produces the failure described above; the rest share the
same defect and are listed so a fix can cover them in one pass.

## Fix

Pass an explicit locale at each of those call sites, eg. in `URDecoder.parse()`:

```java
String lowercased = string.toLowerCase(Locale.ROOT);
```

## Notes for the report

- Sparrow 2.5.3 on macOS, system locale `tr_TR`, `~/.sparrow/sparrow.log` shows 1407
  `Bytewords$InvalidChecksumException` entries across three scanning sessions.
- Verified against Sparrow's own shipped classes, extracted from the jlink image with
  `jimage extract`, so this is not a version-drift artifact.
