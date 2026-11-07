---
id: amo-zkjmv
status: closed
deps: [amo-mahwx]
links: []
created: 2026-11-06T23:13:23Z
type: task
priority: 2
assignee: Stavros Korokithakis
---
# Wake word (Alexa, WakeNet9s) and KEY push-to-talk

Ready for implementation.

Objective: a trigger module that reads microphone frames from the audio module and raises a trigger when it hears 'Alexa' or when KEY (GPIO10) is pressed. The session layer can enable and disable it.

Scope:
- ESP-SR component, WakeNet9s model wn9s_alexa, without the AFE (the C6 cannot run it). The model goes into the 'model' partition; make sure the normal flash command also writes the model, and update AGENTS.md if the command changes.
- KEY button on GPIO10: active level and pull from the schematic or by test. Debounce. A press while enabled raises the same trigger, tagged as a button press.
- Disabled while a session or playback is running (the device has no echo cancellation).
- Log each detection.

Non-goals: other wake words, wake word sensitivity UI, the PWR button.

