# Voice query flow — from “Ok Nino” to the spoken reply

This is the path a spoken question takes on the current firmware and PC server: wake word, voice-activity capture, WebSocket, speech-to-text, reply, speech synthesis, and playback.

The board never turns speech into text. It records a short WAV and sends that clip. The PC (`server/app.py` + `server/voice_service.py`) transcribes it, chooses a reply, synthesizes audio, and sends a WAV back. The ESP plays that WAV on the speaker.

---

## 1. What you hear, in order

```text
You say "Ok Nino"
        │
        ▼
Wake detector (always listening)
        │
        ▼
Short beep (wake chime)
        │
        ▼
Eyes switch to listening
        │
        ▼
VAD records your question (16 kHz mono)
        │  stops on trailing silence, or after 10 s
        ▼
WAV sent over WebSocket to the PC
        │  eyes switch to thinking
        ▼
PC: speech-to-text → reply text → speech WAV
        │
        ▼
JSON metadata, then reply WAV, back to the ESP
        │
        ▼
Speaker plays the reply
        │
        ├─ usually: done beep, then wake word is armed again
        │
        └─ conversation replies: mic opens again without "Ok Nino"
           (continue-listen), until you say goodbye or the listen times out
```

A query only starts if the board already has a PC WebSocket URL (`voice connect <PC_LAN_IP> 8000`, stored as something like `ws://<ip>:8000/ws/voice`). If that URL is empty, the wake word is logged and ignored.

---

## 2. Who owns the microphone

One reader at a time. Both wake detection and VAD call `nino_mic_read()`.

| Piece | File | Role |
| --- | --- | --- |
| Mic picker | `main/mic_input.c` | USB 4-mic when it is plugged in; onboard ES8311 only as fallback |
| Wake loop | `main/voice_wake.cpp` | Reads 10 ms chunks (160 samples at 16 kHz) and feeds the wake model |
| VAD | `main/voice_assist.c` | Reads 20 ms frames while it owns the mic |
| Hold flag | `nino_voice_wake_set_mic_capture_hold()` | While true, the wake loop does not read the mic |

When VAD starts it sets the hold, flushes leftover samples (`nino_mic_flush()`), and pauses music (`nino_music_pause_for_speech(true)`). When VAD finishes it clears the hold and resets the wake model so the phrase “Ok Nino” that was just spoken does not immediately re-trigger.

During the beep and during VAD the wake task is also blocked by `s_after_wake_busy`, so it does not compete for samples even before the hold is set.

---

## 3. Wake word — “Ok Nino”

Started from `app_main` by `nino_voice_wake_init()` (`main/voice_wake.cpp`).

1. `mww_detector_init()` loads the embedded model `ok_nino_v6.tflite` and the TensorFlow Lite Micro audio frontend (`main/mww_detector.cpp`).
2. Task `wake_detect` runs on the second core when the chip has two cores.
3. Each loop reads 10 ms of 16 kHz mono PCM and calls `mww_detector_process()`.
4. The frontend turns PCM into 40 mel features (30 ms window, 10 ms step, 125–7500 Hz). The model scores those features. A hit is the sliding-window average rising above cutoff `102` (about 0.4 on a 0–255 scale), and only after at least 100 feature slices so the model can settle.
5. A hit is ignored for 2 seconds after the previous hit (`WAKE_COOLDOWN_MS`).
6. On a valid hit, task `voice_wake_q` runs `after_wake_task`:
   - play the cached beep (`nino_voice_play_wake_chime()`),
   - set the eyes to listening,
   - call `nino_voice_assist_run_query_only()`.

Wake detection is phrase spotting only. It does not record the question and it does not use the energy VAD described below.

---

## 4. VAD — how the question is recorded

Function: `nino_voice_capture_vad_wav()` in `main/voice_assist.c`.

This is a **frame energy detector**, not ESP-SR VADNet and not the wake-word model. Each frame is scored by the average absolute sample value (mean absolute amplitude). A frame is “speech” when that number is above a threshold that moves with the room noise.

### Audio format

| Setting | Value |
| --- | --- |
| Rate | 16 kHz |
| Channels | 1 (mono) |
| Sample width | 16-bit signed PCM |
| Frame | 20 ms = 320 samples = 640 bytes |
| Max clip (normal query) | 10 seconds (`VOICE_QUERY_VAD_MAX_SEC`) |
| Max clip (follow-up listen) | 8 seconds (`MED_ACK_VAD_MAX_SEC`) |
| Output | RIFF WAV, 44-byte header + PCM, kept in RAM |

### Two jobs inside one loop

**Speech vs silence** — is this 20 ms frame voice?

**End of utterance (EOU)** — has the person finished the sentence?

Those are separate. A quiet frame does not end the recording by itself. Recording ends only after enough trailing silence, and only after at least 300 ms of audio has been kept.

### Thresholds

Noise floor starts at `220` and is updated with a slow average of quiet frames.

| Decision | Rule |
| --- | --- |
| Start of speech | energy ≥ max(1800, noise floor × 3) |
| Still speaking, before the peak rule | energy ≥ max(1200, noise floor × 2) |
| End-of-speech line, once recording | energy ≥ max(start threshold, 30% of the loudest frame in this utterance) |

The 30% of peak rule matters on the USB mic. Room noise there often sits above the fixed “continue” energy, so a fixed floor would never see silence. Comparing against the utterance peak lets a quieter tail count as silence even when the absolute level is still high.

Speech must clear the start threshold for **3 frames in a row** (60 ms). One loud click does not open the recording.

### Before speech starts

- Every frame is pushed into a ring of the last **200 ms** (10 frames). That pre-roll is copied into the WAV when speech is confirmed, so the first consonant is not cut off.
- If **6 seconds** pass with no confirmed speech (`VAD_LISTEN_TIMEOUT_MS`), capture returns `ESP_ERR_NOT_FOUND`. Eyes go idle, wake is re-armed, music may resume. Nothing is sent to the PC.
- While waiting, quiet frames update the noise floor. The serial log prints energy, noise, and threshold about once a second (`VAD listen ...`).

### While recording

Every frame is appended to the WAV buffer.

- If energy stays at or above the end threshold, `silence_streak` is cleared. This frame is speech.
- If energy drops below that line, `silence_streak` increments and the noise floor is nudged with this quieter frame.
- The loudest frame so far is stored as `peak_energy`. The end threshold is recomputed from it, so a louder word later in the sentence raises the bar for what counts as silence.

### Adaptive trailing silence

Default stop time is **750 ms** of trailing silence. It is never shorter than **500 ms** and never longer than **1500 ms**.

If the person pauses and then keeps talking (speech → silence → speech), that pause is an intra-utterance pause, not the end:

```text
new timeout = clamp(longest pause so far + 350 ms,  500 ms … 1500 ms)
```

Example: a 400 ms hesitation mid-sentence raises the stop time to 750 ms (400 + 350, which equals the default). A 900 ms hesitation raises it to 1250 ms. A later pause must beat that longer timeout before the clip is closed. Someone who pauses inside a sentence is less likely to be cut off; someone who stops cleanly still ends near 750 ms.

Recording also stops if the buffer hits the max duration (10 s or 8 s). The WAV is still sent in that case, as long as at least one sample was stored.

Serial lines to watch:

- `VAD speech start` — three loud frames in a row; pre-roll copied in
- `EOU resume pause=... adaptive=...` — they paused and resumed; timeout grew
- `VAD trailing ... silence=...ms adaptive=...` — quiet tail, not finished yet
- `EOU end silence=...` — clip closed
- `VAD WAV N ms` — final clip length

### What VAD does not do

It does not know words. It does not run Whisper. The PC’s Whisper path has its own `vad_filter` later; that is a second, independent filter on the already-recorded clip and does not control when the ESP stops the mic.

---

## 5. Sending the clip and receiving the reply

`run_ws_and_queue()` (`voice_assist.c`) is the glue.

1. Run VAD (section 4). Log `latency: VAD capture`.
2. On failure: idle eyes, release the wake lock, unpause music.
3. On success: **release the wake lock immediately** (`nino_voice_wake_release_after_wake()`), before the network call. “Ok Nino” can be detected again while the PC is still thinking. A new query still has to wait until this capture’s hold is clear; the hold is already clear at this point, so a second wake can start another capture. The speaker queue serializes playback.
4. Spawn task `voice_ws`, which calls `nino_voice_ws_exchange()` (`main/voice_ws_client.c`).

WebSocket exchange, one query per connection:

1. Connect to `ws://<pc>:8000/ws/voice` or `.../voice-query` (same handler). Connect wait is up to about 10 s.
2. Send the query WAV as one binary frame.
3. Eyes go to **thinking**.
4. Wait up to 300 s for the server.
5. First message is a **text JSON** frame, for example:

```json
{"prompt_medical_ack": false, "device_id": "...", "eye_expression": "happy"}
```

`prompt_medical_ack: true` means “after this reply, open the mic again.” The name is historical; conversation continue-listen uses the same flag as a medical alarm follow-up.

`eye_expression`, when present, is applied as soon as the JSON arrives (before the WAV finishes downloading).

6. Then one **binary WAV** (possibly several WebSocket fragments). The client waits until the bytes form a valid WAV, then closes the socket.
7. The reply buffer is queued with `nino_main_queue_audio_wav()`.

If the socket fails or the bytes are not a WAV, eyes return to idle and music is unpaused. No partial audio is played.

---

## 6. What the PC does with the WAV

Handler: `_voice_ws_pipeline()` in `server/app.py`. Both `/ws/voice` and `/voice-query` call it.

For each binary frame:

1. Mark a voice query as active (face greeting TTS stays quiet during this).
2. Snapshot the camera: if a face is recognized, that name is the viewer for this turn. If the face drops during continue-listen, the last remembered viewer is reused.
3. `process_voice_wav()` in `server/voice_service.py`:

**Speech-to-text.** Default is local faster-whisper (`tiny`, English, CPU int8) with its own VAD filter (`min_silence_duration_ms` 250). If `STT_PROVIDER=elevenlabs`, Scribe is tried first and Whisper is the fallback. Input must be 16-bit PCM; the server accepts mono or stereo and resamples to 16 kHz if needed. Empty or oversized audio (over 512 KB) is an error.

**Reply text.** The transcript is matched in order. The first hit wins; anything left over goes to the local LLM (Ollama):

| If the transcript looks like… | Reply comes from |
| --- | --- |
| Speaker volume | HTTP call back to the ESP, then a short confirmation |
| Echo of the robot’s own TTS, or garbage STT | “Sorry, I didn’t catch that…” |
| Face-registration phrase | face registration service (sometimes no WAV; a relisten is scheduled) |
| Goodbye | a short farewell; mic stays closed afterward |
| Time-of-day greeting, “how are you”, wellbeing | fixed small-talk lines |
| Clock time | the PC clock |
| Weather | weather service for this device’s location |
| World Cup winner / top scorer / live scores / joke | football or joke helpers |
| Alarm | `alarm_voice`; a reschedule prompt sets `prompt_medical_ack` |
| Servo spin | confirmation, then a delayed `POST /servo/360` |
| “What did we talk about” | memory recap (needs a recognized face) |
| Anything else | Ollama, with that person’s memory block when a face is known |

Conversational paths (`llm`, greetings, jokes, recap, memory, and similar) set `prompt_medical_ack` when `VOICE_CONTINUE_LISTEN` is on (default on) and the user did not say goodbye. That is what reopens the mic.

**Eyes.** `infer_eye_expression_for_response()` picks a tag from the reply text and the path. It is sent in the JSON frame.

**Speech.** The reply string is synthesized (SAPI or the configured TTS provider), resampled to 16-bit mono at the playback rate, and returned as WAV bytes.

4. The server sends the JSON metadata, then the WAV bytes.
5. On a pipeline exception it still tries to speak a short error (“Ollama is unavailable” or a one-line failure), so the robot does not stay silent.

The ESP does not parse the transcript. It only sees the JSON flags and the WAV.

---

## 7. Playback on the robot

`audio_playback_task` in `main/audio_queue.c` is the single speaker consumer.

1. Decode the reply WAV.
2. If the server sent an eye expression, show it for the duration of the clip.
3. Play PCM on the ES8311 speaker. Servo motion can run with the clip unless a touch pause freezes playback.
4. When the clip ends, eyes return to idle.
5. If this reply will **not** reopen the mic, play the done beep (`beep.wav`, same chime as wake).
6. If `prompt_medical_ack` was true, skip a second beep when the done beep already played, then start `prompt_listen_task`:
   - wait 900 ms,
   - optional extra chime only if a done beep was not just played,
   - run the same VAD + WebSocket path again, max 8 seconds, **without** requiring “Ok Nino”.
7. If the mic is not reopening and the user did not pause music, music resumes.

A touch can suspend a server clip mid-way. The remainder, the done beep, and the continue-listen are kept and run when playback resumes.

---

## 8. One turn, with the tasks

```text
wake_detect          always: mic → MicroWakeWord
        │ "Ok Nino"
        ▼
voice_wake_q         beep, listening eyes, run_query_only()
        │
        ▼
(same task)          VAD owns mic  (wake_detect skips reads)
        │ WAV ready; wake re-armed
        ▼
voice_ws             connect, send WAV, thinking eyes, recv JSON+WAV
        │
        ▼
audio playback task  play reply, idle eyes, done beep
        │
        ├─ stop ────────────────────────── wake_detect keeps listening
        │
        └─ prompt_listen (continue)
                 wait → optional beep → VAD again → voice_ws again
```

Latency lines in the ESP log, in order: chime duration, `latency: VAD capture`, WebSocket connect, `Server reply took`, `latency: WS round-trip`. The PC log breaks the server side into `stt_seconds`, `reply_seconds`, and `tts_seconds`.

---

## 9. Files

| File | What it owns |
| --- | --- |
| `main/voice_wake.cpp` | Wake loop, cooldown, after-wake task, mic hold |
| `main/mww_detector.cpp` | “Ok Nino” model and audio frontend |
| `main/mic_input.c` | USB 4-mic vs ES8311 |
| `main/voice_assist.c` | Beep cache, energy VAD, adaptive EOU, WS job, continue-listen |
| `main/voice_ws_client.c` | One WAV up, JSON + WAV down |
| `main/audio_queue.c` | Speaker queue, done beep, follow-up listen |
| `server/app.py` | `/ws/voice` and `/voice-query` |
| `server/voice_service.py` | STT, reply routing, continue-listen flag, TTS WAV |

Related notes that go deeper on one slice: [ADVA_PLAN.md](ADVA_PLAN.md) (why the silence timeout adapts), [USB-4MIC-INTEGRATION.md](USB-4MIC-INTEGRATION.md) (USB mic and exclusive read). The fixed 450 ms silence figure in older notes is no longer what `voice_assist.c` uses; the live stop window is 500–1500 ms, default 750 ms.
