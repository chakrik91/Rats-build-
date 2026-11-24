# Server flow

What the PC does for the robot. The board sends a camera picture and a question recording. The PC decides who is there, what was said, what to answer, and what the eyes should show, then sends speech back.

The server is a web app on port 8000. The browser UI, the voice socket, alarms, and face enrollment all sit on that one process.

---

## 1. What the PC is responsible for

| Job | Where the answer comes from |
| --- | --- |
| See the camera | Pull the board’s live stream, or a local webcam |
| Name the face | A detector plus a stored embedding for each person |
| Read emotion | A cloud vision API, with a local model if the cloud call fails |
| Hear the question | Speech-to-text on the uploaded WAV |
| Answer | A fixed route when the sentence matches, otherwise a local language model |
| Remember | Optional database of people, recent turns, facts, and a daily summary |
| Speak | Text-to-speech, then a WAV posted to the board or returned on the voice socket |
| Wake the house | Alarms stored on the PC, fired by the PC clock, spoken through the board |
| Find the robot | mDNS and UDP discovery on the LAN |

The board never sends text. The PC never needs the board to understand a sentence.

---

## 2. Startup

When the process starts it does this in order:

1. Load settings from the environment and from a local env file if one is present.
2. Apply face-recognition and face-enrollment settings.
3. Open conversation memory if a database address is configured. Without it, voice still works; recap and long-term facts do not.
4. Start the alarm clock. Pending alarms are read from disk.
5. Work out the board speaker address. If the camera source is the board’s stream, the speaker address can be derived from that same host.
6. Reload the device list, then run one LAN discovery pass and drop robots that no longer answer.
7. Open a camera reader for each known device.
8. Keep discovering in the background so a robot that appears later is picked up.
9. Point greeting speech and face enrollment at the device selected in the UI.
10. Start a vision loop for every device, about twice a second. Emotion, greetings, and enrollment run there even when no browser is open.
11. Warm the language model in the background so the first question is not paying the load cost.

On the way down, discovery, memory, alarms, and the cameras are stopped.

Settings precedence is: command-line flags, then environment variables, then an optional local config. Secrets stay in the env file and are not part of the flow itself.

---

## 3. Finding the robot

Each device record keeps a display name, the camera stream address, the speaker address, a location (latitude, longitude, and a place name), and the Wi-Fi network it last reported.

Discovery uses the board’s mDNS name and its UDP discover reply. A scan at startup replaces the previous inventory with whoever actually answers. The UI can force another scan, pick which robot is “the” one on screen, and store a location. Weather answers use that location. A robot with no coordinates cannot answer a weather question.

Voice and vision are keyed by device id. A follow-up question can reuse the person who was recognized on that same device for about two minutes, so a brief look-away does not forget them.

---

## 4. Vision loop

For each device, on a timer:

```text
Latest camera frame
        │
        ▼
Find faces, compute an embedding, compare with people on disk
        │
        ├─ known person  → name, hold them as the current viewer
        ├─ unknown face  → maybe start voice enrollment
        └─ no face       → clear the live identity after a short gap
        │
        ▼
Emotion label on the main face
        │
        ├─ stable happy / sad / surprised → tell the eyes, if nothing else is speaking
        └─ neutral or no face → eyes back to idle
        │
        ▼
If this is a known person who has not been greeted yet this visit
        → speak a short greeting on the board speaker
```

### Recognition

Detection looks for a plausible frontal face (eyes and mouth have to sit in a human layout; a texture blob is rejected). The match is an embedding distance against the people enrolled earlier.

A score has to clear a threshold, and it has to win by a margin over the next person, across several frames, before the name is committed. A near miss stays unknown so the wrong name is not spoken. The current viewer is held across short gaps so a blink does not reset the greeting.

Samples and embeddings live on disk. The web page can capture a new person or rebuild the model from samples already stored.

### Emotion and the eyes

The preferred reader is the cloud vision API. If that call fails or is not configured, a local model supplies the label. The mapping onto the eyes is small:

| Camera label | Eye face |
| --- | --- |
| Happy | Happy |
| Sad or angry | Sad |
| Surprise or fear | Surprised |
| Neutral, or anything else | Idle |

The same label has to hold for about 1.5–2 seconds before the eyes change. They then stay on that face for about 4–5 seconds. During that hold, newer labels are ignored.

Emotion loses to anything that is already speaking. A voice question, an alarm, and a greeting all outrank it. After a voice turn, emotion stays quiet for several seconds so it does not wipe the expression that belonged to the reply.

### Greeting when someone walks up

The first solid recognition of a person can speak without a wake word. The line is a time-of-day hello. If yesterday’s summary exists for that person, the greeting can mention what you talked about and ask a short follow-up. The greeting is logged like any other turn. While a voice question is in progress, this greeting stays silent so the two do not talk at once.

### Enrolling a stranger by voice

If enrollment is enabled and an unknown face stays in frame for about three seconds (and for enough frames in a row):

1. The PC speaks a short prompt asking for a name, on the board speaker, and tells the board to open the microphone afterward.
2. The next voice clip is treated as the name, not as a normal question.
3. Phrases like “my name is …”, “I’m …”, or “call me …” are accepted. A bare very short name, a greeting, an echo of the prompt, or a cancel is not stored.
4. A good name captures samples from the live frame using the same pipeline as the web page, then trains.
5. If the clip was empty or only an echo, the PC asks again, up to a small retry count, by reopening the mic. Echo retries can be a beep only, so the prompt is not heard twice and transcribed as the name.
6. “No” or a cancel phrase stops the attempt. If the face becomes a known person mid-prompt, the attempt is dropped.

Someone who already matches a stored person is not enrolled again under a new name. The same person can refresh their own samples.

---

## 5. A voice turn

The board opens `ws://<this-pc>:8000/voice-query` or `/ws/voice`. Both enter the same pipeline. One connection carries one question: a binary WAV up, a JSON note down, then a reply WAV down.

```text
Binary WAV from the board
        │
        ▼
Who is in the camera right now?
        │
        ▼
Speech-to-text
        │
        ▼
First matching route wins (volume, alarm, weather, …)
        │  if nothing matches, the language model answers
        ▼
Pick an eye expression from the reply
        │
        ▼
Synthesize speech, resample to 16 kHz mono
        │
        ▼
JSON metadata, then the WAV, back on the same socket
```

The board does not parse the transcript. It only sees the JSON flags and the audio.

### Who is speaking

At the start of the clip the PC snapshots the camera. A recognized face becomes the viewer for this turn. If the face drops during a follow-up listen, the last viewer for that device is reused for a couple of minutes. Recap and personal memory require that name. A question asked to an empty room does not get someone else’s history.

### Speech-to-text

The default engine is a local Whisper model, English, with its own silence filter on the already-recorded clip. That filter does not decide when the board stops the microphone. If a cloud scribe key is configured, that engine is tried first and Whisper is the fallback.

The clip must be 16-bit PCM. Mono or stereo is accepted and brought to 16 kHz. Empty audio, or a clip over the size cap (512 KB), is rejected.

### Choosing the reply

Routes are tried in this order. The first one that matches owns the turn. Later checks are skipped.

| If the transcript looks like… | What happens |
| --- | --- |
| A volume command | The PC calls the board’s volume address and confirms in one sentence |
| An echo of the robot’s own voice, or garbage text | “Sorry, I didn’t catch that.” Enrollment prompts are exempt so a name is not thrown away |
| A name during enrollment | The enrollment service handles it. Sometimes there is no spoken WAV; the mic is simply opened again |
| Goodbye | A short farewell. The microphone stays closed |
| Good morning / evening, “how are you”, a wellbeing reply | A fixed small-talk line, using the person’s name when known |
| What time is it | The PC’s clock |
| Weather | The forecast for this device’s saved location, or a spoken explanation if location or the weather service is missing |
| World Cup winner or top scorer for a year | A lookup, with a spoken failure if the result cannot be read |
| Who is your World Cup favourite, or a football joke | A fixed line |
| Tell me a joke | The next joke from a shuffled deck, so the same joke does not repeat immediately |
| A live score | The football feed, or a spoken explanation if the key is missing or the feed is down |
| A vague football question | A hint listing what can actually be answered |
| Set, list, or cancel an alarm | The alarm book. A medical reminder that still needs a yes or no sets the flag that reopens the microphone |
| Spin / turn around | A short confirmation, then a delayed call to the board’s full-spin address |
| What did we talk about | Memory recap. Without a recognized face, the answer says it cannot tell who is asking |
| Anything else | The language model, with that person’s recent turns and stored facts in the prompt |

Volume and the “I didn’t catch that” path return before memory is loaded. Everything after that can use the person’s context.

### Conversation follow-up

For a real conversation (the language model, greetings, jokes, recap, and similar paths), the JSON flag that reopens the microphone is set when follow-up listening is enabled, which it is by default. Goodbye clears it. Volume, alarms, and the spin confirmation do not reopen the mic, except a medical alarm that is explicitly waiting for yes or no.

The board then records again with no wake word and sends a new clip. That clip is a new turn through this same pipeline.

### Eyes on the reply

Only some paths get an expression tag. Volume, alarms, and the spin confirmation leave the tag empty, and the board stays idle for that clip.

For a language-model reply, words in what the person said count more than words in the answer. The tag is one of happy, tired, curious, sad, surprised, or recalling. A tie prefers surprise, then sad, tired, recalling, happy, and curious. Recap leans toward recalling. The tag is in the JSON frame, so the eyes change before the WAV has finished arriving.

### Speech synthesis

The reply string is cleaned first: emoji, markdown, and labels like “NiNO says” are stripped so they are not read aloud. The name NiNO inside a normal sentence is kept.

The engine is the configured one: a cloud voice, the Windows speech API, or a local synthesizer. The bytes are resampled to 16-bit mono at the playback rate the board expects (16 kHz). Long answers are split on sentence boundaries into clips that fit the board’s 384 KB playback limit, and those clips are sent in order when the path is a direct post rather than the single voice-socket reply.

If the pipeline throws, the PC still tries to say a short failure (“the model is unavailable”, or one line about what broke) so the robot does not go silent.

### Timing log

Each turn records how long transcription, the reply, and synthesis took, plus who was seen, which route fired, and whether the mic will reopen. Those rows are available from the status API and are appended to the latency log on disk.

---

## 6. Speaking without a question

Some audio is pushed to the board with an HTTP post of a WAV, not over the voice socket.

| Cause | What is posted |
| --- | --- |
| Someone is recognized | The greeting, and sometimes a prompt to keep talking |
| An alarm comes due | The reminder text, synthesized at 16 kHz so it fits the board limit |
| Enrollment | The “what is your name” prompt, with the header that reopens the mic |
| Emotion | Not audio. A separate eye-expression call |

Headers on that post match what the board understands: eye face, the voice address the board should call next, and whether to listen again when the clip ends.

The speaker address is the device’s own address when several robots are known, and falls back to the single address in the environment.

---

## 7. Alarms

Alarms live on the PC, in a JSON book on disk, and fire from the PC clock.

A spoken “remind me…” is classified:

- Ordinary reminders are normal priority.
- Medicine and similar wording is medical: highest priority, and it must be acknowledged.

When the time arrives the PC synthesizes the line and posts it to the board. A medical alarm says the person’s name when the camera knew who set it. After it plays, the board is told to listen. “Yes” acknowledges it. “No”, or no answer, can leave it pending so it speaks again after the repeat interval. The web page can also acknowledge or delete any alarm, including a medical yes or no.

The scheduler walks pending alarms in priority order, then by time. Medical ones go out before a normal reminder that happens to be due at the same moment.

Alarm commands are not written into conversation memory.

---

## 8. Memory

Optional. If no database is configured, the voice routes that need history say so by lacking context, and the rest of the pipeline still runs.

What is stored, per person, tied to the face name:

- The user record.
- Recent conversation turns.
- Long-term facts (favourite food, and similar), with an importance cutoff.
- A daily summary, usually built just after midnight.

On a turn, the PC loads the last several lines and the top facts that relate to the question, and puts them in the language-model prompt. A spoken preference (“my favourite food is …”) can be saved immediately, before the slower extraction pass. Extraction skips jokes, alarm commands, echoes of the robot’s own voice, and questions that are not facts.

Recap (“what did we talk about?”, “what did I say about …”) reads that history. If the question assumes a topic that was never discussed, the answer says it was not in the history instead of inventing it. Recap is refused when no face is recognized, so one person’s log is not read to someone else.

The morning greeting can use yesterday’s summary. The summary job is scheduled at a configured local time.

---

## 9. Language model

The default is a local Ollama model (`qwen2.5:1.5b`). On a machine with a GPU listener, the server prefers that port over the ordinary CPU port if the configured address does not actually have the model.

The prompt carries the person’s name, recent turns, and stored facts. If the model answers as if the recognized speaker were someone else in the room (“you should ask Avery”), that reply is thrown away and tried once more. If the retry still does it, a safe short reply is used instead.

Replies are kept short for speech. The word cap depends on the route; recap can be a little longer than a normal chat line.

---

## 10. The web page

Open `http://localhost:8000`.

| Area | What it does |
| --- | --- |
| Live picture | The camera with face boxes and emotion labels drawn on |
| Camera source | Switch between the board stream and a local webcam, and set rotation |
| People | Type a name, capture samples, train; or rebuild from samples already on disk |
| Alarms | List, acknowledge a medical reminder, delete one or all |
| Devices | Who was discovered, which one is selected, location |
| Servo | Read position, record a move, send a goal, play the recording back. These calls are forwarded to the same paths on the board |
| Status | Camera, faces, emotion, speech engines, alarms, memory |

Other reads: a snapshot, the latency log, and memory stats.

---

## 11. What the PC never does

- It does not run the wake word. “Ok Nino” is entirely on the board.
- It does not decide when the person stopped talking. The board closes the clip, then the PC transcribes whatever arrived.
- It does not drive the pan servo to center a face. That loop stays on the board. The PC only asks for a full spin, or forwards record and play from the web page.
- It does not play audio on the PC speakers as the primary path. Speech is sent to the board.
- The current voice routes do not resolve a song title or start the board’s music stream. Music is a board feature a LAN client can drive directly.

---

## 12. One question, both sides

```text
Board                          PC
─────                          ──
"Ok Nino" on the USB mic
beep, listening eyes
record until a quiet tail
        │
        │  WAV
        ├─────────────────────►  name the face
        │                        transcribe
        │                        route or language model
        │                        choose an eye face
        │                        synthesize 16 kHz WAV
        │  JSON, then WAV
        ◄──────────────────────┤
thinking eyes, then the expression
play the reply
        │
        ├─ done beep, arm wake word
        │
        └─ or listen again with no wake word
           and send the next WAV the same way
```

Alarms and walk-up greetings skip the left column. The PC posts a WAV when its own clock or the camera says it is time, and the board plays it on the same speaker queue as a reply.
