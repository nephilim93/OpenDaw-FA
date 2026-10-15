# Modular patching eval - proposed cases

40 cases. `start`: empty = new project; init = a Modular Synth with the init patch; warm-pad = a fixture pad patch (detuned saws, LFO, reverb).

| id | tags | start | played as | checks |
|---|---|---|---|---|
| seed-pad | pad, seed | empty | notes [57, 60, 64, 67] for 2.5s | audible, pitch_tracks, min_attack_s=0.15, macro_named="bright" |
| seed-darker | edit, seed | warm-pad | notes [57, 60, 64, 67] for 2.5s | audible, centroid_ratio_vs_start={"max": 0.85}, has_module=["lfo"] |
| seed-acid | sequenced, seed | empty | song plays 8s, no notes | audible, plays_without_notes, has_module=["step_sequencer", "delay"], has_param=[{"type": "delay", "param": "sync", "value": "1/8d"}] |
| seed-pluck | pluck, seed | empty | notes [60] for 1.5s | audible, pitch_tracks, decays, min_centroid=1800 |
| bass-sub | bass | empty | notes [36] for 1.0s | audible, pitch_tracks, max_centroid=350 |
| bass-reese | bass | empty | notes [36] for 2.0s | audible, pitch_tracks, max_centroid=2000, has_module=["lfo"] |
| bass-pluck | bass | empty | notes [40] for 1.0s | audible, pitch_tracks, decays |
| bass-acid-poly | bass | empty | notes [45] for 1.0s | audible, pitch_tracks, has_param=[{"type": "voice_in", "param": "glide", "min": 0.01}] |
| bass-fm | bass, hard | empty | notes [40] for 1.0s | audible, pitch_tracks, min_vco=2, fm_used |
| pad-dark-ambient | pad | empty | notes [48, 55, 60] for 3.0s | audible, pitch_tracks, min_attack_s=0.4, max_centroid=1500, has_module=["reverb"] |
| pad-strings | pad | empty | notes [60, 64, 67] for 2.5s | audible, pitch_tracks, min_attack_s=0.1, has_module=["lfo"] |
| pad-evolving | pad | empty | notes [57, 64] for 5.0s | audible, pitch_tracks, has_module=["lfo"] |
| pad-bright-supersaw | pad, hard | empty | notes [60, 64, 67] for 2.0s | audible, pitch_tracks, min_vco=3, min_centroid=2000 |
| pad-noise | pad | empty | notes [60, 67] for 2.5s | audible, pitch_tracks, has_module=["noise"] |
| lead-square | lead | empty | notes [72] for 1.0s | audible, pitch_tracks, has_param=[{"type": "vco", "param": "wave", "value": "square"}] |
| lead-vibrato-mw | lead | empty | notes [69] for 1.5s | audible, pitch_tracks, mod_wheel_used |
| lead-mono-glide | lead | empty | notes [67] for 1.0s | audible, pitch_tracks, has_param=[{"type": "voice_in", "param": "glide", "min": 0.02}] |
| lead-sync-ish | lead, hard | empty | notes [72] for 1.0s | audible, pitch_tracks, min_centroid=1500, has_param=[{"type": "vcf", "param": "resonance", "min": 0.5}] |
| keys-ep | keys | empty | notes [60, 64, 67] for 1.5s | audible, pitch_tracks, decays, has_module=["lfo"] |
| keys-bell | keys, hard | empty | notes [84] for 2.0s | audible, pitch_tracks, decays, min_centroid=1500 |
| keys-organ | keys | empty | notes [60, 64, 67] for 1.0s | audible, pitch_tracks, max_attack_s=0.03, sustains |
| pluck-short | pluck | empty | notes [72] for 1.0s | audible, pitch_tracks, decays |
| pluck-delay | pluck | empty | notes [67] for 1.0s | audible, pitch_tracks, decays, has_param=[{"type": "delay", "param": "ping_pong", "value": "on"}] |
| fx-riser | fx | empty | notes [60] for 3.0s | audible, has_module=["noise"] |
| fx-wind | fx | empty | notes [60] for 3.0s | audible, has_module=["noise", "lfo"] |
| drone-generative | drone | empty | song plays 8s, no notes | audible, plays_without_notes |
| drone-sh | drone, hard | empty | song plays 8s, no notes | audible, plays_without_notes, has_module=["sample_hold", "quantizer"], has_param=[{"type": "quantizer", "param": "scale", "value": "dorian"}] |
| seq-berlin | sequenced | empty | song plays 8s, no notes | audible, plays_without_notes, has_module=["step_sequencer"], has_param=[{"type": "clock", "param": "division", "value": "1/16"}] |
| seq-swing | sequenced | empty | song plays 8s, no notes | audible, plays_without_notes, has_param=[{"type": "clock", "param": "swing", "min": 0.1}] |
| seq-triplet | sequenced | empty | song plays 8s, no notes | audible, plays_without_notes, has_module=["quantizer"] |
| seq-random | sequenced, hard | empty | song plays 8s, no notes | audible, plays_without_notes, has_module=["random", "quantizer"] |
| seq-poly | sequenced | empty | song plays 8s, no notes | audible, plays_without_notes, has_module=["reverb"] |
| edit-brighter | edit | init | notes [60] for 1.5s | audible, centroid_ratio_vs_start={"min": 1.2} |
| edit-slow-attack | edit | init | notes [60] for 2.0s | audible, min_attack_s=0.3 |
| edit-add-delay | edit | init | notes [60] for 1.0s | audible, has_module=["delay"] |
| edit-remove-reverb | edit | warm-pad | notes [57, 60, 64] for 2.0s | audible, lacks_module=["reverb"] |
| edit-macro | edit | init | notes [60] for 1.5s | audible, macro_named="cutoff", macro_count_min=2 |
| edit-detune | edit | init | notes [60] for 1.5s | audible, pitch_tracks, min_vco=2 |
| neg-no-change | negative | init | notes [60] for 1.0s | patch_unchanged |
| neg-vst-request | negative | empty | notes [60] for 1.0s | no_modular_created |

## seed-pad

````text
Create a MIDI track with a warm detuned pad on the Modular Synth, with a Brightness macro.
````

## seed-darker

````text
Make it darker and add slow filter movement.
````

## seed-acid

````text
On a new track, build an acid bassline in A minor from the step sequencer, with a dotted-eighth delay.
````

## seed-pluck

````text
Give me a bright plucky lead for arpeggios on a new MIDI track.
````

## bass-sub

````text
Make a clean sub bass I can play under a kick.
````

## bass-reese

````text
I want a gritty reese bass: two detuned saws through a lowpass that slowly moves.
````

## bass-pluck

````text
Funky plucked bass with a quick filter envelope, like a synth slap bass.
````

## bass-acid-poly

````text
303-style acid bass I can play from my keyboard, with glide between notes.
````

## bass-fm

````text
Metallic FM bass: one oscillator modulating another's frequency.
````

## pad-dark-ambient

````text
A dark, slow ambient pad with lots of reverb for a film score.
````

## pad-strings

````text
Lush string-machine pad, 70s style, with gentle vibrato.
````

## pad-evolving

````text
An evolving pad where the tone keeps shifting on its own over several seconds.
````

## pad-bright-supersaw

````text
Big bright trance supersaw chord stack.
````

## pad-noise

````text
Airy breathy pad with some filtered noise mixed in.
````

## lead-square

````text
Classic square-wave chiptune lead.
````

## lead-vibrato-mw

````text
Soaring lead with vibrato I can bring in with the mod wheel.
````

## lead-mono-glide

````text
Smooth portamento lead that slides between notes.
````

## lead-sync-ish

````text
Aggressive screaming lead with a resonant filter.
````

## keys-ep

````text
Soft electric-piano-like keys with a little tremolo.
````

## keys-bell

````text
Glassy bell sound for a lullaby.
````

## keys-organ

````text
Simple organ: no envelope fade, notes stop the moment I let go.
````

## pluck-short

````text
Very short percussive pluck, like a marimba.
````

## pluck-delay

````text
Pluck with a ping-pong delay synced to the song.
````

## fx-riser

````text
A white-noise riser effect that sweeps up while I hold a note.
````

## fx-wind

````text
Wind sound made from filtered noise slowly moving.
````

## drone-generative

````text
A generative drone that plays by itself when the song plays, no MIDI needed.
````

## drone-sh

````text
Random sample-and-hold bleeps quantized to D dorian, playing by themselves.
````

## seq-berlin

````text
Berlin-school sequence: 16 steps of 1/16 notes in C minor with a filter sweep.
````

## seq-swing

````text
A swung 1/8 groove bassline in E minor driven by the clock.
````

## seq-triplet

````text
Triplet-feel arpeggio on its own sequencer, quantized to G major.
````

## seq-random

````text
Melodic random walk: random notes quantized to A minor pentatonic, one per 1/8.
````

## seq-poly

````text
Make a step-sequenced pluck in F minor and add reverb.
````

## edit-brighter

````text
Make this sound brighter and more present.
````

## edit-slow-attack

````text
Give it a slow fade-in attack.
````

## edit-add-delay

````text
Add a tempo-synced delay on the whole sound.
````

## edit-remove-reverb

````text
Remove the reverb, it's too washy.
````

## edit-macro

````text
Add a Cutoff macro and a Space macro I can automate.
````

## edit-detune

````text
Make it wider: add a second slightly detuned oscillator.
````

## neg-no-change

````text
What modules does the Modular Synth have? Just tell me, don't change anything.
````

## neg-vst-request

````text
Load Serum 2 on this track.
````
