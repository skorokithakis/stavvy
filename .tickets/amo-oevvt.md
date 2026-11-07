---
id: amo-oevvt
status: open
deps: [amo-pmgdr]
links: []
created: 2026-11-06T23:13:23Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# ESPHome native API server (subset used by havpe-server)

Ready for implementation.

Objective: a TCP server on port 6053 that speaks the plaintext ESPHome native API well enough for havpe-server (a Go client that drives a voice pipeline) to connect, stay connected and run voice sessions. The architect gives you the path to the havpe-server source; it is the reference for behaviour. Its main.go handleConnection and frame.go show exactly what the client sends and expects.

Scope:
- Framing: 0x00 preamble, varint body size, varint message type, protobuf body.
- Protobuf with nanopb. Copy only the needed messages from havpe-server's proto/api.proto into a reduced .proto, with the same field numbers. Generate the code at build time or commit the generated files; your choice, but the build must work in the Docker image.
- Messages: HelloRequest/Response (1/2), DisconnectRequest/Response (5/6), PingRequest/Response (7/8, answer the client's pings), DeviceInfoRequest/Response (9/10), ListEntitiesRequest (11) answered only with ListEntitiesDoneResponse (19), SubscribeStatesRequest (20, accept and ignore), SubscribeVoiceAssistantRequest (89), VoiceAssistantRequest (90, device to client), VoiceAssistantResponse (91, client to device), VoiceAssistantEventResponse (92, client to device, with name/value data), VoiceAssistantAudio (106, device to client). Log and skip unknown types.
- One client at a time. A new connection replaces the old one, because after a network drop the client reconnects while the old socket may still look open. Drop a client that sends nothing for 90 s.
- Module API for the session layer: is a client connected and subscribed to voice assistant; send VoiceAssistantRequest (start flag, wake word phrase); send audio chunk; callbacks for VoiceAssistantResponse and for events (event type plus name/value pairs, e.g. 'text', 'url', 'code', 'message'); callback on client connect and disconnect.
- Writes come from more than one task (receive task, audio task), so serialise frame writes.

Non-goals: noise encryption, entities, states, services, UDP audio, any other message types.

## Design

Fill DeviceInfoResponse with name, MAC and model, and set voice_assistant_feature_flags for voice assistant plus API audio, so the device looks like a voice satellite. havpe-server does not check these today. HelloResponse should report API version 1.10, which matches what the client sends.

