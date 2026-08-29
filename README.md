# AlekblBeat-ESP32
​AlekBeat-ESP32 is an embedded audio hardware project designed to install directly inside an acoustic guitar body, adding real-time, built-in percussive elements (kick drum and snare) to standard play. The system synthesizes percussion on demand and passively blends the audio with the guitar's native pickup signal, producing a rich, multi-layered performance through a single standard instrument output jack.
​Key Features
•​Built-in Percussion Engine: ESP32-driven sound module configured to synthesize digital kick drum and snare sounds triggered during play.
•​Unified Single-Jack Output: Combines guitar and drum signals inside the guitar body, eliminating the need for extra external output jacks or dual cables.
•​Passive Audio Mixing: Uses a 2-channel stereo passive audio mixing board to cleanly sum the ESP32 synthesized drum output and the guitar pickup output into 1 mixed output line without signal bleed.
•​Single Power System: Powered by a standard 9V battery utilizing voltage step-down regulation to supply both the ESP32 microcontroller board and the active guitar pickup system simultaneously.
​Hardware Architecture
•​Microcontroller: ESP32 Development Module (audio synthesis and trigger processing)
•​Audio Mixing: 2-Channel Passive Audio Mixer Board (2-Way Input, 1-Way Mixed Output)
•​Signal Source: Standard Acoustic Guitar Pickup (Piezo or Active Transducer)
•​Power Source: 1x 9V Battery (with power distribution to pickup and regulated step-down for ESP32)
•​Output: 1/4" (6.35mm) Mono Instrument Output Jack