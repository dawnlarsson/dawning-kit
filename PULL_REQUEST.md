# sec: harden Waterlink admission, entropy, DH validation, authorization, and transfers

## Security audit scope

This PR hardens Waterlink's highest-risk trust boundaries:

- unauthenticated handshake admission and denial-of-service resistance
- entropy failures and ephemeral-key generation
- X25519/Diffie-Hellman input validation
- Noise handshake transcript integrity
- peer and group authorization databases
- private-key and secret cleanup
- privileged push/pull staging and atomic publication

Waterlink's latency-critical established-session transport path remains unchanged. The added checks run during handshake admission, handshake and pairing construction, rekeying, discovery rotation, authorization-file loading, and one-time file publication.

## Security fixes

### Bound aggregate unauthenticated handshake work

Waterlink already limited initiations per source address, but source addresses are unauthenticated at that point. An attacker could rotate spoofed source addresses through the fixed-size admission table and repeatedly obtain a fresh per-source bucket.

This PR:

- retains the existing per-source limit of 5 initiations in a burst and 5 per second
- adds a global admission bucket limited to a burst of 64 initiations and a sustained rate of 64 per second
- applies the global bucket after the per-source bucket, preserving the tighter per-source policy
- prevents rotating or spoofed source addresses from causing unbounded curve work
- keeps both admission checks off the established encrypted-session path

### Fail closed when cryptographic randomness is unavailable

All security-sensitive Waterlink calls to `system_random_fill` now check their result.

Checked operations include:

- initial handshake ephemeral keys
- responder ephemeral keys
- pairing ephemeral keys
- session receiver indexes
- rekey receiver indexes
- conversation identifiers
- generated group secrets
- mDNS host labels
- group discovery instance identifiers
- group discovery nonces
- randomized transfer staging names
- first-use private-key generation

If entropy is unavailable, the affected operation now fails instead of continuing with uninitialized or predictable state.

Session-index allocation is also bounded to 128 attempts. It returns failure if randomness is unavailable or repeatedly produces zero or colliding indexes, rather than spinning indefinitely.

### Draw responder entropy before reserving a session

The responder now obtains everything required to construct its answer—the receiver index and ephemeral secret—before reserving a session slot.

This prevents an entropy outage from allowing repeated initiations to fill the session table with sessions that can never be answered.

### Withhold discovery until random labels are ready

Nearby group discovery now tracks whether a complete set of random labels and nonces was generated successfully.

If any random draw fails:

- discovery does not publish partially refreshed or predictable labels
- the incomplete rotation is not marked successful
- announcements wait until a complete label set exists
- generation is retried later
- abandoned pairing and Noise state is cleared

Normal discovery resumes only after the host label and every group's instance identifier and nonce have been generated successfully.

### Reject invalid and low-order X25519 inputs

The Noise IK and group-pairing message writers previously did not propagate every X25519 failure. A configured low-order peer key could therefore leave the Noise state without an expected derived key while message construction continued.

This PR changes the relevant handshake and pairing writers to return success or failure and validates every DH step before continuing.

The following paths now fail closed:

- initiator ephemeral-to-responder-static DH
- initiator static-to-responder-static DH
- responder ephemeral-to-initiator-ephemeral DH
- responder ephemeral-to-initiator-static DH
- group-pairing ephemeral-to-ephemeral DH
- group-pairing static-to-ephemeral DH

When a DH operation fails:

- no partially derived message is sent
- no subsequent payload is sealed under incomplete state
- ephemeral and Noise state is cleared
- pairing state is abandoned safely
- the failure is propagated through the listener, client, rekey, or pairing caller

A new `sec:` regression verifies that initiation refuses a low-order peer key while valid peer keys still complete the same handshake.

Manually paired public keys are also checked before they enter the authorization database. A low-order key is refused at `moonwater link pair` rather than being stored as a peer that can never complete a safe handshake.

### Preserve the live Noise transcript when an answer is invalid

Processing a responder message advances Noise handshake state. Applying an invalid response directly to the live initiator transcript could corrupt that transcript before the legitimate response arrived.

Responder messages are now processed against a private candidate copy:

1. validate the datagram kind and receiver index
2. validate the cheap message gate
3. copy the live Noise transcript into a candidate
4. perform DH and authenticated decryption against the candidate
5. wipe the candidate on failure without modifying the live transcript
6. split and install transport keys only after complete verification
7. clear the original handshake state after the candidate succeeds

A forged or malformed response can no longer advance the live transcript and spoil a valid response that follows it.

### Clear sensitive state on failure paths

Temporary cryptographic state is now cleared on early failures as well as successful completion.

This includes:

- freshly generated private-key bytes
- random private-key temporary-name bytes
- handshake ephemeral keys
- partially advanced Noise state
- abandoned pairing state
- generated group-secret buffers
- loaded group key material where applicable

Private-key generation now clears temporary material after random-generation, file-creation, write, and `fsync` failures.

### Validate peer and group authorization databases

The peer and group databases decide which remote keys are trusted and which capabilities they receive. They are now loaded through a dedicated private-record reader rather than as ordinary state text.

`/root/link.peers` and `/root/link.groups` must now be:

- regular files
- owned by root
- inaccessible to group and other users
- opened with `O_NOFOLLOW`
- no larger than their fixed in-memory database
- an exact multiple of the corresponding record size
- completely readable to the size authenticated from the opened descriptor

The loader rejects:

- symbolic links
- non-regular files
- wrong ownership
- group- or world-accessible files
- partial records
- oversized databases
- short reads

A rejected authorization database is treated as empty, so unsafe or malformed state grants no peer or group authorization.

### Use randomized and exclusive transfer staging files

Push and pull previously used the predictable staging name:

```text
PATH.link-part
```

A fixed name could be planted in advance to deny a transfer, and concurrent transfers targeting the same destination could collide.

Transfers now use randomized names of the form:

```text
PATH.link-part.<random>
```

Each staging file is:

- created beside its destination
- opened with exclusive-create semantics
- opened with `O_NOFOLLOW`
- opened with `O_CLOEXEC`
- retried with a new random suffix on collision
- never created by truncating an existing inode

Keeping staging beside the destination preserves the final same-filesystem atomic rename.

The server records the exact staging pathname for each push, and the client records it for each pull.

### Authenticate staging ownership before publication

Exclusive creation does not by itself prove that the staging pathname still refers to the inode originally opened by the transfer.

Pushes and pulls now retain the staging descriptor through the publication decision. Immediately before rename, Waterlink:

- obtains the identity of the open staging descriptor
- examines the current staging pathname without following symlinks
- verifies that the descriptor and pathname refer to the same inode
- refuses publication if the pathname was removed, replaced, or changed into a symlink
- avoids removing a pathname after validation shows it no longer belongs to the transfer

Only a completely written, successfully synchronized, still-owned staging inode is renamed over the final destination.

This preserves whole-or-not-at-all publication for both remote pushes and local pull destinations.

### Clean up interrupted transfers safely

Each push session tracks its exact randomized staging pathname.

If a push is interrupted:

- the incomplete staging file is not published
- the transfer's randomized staging name is removed during cleanup only when the still-open descriptor proves the pathname is the same inode
- unrelated predictable `.link-part` files remain untouched
- a pathname replaced during an interrupted transfer is not deleted as though it still belonged to the transfer

## Regression coverage

New `sec:` checks cover:

- rotating source addresses being held to the global admission burst
- the global admission bucket refilling normally
- an initiator refusing a low-order peer key
- `moonwater link pair` refusing a low-order public key before storing it
- valid initiation and response construction still succeeding
- a planted predictable `.link-part` file not being truncated
- a planted predictable `.link-part` file not denying a push
- a planted predictable `.link-part` file not denying a pull
- a publicly writable peer database granting no authorization
- restoring private peer-database permissions restoring authorization

Existing Waterlink tests continue to cover:

- Noise IK message construction and authentication
- replay-window behavior
- reliable delivery and retransmission
- rekeying
- grant enforcement
- push and pull integrity
- symlink handling
- atomic publication

## Latency and performance

Waterlink is latency critical, so the established encrypted transport path was deliberately left unchanged.

The following per-frame operations have no new security branches, allocations, random draws, filesystem calls, or admission checks:

- frame posting
- packet filling
- AES-GCM sealing
- AES-GCM opening
- replay checks
- frame delivery
- acknowledgement generation
- acknowledgement processing

The new work is limited to:

- unauthenticated initiation admission
- initial handshake construction
- rekey construction
- group-pairing construction
- discovery-label rotation
- authorization-database loading
- one-time staging-file creation
- one-time transfer publication

The added DH result checks occur only while constructing handshake or pairing messages.

The native x86_64 transport benchmark completed at:

| Traffic shape | Total cost |
| --- | ---: |
| Bulk | 813 ticks/frame |
| Keystroke | 767 ticks/frame |

Earlier runs measured 810–815 and 771–778 ticks/frame respectively, placing the latest results within normal run-to-run variation.

## Testing

The following checks were run:

- `git diff --check`
- `python3 -m py_compile test/differential.py`
- `rg -n "system_random_fill\\(" src/waterlink -C 1`
  - audited every remaining Waterlink random draw
  - every security-sensitive result is now checked
- `rg -n "^[[:space:]]*waterlink_mix_dh\\(" src/waterlink`
  - no unchecked standalone DH-mixing calls remain
- `sh test/run waterlink`
  - native x86_64: 156/156 checks passed
  - ARM64 and RISC-V variants were unavailable because the corresponding cross-compilers are not installed
  - optional Noise-package interoperability checks were unavailable because the required Python cryptography package is not installed
  - optional mDNS checks were unavailable because `dnspython` is not installed
- `sh test/run audit`
  - all 2,263 available native checks passed
  - the lane was incomplete only because ARM64 and RISC-V cross-compilers were unavailable
- `sh test/run bench link`
  - native x86_64 completed
  - bulk: 813 ticks/frame
  - keystroke: 767 ticks/frame
  - cross-architecture benchmark variants were unavailable because the cross-compilers are not installed

The network-namespace Waterlink integration lane could not be executed in this environment because it lacks `ip` and the required unprivileged namespace, veth, and netem facilities.
