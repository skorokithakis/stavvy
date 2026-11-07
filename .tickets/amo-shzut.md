---
id: amo-shzut
status: closed
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


## Notes

**2026-11-07T20:03:10Z**

Design notes from earlier tasks: (1) Use one session task with a FreeRTOS queue. Trigger, API (connect/disconnect/response/event) and player completion callbacks only post messages to the queue; all state changes, trigger_enable/disable, player_play_url/player_stop, display calls and audio streaming decisions happen in the session task. Reason: API callbacks run on the API task, the player callback runs on the player task, and player_stop() waits for the player task, so calling it from a callback can deadlock or stall the API task. (2) The session task, or a streaming task it owns, reads the microphone via audio_capture_* after trigger_disable() returns; esphome_api_send_audio max chunk is 2048 bytes. (3) Event values arrive truncated to 1024 bytes; that is fine. (4) havpe-server serves an empty tts.mp3 in some cases; the player reports success with no audio, treat it as normal. (5) Display boot demo and player/audio boot tests must be off in the default build. (6) Free heap is tight: about 114 KB minimum idle, 82 KB minimum during MP3. Avoid new large buffers; log free heap and minimum free heap after each session.
