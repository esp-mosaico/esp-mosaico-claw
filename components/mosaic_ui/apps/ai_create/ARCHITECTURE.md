# AI Create runtime architecture

## Non-negotiable boundaries

- `ai_create_controller` is the only owner of business and navigation state.
- `ai_create_presenter` is the only writer of AI Create scene bindings.
- `ai_create_app` translates GSP actions, pointer lifecycle, and timer ticks into
  Controller commands. It does not predict successful state transitions.
- The scene is declarative layout only. AI Create does not load a Lua reducer.
- Device-only services sit behind the Controller's gateway, ASR, and session
  APIs. Snapshot reads are pure and never advance work.
- Unsupported image and attachment flows are not present in the bundle.

The event direction is one way:

```text
GSP input -> ai_create_app -> Controller command -> Controller snapshot
                                              -> Presenter -> GSP bindings
ASR/gateway/session events -------------------^
```

## Voice latency contract

Pressing the voice target queues capture immediately. `asr_service_start()`
opens the local audio subscriber and starts its capture task before connecting
the provider. The capture task only moves 20 ms microphone frames into a
bounded PSRAM PCM queue; a separate sender task batches them into 80 ms
WebSocket writes. Network latency therefore cannot block microphone draining.
Queue exhaustion is a visible request error and never overwrites old audio.
Releasing the target captures the in-flight frame, drains the subscriber, and
closes that subscriber before publishing the capture fence, so cloud
finalization cannot accumulate post-release audio in an unread ring. Only
after the sender drains all queued PCM does the service send `finish-task`.
Provider sentence finals are accumulated by
`sentence_id`, so partial and final callbacks always contain the complete
utterance seen so far. The text is logged and copied into the Controller
snapshot for the Presenter.

A hold shorter than 300 ms is treated as an accidental tap and follows the
cancel path. Any empty result or ASR failure closes the voice overlay and is
reported as a retryable notice on the underlying page.

Voice is exposed to the Controller through `ai_create_voice_port`; the
Controller does not depend on `asr_service` or own a provider session. The
runtime composition root owns the ASR adapter and swaps it transactionally when
configuration changes. The port reports structured readiness (`disabled`,
`offline`, `audio_error`, `auth_failed`, and so on), while the Presenter owns
the user-facing prompt and recovery copy. A missing ASR key therefore remains
a stable disabled capability, and transient network/provider failures remain
retryable without pretending that the feature is unconfigured.

Voice cancellation is idempotent and applies while preparing, listening, or
finalizing. Upward release cancels only voice; either back affordance first
invalidates the voice operation and active Agent request, then navigates while
ASR/gateway cleanup continues asynchronously. Late callbacks cannot revive the
page or submit discarded text. Preparing, listening, and finalizing also have
Controller-owned deadlines of 30, 60, and 30 seconds respectively; expiry uses
the same cancellation path and leaves a retryable notice.

## Session history contract

- The Controller snapshot owns a bounded, ordered transcript. The Presenter
  never reconstructs turns from the legacy `input` and `response` fields.
- The device history port resolves the currently selected chat alias through
  `claw_session_mgr`, reads the persisted message array from `claw_memory`,
  ignores tool/system records, and returns the most recent five complete
  user/assistant turns.
- Starting the Controller and switching or creating a session reloads that
  transcript. An empty session stays empty; a stale read cannot replace a
  request that became active concurrently.
- Session deletion is confirmed in Controller-owned UI state and completed on
  the session worker. Deleting the current alias selects an adjacent session,
  or creates one empty replacement when it is the only session, before the
  old history is removed.
- The Host adapter stores transcripts per alias so simulation exercises the
  same new-session/restore-session behavior without linking device storage.

## Render budget contract

- Controller-driven bindings are written only when the snapshot revision
  changes. Timer ticks must not rewrite the full retained tree.
- `MOSAIC_EVENT_SCENE_CHANGED` invalidates the Presenter cache and replays all
  model and session bindings into the new GSP scene instance.
- Recording elapsed text is updated at 10 Hz. The Presenter samples the
  lock-free ASR loudness envelope at about 15 Hz, keeps 15 recent values, and
  submits only changed bar geometry in bounded GSP property batches.
- ASR, gateway, storage, and session work remains outside the UI render path.
  Loudness reuses each existing 20 ms PCM frame and does not add a capture
  subscriber, task, PCM copy, floating-point operation, or FFT.

## Simulator contract

The simulator compiles these production sources directly:

- `common/ai_create_controller.c`
- `apps/ai_create/ai_create_presenter.c`
- `apps/ai_create/ai_create_app.c`

`simulator/mosaic_ai_create_host.c` supplies only deterministic fake
ports: gateway events, voice events, session mapping, and a logical clock. It
must not implement Controller state transitions. Tests must
advance time through `mosaic_ai_create_runtime_step()`; getters remain pure.

## Device voice/UI validation mode

`CONFIG_MOSAIC_UI_AI_CREATE_DEVICE_MOCK=y` keeps the physical microphone and
real ASR service, but injects a local gateway into the production Controller.
After ASR final text is submitted, the runtime emits deterministic accepted,
partial, and final response events without publishing to the Agent. Disable
this option before validating the real Agent integration.

## Verifiable stages

1. **Single state owner**: no AI Create Lua logic is packaged; mode, screen,
   composer, voice, and request state are present in one snapshot.
2. **Atomic commands**: navigation and mode actions dispatch Controller
   commands; rejected commands keep the previous state and expose a notice.
3. **Voice-only input**: no keyboard, camera, source, or attachment action is
   emitted; voice is rejected outside welcome/chat and while a request is active.
4. **Shared simulation**: the host target links the production Controller and
   Presenter, with fake ports only. Host unit and scene interaction tests pass.
5. **Firmware integration**: AI Create, presenter, voice-session, and ASR
   objects cross-compile for the selected board. A full firmware build and
   device smoke test close this stage.
6. **History restore**: complete a turn, create a new session, and switch back;
   the latest five persisted turns reappear and no demo messages are injected.
7. **Animation budget**: while holding voice input, steady-state Presenter
   writes are limited to the 10 Hz timer label plus an approximately 15 Hz
   loudness waveform. Geometry uses bounded batches and a 2 px deadband, with
   no full-tree writes on timer-only frames.
8. **Lossless ASR boundary**: speak through more than one server VAD sentence,
   pause, then release while speaking the final word. The submitted text keeps
   every finalized sentence; the PCM summary reports `queue_dropped=0`,
   `capture_dropped=0`, and `captured=sent`.

Run simulator verification from the repository root:

```sh
cmake -S simulator -B build-host/mosaic-sim
cmake --build build-host/mosaic-sim --parallel 4
ctest --test-dir build-host/mosaic-sim --output-on-failure
```
