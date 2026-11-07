---
id: amo-shzut
status: open
deps: [amo-oevvt, amo-zkjmv, amo-lgdor, amo-imgbe]
links: []
created: 2026-11-06T23:13:23Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Voice session state machine and integration

Ready for implementation.

Objective: connect trigger, API server, audio, player and display into one working voice assistant that havpe-server drives end to end.

Flow:
- Idle: trigger enabled. On trigger, if no subscribed API client, show 'no server' and stay idle. Otherwise disable the trigger, send VoiceAssistantRequest(start=true, wake word phrase 'alexa' or empty for a button press) and show listening.
- After VoiceAssistantResponse (port 0), stream microphone audio as VoiceAssistantAudio, 16 kHz 16-bit mono, in chunks of about 32 ms (1024 bytes).
- Stop streaming on STT_VAD_END, ERROR or RUN_END. Show thinking after STT_VAD_END.
- STT_END 'text': show as transcript. TTS_START 'text': show as reply. ERROR 'message': show the error.
- TTS_END 'url': play it. RUN_END usually arrives right after TTS_END; let playback finish before returning to idle.
- Return to idle after playback ends, or after RUN_END when there is no URL. Re-enable the trigger.
- Client disconnect during a session: stop streaming and playback, go idle.
- Safety: if streaming lasts longer than 20 s with no stop event, send VoiceAssistantRequest(start=false) and go idle.
- Remove or disable the boot demos and self-tests from earlier tasks by default.
- Log free heap after each session.

Non-goals: barge-in, conversation follow-up (continue listening after a reply), timers.

## Acceptance Criteria

With havpe-server unchanged and pointed at the board by mDNS discovery: saying 'Alexa' and a command, or pressing KEY and speaking, runs a shortcut or the webhook and plays the reply. Free heap after a session returns to the same level over many sessions.

