# Secure Bootloader: Threat Model and Design

Reprogramming an ECU over the diagnostic interface, using UDS (ISO 14229) over
ISO-TP (ISO 15765-2), with authenticity and rollback protection that the UDS
specification itself does not provide.

**Why it is here.** The diagnostic interface is the most valuable attack surface
on a vehicle, because it is the one designed to replace running code. UN R155
requires a cybersecurity management system covering exactly this class of
threat, R156 governs the software update process itself, and ISO 21434 is the
engineering standard underneath both. This document is the analysis; the code
implements the parts marked as built.

> **Status.** The transport layer and the UDS server, including the security
> access state machine, are implemented and tested. The signature algorithm
> behind the verifier interface and the flash programming are designed here and
> not yet written — see [Implementation status](#implementation-status). Nothing
> in this document should be read as a claim that the vehicle is secure.

---

## 1. The item and its assets

| Asset | Why an attacker wants it | Loss of |
|---|---|---|
| Application firmware | Run chosen code on a node that drives the vehicle | Integrity, authenticity |
| Bootloader | Persistent control that survives reflashing | Integrity |
| Verification public key | Replace it and every later image is attacker-signed | Integrity |
| Rollback counter | Reinstall an older signed image with a known flaw | Integrity |
| Diagnostic authorisation state | Reach programming services without credentials | Access control |

The safety-relevant consequence is the one that matters: the Motion node drives
the motor and steering. Arbitrary code there is arbitrary vehicle behaviour.

## 2. Threat scenarios

Framed the way a TARA would, kept short. Feasibility assumes physical access to
the bus, which for this vehicle is trivially available.

| # | Threat scenario | Attack path | Impact |
|---|---|---|---|
| T1 | Unauthorised firmware installed | Reach programming session, bypass security access, write image | Safety |
| T2 | Rollback to a vulnerable version | Install a genuinely signed older image | Safety |
| T3 | Bootloader entry reached without authorisation | Send the session-control request; nothing on CAN is authenticated | Operational |
| T4 | Credentials recovered from one ECU and reused fleet-wide | Extract the shared secret, unlock every ECU of that type | Safety |
| T5 | Authorisation replayed | Capture a valid exchange, replay it later | Safety |
| T6 | Brute force of the authorisation | Repeatedly guess, power-cycling to clear the attempt counter | Safety |
| T7 | Partial or corrupted image left runnable | Interrupt the transfer, leave half an image that still boots | Safety |
| T8 | Firmware extracted for analysis | Read flash over the debug port | Confidentiality |
| T9 | Transport-layer memory corruption | Malformed segmented message overflows the reassembly buffer | Safety |

## 3. Why UDS SecurityAccess is not enough

This is the part worth being precise about, because service `0x27` is what most
implementations rely on and it does not do what its name suggests.

The conventional design: the ECU returns a seed, the tester computes
`key = f(seed, secret)` and sends it back. The ECU computes the same function
and compares.

**The function and the secret are symmetric and shared.** The same `f` and the
same secret are compiled into every ECU of that type *and* into every diagnostic
tool that services them. Recovering it once — from a tester binary, or by
dumping a single ECU — unlocks every ECU of that type, permanently, across the
whole fleet. Published seed-key algorithms exist for several manufacturers. That
is threat T4, and it is a structural property of the design, not an
implementation slip.

**The seed is frequently not random.** A free-running timer or a counter is
common. Predictable seeds allow precomputation, and short seeds — two bytes has
been used in production — are brute-forceable outright.

**Attempt limiting usually lives in RAM.** The specification provides
`exceededNumberOfAttempts` and `requiredTimeDelayNotExpired`, but if the counter
resets on power-up then the bypass is simply to power-cycle between attempts.
That is T6, and it is defeated by persistence, not by cryptography.

**Unlocking grants blanket authority.** Access is not bound to a particular
operation or image, so any subsequent request in that session is permitted.

The honest summary: `0x27` as commonly built is obfuscation. It raises the
effort required, which has value, but it does not survive an attacker who has
one tester and one ECU.

## 4. What this design does instead

Two separate mechanisms, because they defend different things.

### 4.1 Authorisation: asymmetric challenge-response

The ECU stores a **public** key. The tester proves possession of the
corresponding private key, which never leaves the signing service.

```
tester                                     ECU
  |-- RequestSeed ------------------------->|
  |                                         |  challenge = counter || UID || operation
  |<-- challenge ---------------------------|
  |                                         |
  |  sign(challenge, private key)           |
  |-- signature --------------------------->|
  |                                         |  verify against stored public key
  |                                         |  on success: counter += 1
```

Compromising an ECU now yields a public key, which is worthless for forging.
There is no shared secret to extract, and no fleet-wide break from a single
device. T4 is addressed structurally rather than by hiding the algorithm.

### 4.2 Freshness without a random number generator

**The STM32F446 has no RNG peripheral.** Verified against the device header:
there is no `RNG_BASE`, no `CRYP`, no `HASH`. This is a real constraint and it
changes the design.

A challenge-response normally uses a random nonce. Without an entropy source,
inventing one from a timer would reproduce exactly the weakness criticised
above. Freshness, however, does not require randomness — it requires that a
challenge is never accepted twice.

So the challenge is a **strictly monotonic counter held in flash**, combined
with the device unique ID at `0x1FFF7A10` and the requested operation:

- **Replay** fails because the counter has already advanced past the captured
  value (T5).
- **Cross-ECU replay** fails because the UID in the signed payload names one
  device.
- **Precomputation** buys nothing: an attacker can predict the next challenge,
  but predicting it does not help without the private key.

This is a better fit for the hardware than a poor random number generator would
be, and it is worth being able to explain why.

### 4.3 Image authenticity, verified at every boot

Security access authorises the *session*. It says nothing about what was
written. The image therefore carries its own signature over a header that
includes its length, version and a hash of the payload.

The bootloader verifies that signature **on every boot**, not only at
programming time. A transfer interrupted halfway leaves an image whose hash no
longer matches, so it never executes — which is T7, and it is why a CRC is not
sufficient here.

> A CRC detects corruption. It does not detect an attacker, because an attacker
> recomputes it. The distinction between integrity and authenticity is the one
> most commonly collapsed, and conflating them is how a bootloader ends up
> "verified" but not secure.

### 4.4 Rollback protection

A monotonic version floor in flash. An image whose version is below the floor is
refused even though its signature is perfectly valid, because the threat is an
authentic old image with a known defect (T2). The floor is raised only after an
image has booted successfully, so a bad update cannot lock the ECU out of
recovery.

## 5. Attack surface, and what answers each part

| Surface | Threat | Mitigation | Built |
|---|---|---|---|
| Programming session entry | T3 | Entry permitted, but every service behind it requires authorisation | **built** |
| Security access exchange | T4, T5 | Asymmetric signature over counter, UID and operation | **built** (verifier injected) |
| Repeated attempts | T6 | Attempt count persisted, delay re-imposed from boot | **built** |
| Image transfer | T1, T7 | Signature over the whole image, verified at every boot | designed |
| Version acceptance | T2 | Monotonic version floor | designed |
| ISO-TP reassembly | T9 | Declared length checked against the buffer before any copy; overflow refused | **built** |
| Debug port | T8 | Readout protection; Level 2 is irreversible and is an operational decision, not a default | not addressed |
| CAN bus itself | all | Unauthenticated by design. Out of scope here; see the note below | not addressed |

**The bus is not authenticated and this design does not change that.** Any node
with bus access can send any frame. The bootloader defends the act of
reprogramming, not the bus. Message authentication for the control traffic is a
separate piece of work, and claiming otherwise would misrepresent what is built.

## 6. Hardware constraints that shaped the design

These are the details that decide whether a design is real or a diagram.

| Constraint | Verified | Consequence |
|---|---|---|
| No RNG peripheral | no `RNG_BASE` in the device header | Monotonic counter instead of a nonce (§4.2) |
| No crypto or hash accelerator | no `CRYP_BASE`, no `HASH_BASE` | Signature verification in software |
| CRC peripheral present | `CRC_BASE` defined | Useful for corruption, useless against an attacker |
| 512 KB flash, non-uniform sectors | 4×16 KB, 1×64 KB, 3×128 KB | Slot boundaries must fall on sector boundaries |
| 96-bit unique ID at `0x1FFF7A10` | in the device header | Binds a challenge to one device |

Software verification costs time, but it runs once per boot rather than per
frame, so tens of milliseconds is acceptable. The sector geometry is the
constraint that actually dictates the memory map:

```
0x08000000  sectors 0-3   64 KB   bootloader
0x08010000  sector  4     64 KB   keys, counters, metadata
0x08020000  sector  5    128 KB   application slot A
0x08040000  sector  6    128 KB   application slot B
0x08060000  sector  7    128 KB   spare / staging
```

Two slots mean an update never overwrites the only working image, so a failed or
interrupted update leaves the previous version bootable.

## 7. Transport layer

UDS messages exceed the eight bytes a classic CAN frame carries, so ISO-TP
segments them: a single frame up to seven bytes, or a first frame declaring the
total length followed by consecutive frames, paced by flow control from the
receiver.

Reassembly is an attack surface in its own right (T9). The declared length in a
first frame is attacker-controlled and can claim up to 4095 bytes. The
implementation therefore checks it against the available buffer **before any
copy**, and refuses with a flow-control overflow rather than truncating or
writing past the end. Sequence numbers are checked on every consecutive frame,
consecutive frames arriving without a first frame are ignored, and a stalled
transfer times out rather than holding the buffer indefinitely.

These are the cases the tests target, because they are the ones an attacker
reaches first.

## 8. Residual risk

Stated rather than implied away:

- The bus carries no authentication, so any node can impersonate any other.
- Without readout protection the debug port reveals the whole image.
- Readout protection Level 2 is irreversible and prevents field debugging; that
  is a business decision rather than a technical default.
- The signing key becomes the highest-value asset in the system. Its handling is
  out of scope here and would dominate a real deployment.
- No side-channel or fault-injection resistance. Glitching the verification
  branch is a known attack class and is not addressed.

## Implementation status

| Component | State |
|---|---|
| ISO-TP segmentation and reassembly | **implemented, host-tested** |
| UDS server: sessions, services, negative responses | **implemented, host-tested** |
| Security access state machine with persisted attempt limiting | **implemented, host-tested** |
| Signature algorithm behind the verifier interface | not written |
| Signature verification | designed, not written |
| Flash programming and slot management | designed, not written |
| Readout protection | not addressed |

## References

- ISO 14229 — Unified Diagnostic Services
- ISO 15765-2 — Diagnostics over CAN, network and transport layer
- ISO 21434 — Road vehicles, cybersecurity engineering
- UN R155 — cybersecurity and cybersecurity management systems
- UN R156 — software update and software update management systems
