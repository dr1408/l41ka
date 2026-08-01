<p align="center">
  <img src="https://github.com/0cyn/l41ka/blob/public/.github/img/laika.png?raw=true" alt="l41ka app image"/>
</p>

<p align="center">| <a href="https://github.com/0cyn/l41ka-app">Mac/Linux/Windows Controller App</a> | <a href="https://github.com/0cyn/l41ka/releases/latest">Firmware Download</a> | </p>

<p align="center">l41ka is an rpi firmware and iOS bootkit aimed at
iOS security/low-level research, developed for the sake of learning more about these SOCs at a low level. </p>


---

l41ka is capable of (independently!) loading A12/A13 devices to pongoOS, and of proxying file uploads from host if needed
at every stage. l41ka is not a full end-user jailbreak.

it can also be driven over usb to stop after independent stages, allowing you to do your security researchy things at
given breakpoints. this is the primary case this tool is developed for.

it embeds an updated build of PongoOS that adds support for 8020/8030 SOCs. 

"Tested on" SOC + iBoot Version matrix:

| soc  | iOS 18.7.9 | iOS 26.5 |
|------|------------|----------|
| 8020 | yes        |          |
| 8030 |            | yes      |

8027 support is being worked on currently ^..^

Boards this was tested on:

| board              | works? |
|--------------------|--------|
| Waveshare RP2350-A | yes    |
|                    |        |

If you're able to test, file an issue and let me know the state of things with your device/board. 

please expect this early version to be buggy, and please file issues when you hit problems. please work with me and
be understanding that this was written primarily by a single person.

the usb code for laikaDFU, usb wrappers, etc is not great. this isn't my audition for OS/driver development :sweat_smile: 

---

### build

Please reference the .github/workflows/build.yml for build instructions currently.

this will generate an elf or uf2 you can flash to the pi.

nothing at the moment requires an aarch64 or os/specific build host (pongo build maybe) but for now the only 'officially' supported
build env is Apple Silicon macOS. 

``` shell
# If you are updating the firmware from a previous l41ka fw, run this first
python3 scripts/laikadbg.py bootsel # app must be closed
# flash.sh does this 

# then:
picotool load -t elf "$elf" -x -f # automatically puts device in bootsel
```

---

## control

the main firmware is driven via a custom USB stack.

we also send debug spam over CDC if that interests you.

eventually there will be a `LAIKA_FIRMWARE_AUTORUN` mode but unironically that is a bit too user friendly for right now. 

### the app

https://github.com/0cyn/l41ka-app

This is how this project is intended to be used. This project implements the host-end of l41ka comms and drives things
nicely.

The project itself is structured with a core API wrapped by UI; it's fully capable of being turned into a cli tool or
integrated into other crap (be aware of this project's license, please)

### i am hacking on this project and want a worse tool to use

there is also a pyusb python script that can talk to the raspi.

```
python3 -m pip install pyusb pyserial
python3 scripts/laikadbg.py -h
# Run the CDC debug stream separately from control commands.
python3 scripts/laikadbg.py --cdc &
```

i'm not writing full docs for this at this time :p you will figure it out I believe in You.

#### direct pwned-dfu usb client

`scripts/usb_handlerctl.py` exists if you need to plug a phone into your pc and directly interact with the r/w/x handler
we install in DFU.

---

current pi board support:  
waveshare 2350A

i haven't tested for any other board config. 

it is likely not that difficult as long as it supports usbliter8.

---

pongo stuff:  
I have an updated fork, however I have not been around for the 4 years of m1n1 re and it appears many people know
much more than i do about DART, etc 😅, if you are one of those people and wouldn't mind taking a look at how 
badly I integrated it <3

---

## Code Licensing

regarding the portions of this code I have ownership over (anything with this comment at the top):

Copyright (c) 0cyn All Rights Reserved.

Individual files may be licensed differently at discretion. 

Please contact me for licensing. Generally, if you have me
on discord or elsewhere, and are not trying to sell stuff, we'll be fine; this is related to
vibe-forks and I am not trying to charge for this code or something.

This project in laikaDFU vendors a file from m1n1; this file specifically is licensed as MIT:  
ch9.h 

The pongoOS fork in this project vendors 2 files from m1n1; the dart driver
is pulled from that project and adapted slightly to work with pongoOS:
dart.c
dart.h

These files exclusively are available under the m1n1 license viewable here:

https://github.com/AsahiLinux/m1n1/blob/main/LICENSE


The license information for the dart driver is also included in pongoOS. 

#### PongoOS

Our build of pongoOS is subject to its own license, outside of this project
, as are things that it licenses. 

You should defer to the pongoOS fork for licensing related to that code:

https://github.com/0cyn/PongoOS/blob/t8030/LICENSE.md

---

creds

tbd: DWC2 haxx  
KJC: PongoOS, kpf  

ty:  
jpm: [usb handler magic](https://github.com/jonpalmisc/respawn)  
axi0mX: ipwndfu was a huge learning experience for implementing  
siguza: patience w/ questions  
air conditioning when it works  
chicago


i wrote this primarily as a research/learning experience. there are some harebrained things that
prior art handles significantly better but i wanted to rediscover the flow of doing something
like this from first principles, instead of just REing existing stuff and brainlessly reshipping/offset-tweaking
payloads.

this project has a lot of homerolled usb and should not be treated as a security boundary ^..^

---

<p align="center"><img width="256" height="256" alt="1257553414393696266" src="https://github.com/user-attachments/assets/6ff8938a-c621-4c20-92f1-8fbdb02e6d57" /></p>
<p align="center"><i>for dani</i></p>
