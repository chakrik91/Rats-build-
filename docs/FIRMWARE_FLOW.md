# Firmware flow

What the ESP32-P4 board does from power-on through a spoken conversation, and how it hands work to the PC. The board records sound, shows eyes, moves the head, and streams the camera. It does not transcribe speech, recognize who someone is, or write the reply.

---

## 1. What lives on the board

| Piece | Role |
| --- | --- |
| USB camera on the high-speed hub | Live picture for the PC, and for onboard face centering |
| USB 4-mic on the GPIO header | The only microphone used for wake and questions |
| Speaker codec | Playback only: chimes, replies, alarms, touch warning, music |
| Two small OLED eyes | Idle, listening, thinking, and emotion faces |
| Two Dynamixel servos | Pan and tilt; face tracking, speech motion, recorded moves, a full spin |
| Capacitive touch | A warning sound that can interrupt whatever is playing |
| Wi-Fi | Home network, plus a setup network when nothing is saved |
| BLE | Phone app writes the home Wi-Fi name and password |

The PC and the board must be on the same LAN. The board’s web server is on port 80. There is no login; the network is trusted.

---

## 2. Boot

Eyes come up first so the robot looks alive before the camera and the microphone are ready.

1. Flash storage is opened. Saved Wi-Fi, the PC voice address, speaker volume, and the device name are read back.
2. The co-processor link used for BLE is started when that feature is built in.
3. Both eye screens start on the idle face.
4. The voice address is loaded. If it is empty, a later wake word is heard and ignored.
5. Wi-Fi starts in the saved mode. With no home network saved, the board advertises its own setup network and starts BLE so a phone can provision it.
6. The speaker starts and the saved volume is applied. One task owns the speaker for the rest of the run.
7. If the board is already on the home network, it plays the Wi-Fi-connected chime.
8. Touch sensing starts and calibrates. Hands should be away during that moment.
9. LAN discovery and a small TCP message port start, then the serial console and the web server.
10. The USB host starts. The camera hub and the microphone header share one host stack.
11. The servo adapter is opened. It shares the camera hub, so the first scan can fail while the camera is still enumerating. It retries.
12. Face centering starts. It stays quiet until tracking is turned on.
13. The camera stream opens.
14. BLE provisioning is offered when the board still needs a network.
15. Wake-word listening is delayed about five seconds so the camera’s USB memory and the wake model do not start in the same instant.
16. A boot-greeting task decides which clip to play.

Boot greeting:

| Situation | What you hear |
| --- | --- |
| No home network saved | A prompt to open the phone app |
| Home network connects within about a minute | The hello-home greeting, after the Wi-Fi chime |
| The saved network rejects the board or cannot be found | A failure clip, once per attempt |

---

## 3. Staying on the network

Saved items:

| Saved item | Used for |
| --- | --- |
| Wi-Fi mode, name, password | Reconnect after power loss |
| Voice WebSocket address | Where questions are sent |
| Speaker volume | Restored on the next boot |
| Device name | Discovery and the phone app |

Ways to set the home network:

- Phone app over BLE. The advertised name is **NINO - HOME**.
- Serial console: connect with the network name and password.
- HTTP: post the name and password to the Wi-Fi config address on the board.

When the board gets an address:

- It announces itself with mDNS as **NINO-HOME.local**, service `_nino._tcp`, web port 80.
- It answers a UDP discover packet on the LAN multicast address with its MAC, name, IP, and message port 8888.
- It plays the Wi-Fi-connected chime.

When the link drops, mDNS stops, the failure clip can play once, and the board tries again after a few seconds.

The voice address is separate from Wi-Fi. A typical value is `ws://<pc-address>:8000/voice-query`. It is saved from the serial command that names the PC and port, and the PC can also push the address on a later playback request so the board learns where to call back.

---

## 4. Camera and onboard face centering

The camera is a USB webcam. The board prefers a 640×480 motion-JPEG picture at about 15 frames a second. The newest frame is kept under a lock. Two consumers read it:

- The web server, for a single snapshot and for a continuous motion-JPEG stream. The PC pulls that stream and does recognition there.
- The onboard tracker. It does not know names. It only finds a face box and turns the pan servo so the face sits near the middle of the picture.

Tracking details:

- Detection runs about four times a second, not on every frame, so the camera stream keeps its time.
- Tracking is off until someone turns it on from the serial console or from HTTP.
- The pan move is smoothed, with a dead zone so the head does not hunt.
- If the camera stalls, the last face is reused for up to about eight seconds.
- Tracking pauses while a speech move, a full spin, a demo sweep, or a recorded playback owns the servos.

The two servos are Dynamixel AX motors. One is tilt, one is pan. Center is the middle of the 0–1023 position range. The pan motor is the one that centers a face and the one that does a full spin.

---

## 5. The speaker

One playback task is the only writer to the speaker. Everything else queues a clip.

Two queues:

- A touch queue. A touch warning jumps the line and can pause a server clip in the middle.
- A normal queue. Boot chimes, voice replies, alarms, and demo clips wait here.

A server clip that was interrupted by touch is resumed from the saved sample, then its end-of-clip chime and any follow-up listen still happen.

Volume is 0–100, saved across reboots, and can be changed from the serial console, from HTTP, or by a spoken command that the PC turns into an HTTP call.

Clips posted as a whole WAV are limited to 384 KB. That is enough for a spoken reply. A song does not use this path. Music uses its own stream, described later.

While a clip plays, the head can nod and turn with the audio unless a touch warning or another servo owner has frozen motion.

---

## 6. A spoken question, end to end

This is the main loop. The board never sees the words. It sends a recording and plays whatever audio comes back.

```text
You say "Ok Nino"
        │
        ▼
Wake model, always listening on the USB mic
        │
        ▼
Short beep, eyes switch to listening
        │
        ▼
Energy detector records the question
        │  stops on a quiet tail, or at 10 seconds
        ▼
One WAV goes to the PC over WebSocket
        │  eyes switch to thinking
        ▼
PC sends a short JSON note, then a reply WAV
        │
        ▼
Speaker plays the reply, eyes follow the note
        │
        ├─ most commands: a done beep, wake is already armed
        │
        └─ a conversation: the mic opens again with no wake word
           until goodbye or the listen window ends
```

If the saved PC address is empty, the wake is logged and nothing is recorded.

### Who holds the microphone

Only one reader at a time. The wake loop and the recorder both read the same mic. While the question is being recorded, the wake loop is told not to read. Music is ducked for that interval. When recording ends, the wake model is cleared so the phrase just spoken does not fire again, and the wake loop may listen during the PC’s thinking time. Playback itself is serialized, so two replies do not talk over each other.

The onboard codec is not used as a microphone. If the USB mic is missing, there is no wake.

### Wake

The wake task reads 10 ms slices of 16 kHz mono audio and feeds a small on-device model trained for **Ok Nino**.

The model does not start scoring a hit until it has seen enough audio to settle (about one second of feature slices). A hit is the recent score average crossing about 0.4. Another hit is ignored for two seconds.

On a hit, a short task plays the beep, sets the eyes to listening, and starts the recorder. The wake model does not record the question.

### Recording the question

This is loudness, not speech recognition. Each 20 ms frame is scored by average absolute level. A frame counts as speech only when it is clearly above a floor that follows the room.

| Rule | Value |
| --- | --- |
| Format | 16 kHz, mono, 16-bit, wrapped as a WAV |
| Frame | 20 ms |
| Speech must hold | 3 frames in a row (60 ms) before recording opens |
| Pre-roll | The last 200 ms is kept and copied in, so the first consonant is not cut |
| No speech at all | Give up after 6 seconds. Eyes return to idle. Nothing is sent |
| Longest question | 10 seconds. A follow-up listen without the wake word is capped at 8 seconds |
| Default quiet tail | 750 ms |
| Quiet tail limits | Never under 500 ms, never over 1500 ms |

The quiet-tail timer adapts. If the person pauses and then keeps talking, that pause is treated as a hesitation inside the sentence. The stop time becomes that pause plus 350 ms, still clamped between 500 and 1500 ms. A later pause has to beat the longer timer. A clean stop still ends near 750 ms.

Once speech has started, “quiet” is also compared with the loudest frame in this utterance (about 30% of that peak). The USB mic’s room noise is often loud in absolute terms, so a fixed floor would never see the end of the sentence.

A click is not speech. The noise floor is learned from quiet frames while waiting.

### The trip to the PC

One WebSocket connection per question.

1. Connect to the saved address. Both `/voice-query` and `/ws/voice` on the PC are the same pipeline. Connect wait is about 10 seconds.
2. Send the WAV as one binary message.
3. Eyes go to thinking.
4. Wait up to about five minutes.
5. The first reply message is text JSON, for example a medical-ack flag, the device id, and an eye expression.
6. The eye expression is applied as soon as that JSON arrives, before the audio has finished downloading.
7. Then one binary WAV, possibly in several socket pieces. The board waits until the bytes are a valid WAV, then closes the socket.
8. The reply is queued on the speaker.

If the socket fails or the bytes are not a WAV, the eyes return to idle, music may resume, and nothing partial is played.

The JSON flag that reopens the microphone is named for medical alarms. Conversation follow-ups use the same flag. When it is true, the board listens again after the reply without requiring “Ok Nino”.

### Playback of the reply

1. Decode the WAV.
2. If an eye expression was sent, hold it for the clip.
3. Play it. Head motion can run with the clip.
4. When the clip ends, eyes return to idle.
5. If the microphone will not reopen, play the done beep (the same chime as the wake beep).
6. If the microphone should reopen, wait about 900 ms, skip a second beep when the done beep already played, then record again with the same loudness detector, capped at 8 seconds.
7. If the user had not paused music, and the microphone is not reopening, music resumes.

A touch during the reply pauses it, plays the warning, then continues the reply from the saved point.

---

## 7. Eyes

Two 128×96 color OLEDs. The animator is its own task. A state change is applied on the next frame and does not block the caller.

States the board can show include idle, listening, thinking, happy, tired, curious, sad, surprised, recalling, mad, a medical capsule, and a set of static picture faces (smile, sparkle, pencil, radio, television, bulb, robot, and a few others).

Who sets them:

| Moment | Face |
| --- | --- |
| Boot, and after a reply ends | Idle |
| Wake beep through the end of the question | Listening |
| Waiting on the PC | Thinking |
| JSON from the PC, or a direct eye command | The named expression, for the reply |
| Camera emotion from the PC | Happy, sad, or surprised, only when nothing more important is speaking |
| Serial console | Whatever state is typed |

An unknown or empty expression name means idle for that reply. The board does not invent an emotion from the audio.

---

## 8. Touch

The capacitive sensor is polled about every 30 ms. A touch must hold for about 300 ms. There is a half-second cooldown between triggers.

On a real touch the warning clip is queued at high priority. If a server reply is in the middle of playing, it pauses, the warning plays with a small nod, and the reply resumes from the saved sample. The done beep and a follow-up listen are not dropped.

---

## 9. Servos beyond face tracking

| Move | How it starts |
| --- | --- |
| Pan to center a face | Onboard tracker, when enabled |
| Nod and turn during speech | The playback task, for the length of the clip |
| Full spin of the pan motor | Voice (the PC calls the board), HTTP, or the serial console |
| Demo sweep | Serial console, until it is stopped |
| Record and play | HTTP from the PC web page |

Record mode can drop torque so the head can be posed by hand, samples the positions, and later plays that sequence back. While a recording or a playback is running, face tracking and the speech nod stay out of the way. The full spin also takes ownership and then returns the pan motor toward center.

---

## 10. Music

Music is not a WAV dropped into the reply queue. A song is too long for the 384 KB reply limit, and the reply path closes the speaker after each clip.

Two ways in:

- HTTP play, with a JSON body that contains a stream address. The board opens that address itself, reads a WAV header, and pulls audio until the socket closes. It does not trust the length fields in the header.
- A WebSocket on the board. A client can push raw audio, or send start, pause, resume, and stop. Status frames report idle, buffering, playing, or paused, plus buffer depth and position.

There is also pause, resume, stop, and status over HTTP.

Speech always wins. Wake and the question recorder duck the music. A user pause is sticky: the voice path will not resume a song the person paused on purpose. When the reply is finished and the mic is not reopening, an automatically ducked song continues.

The current PC voice pipeline does not resolve song names or open these music addresses. A client on the LAN can.

---

## 11. What the board serves

All of this is on port 80.

| Method | Path | What it does |
| --- | --- | --- |
| GET | `/` | A short page and a snapshot preview |
| GET | `/stream`, `/stream.mjpeg`, `/view` | The live camera |
| GET | `/snapshot.jpg` | One JPEG |
| POST | `/play_wav` | Queue a spoken clip. Optional headers set the eye face, the voice address, and whether to listen again afterward |
| POST | `/servo/360` | Full spin |
| GET, POST | `/servo/position`, `/servo/record`, `/servo/goal`, `/servo/play` | Read pose, record, move, play back |
| GET, POST | `/speaker/volume`, `/volume` | Volume 0–100 |
| GET | `/status` | Wi-Fi, volume, name, firmware |
| GET | `/ws/status` | Live status frames |
| GET | `/ws/audio-stream` | Music push |
| POST | `/music/play` | Start a pull stream from a URL |
| POST | `/music/stop`, `/music/pause`, `/music/resume` | Transport |
| GET | `/music/status` | Playing, paused, buffer, position |
| GET, POST | `/face/track` | Tracking on, off, or status |
| POST | `/eye/expression` | Set the eyes directly |
| POST | `/demo` | Play the stored demo |
| GET, POST | `/api/wifi/config`, GET `/api/wifi/status` | Provisioning |
| GET, POST | `/device/name` | The name used in discovery |

Playback headers the PC sends:

| Header | Effect |
| --- | --- |
| Eye expression | Show that face for the clip |
| Voice WebSocket address | Save where the next question should go |
| Prompt acknowledgement | After the clip, open the mic with no wake word |
| Prompt-ack chime | Whether that reopen should beep |

---

## 12. Serial console

The prompt is `usb_cam>`.

| Command | Effect |
| --- | --- |
| `wifi mode`, `wifi connect`, `wifi status` | Network |
| `voice connect <pc-ip> [port]` | Save the voice address |
| `voice wake on` or `off`, `voice status` | Wake word |
| `speaker volume` | 0–100 |
| `eye <state>` | Eyes |
| `track on`, `off`, `status`, `hon` | Face centering and the demo sweep |
| `360`, `hstop` | Spin, and stop the sweep |

---

## 13. What the board never does

- It does not turn speech into text.
- It does not choose the reply.
- It does not match a face to a name. Naming happens on the PC.
- It does not store conversation memory or alarms. It only plays what the PC sends when an alarm is due.
- It does not run the language model.

The contract with the PC is: camera stream out, question WAV out, reply WAV and a few headers back in.
