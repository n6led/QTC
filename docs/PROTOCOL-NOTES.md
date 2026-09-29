# MeshCore Companion Protocol notes

QTC communicates with MeshCore Companion firmware using the public Companion Protocol. Byte-level framing and payload handling are isolated in `src/protocol.c`.

## USB framing

- application to radio: `<`, little-endian `uint16` payload length, payload
- radio to application: `>`, little-endian `uint16` payload length, payload

QTC opens the serial device in raw nonblocking mode and continuously drains available radio frames.

## Session startup

QTC initializes the Companion application session before normal messaging traffic, queries radio/device information, then performs contact, channel, and stored-message synchronization in the background.

Only one request/response transaction is active at a time, while asynchronous firmware pushes can be received at any point.

## Messaging

QTC uses the Companion protocol operations for:

- direct text messages
- channel text messages
- stored-message synchronization
- message-waiting notifications
- direct-message delivery confirmations
- contact and channel synchronization
- radio settings and self advertisements
- export of the radio's own contact information

Waiting-message pushes trigger immediate stored-message retrieval until the radio reports that no messages remain. Interactive sends and receive work are prioritized over background roster refreshes.

## Reply and mention text

Replies insert human-readable `@[Name] > selected body | ` into the normal composer.
The selected logical message supplies a bounded, single-line quote; a recognized
channel sender prefix is omitted from the quote. The stored message is unchanged.
The existing direct/channel text send path handles the result, including its
existing multipart behavior. Selection uses local logical keys only inside the
TUI; these keys and database IDs are never added to the outgoing text.

Direct sender display comes from the contact identified by the message's
conversation key, not its text. Channel messages have no separately stored sender
identity in QTC. MeshCore's
[BaseChatMesh::sendGroupMessage](https://github.com/meshcore-dev/MeshCore/blob/main/src/helpers/BaseChatMesh.cpp)
prepends `<sender>: ` to channel text. QTC recognizes only incoming channel text
whose first colon is followed by an ASCII space, with 1–31 bytes before the colon
(the Companion's 32-byte name buffer includes NUL). The name must be valid UTF-8,
without control characters, brackets, or leading/trailing spaces. Missing or
ambiguous prefixes are left unknown; names containing colons are not supported.
For an existing multipart group, the first part supplies this display prefix;
without that part, the sender stays unknown. Received text is not rewritten.

This is a display convention, not cryptographic sender verification. A sender can
spoof it, and unconventional channel clients may not supply a usable prefix.
QTC does not infer direct senders from this convention, add persistent sender
fields, or change existing fragment assembly.

## Upstream documentation

- https://docs.meshcore.io/companion_protocol/
- https://github.com/meshcore-dev/MeshCore/wiki/Companion-Radio-Protocol

Protocol compatibility should be checked against current MeshCore firmware when the upstream Companion protocol changes.
