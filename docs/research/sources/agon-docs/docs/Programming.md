# Programming your Agon computer

Whilst there is a lot of great software you can download and run on your Agon, you can write your own software too. And this is probably the reason you bought your Agon!

There are several programming languages available, something for everyone, from beginner to expert... or those just reliving their 1980's memories.

Some of the languages run directly on your Agon computer. Some run on your Windows, Mac or Linux PC for transfer to your Agon. Plus, there are some which your can run both on the Agon and your PC.

Most of the languages available allow you to code in traditional 16bit or 24bit _ADL mode_, where the full amount of memory is available, not limited to 64k.

In order to help speed up development, there is also an emulator so your can program and test _most_ code quickly on your PC, before transferring to the Agon.

It doesn't matter which model you own from the Agon platform range of computers, inside they are all the same and can run exactly the same software. The only thing which might make your software be less compatible is if you utilise features from the latest OS builds where another user may not have updated their system. If you intend on sharing your programs, make sure you specifiy the minimum system of MOS/VDP required.

All languages listed here come with examples to help you get started.



## The Languages

### BBC BASIC

This popular language is installed by many Agon owners and is a great way to quickly start programming your Agon computer.  Many of the examples in this documentation for the VDP's VDU commands are written in BBC BASIC.

The Z80 version of BBC BASIC was first created by Richard Russell for Acorn's Z80 second processor for the BBC Micro in the 1980's.  It was also available on CP/M systems, and has since spawned [many other versions](https://www.rtrussell.co.uk/).  The Z80 BBC BASIC was designed to be compatible with Acorn's 6502-based BBC BASIC versions 2, 3 and 4 for the original BBC Micro model B, B+ and Master series of computers.  It is almost completely compatible with the BBC Master's BBC BASIC 4, with only some very minor exceptions.

The original source for the 1987 BBC Micro Z80 second processor BBC BASIC was made open source by Richard Russell several years ago, and was ported to the Agon by Dean Belfield as part of the original Quark firmware.

The Agon ports of BBC BASIC provide a familiar yet powerful language used by beginners and experts alike. It runs directly on the Agon computer and is often started automatically by many users with a command in their [system boot script](./MOS.md#boot-script).

There are two versions of BBC BASIC available for the Agon, based on the original Z80 version.  Firstly there is the [pure Z80 version](https://github.com/breakintoprogram/agon-bbc-basic), which runs in Z80 mode and is limited to 64k of memory.  Secondly there is an [ADL version](https://github.com/breakintoprogram/agon-bbc-basic-adl), which has been converted to use the Agon eZ80's ADL mode.  This allows it to both access all 512kb of available memory on your Agon, and the inbuilt assembler has been extended to support the eZ80's new instructions.

The latest release of the Z80 version of BBC BASIC can be [downloaded here](https://github.com/breakintoprogram/agon-bbc-basic/releases/latest), and the ADL version can be [downloaded here](https://github.com/breakintoprogram/agon-bbc-basic-adl/releases/latest).

In late 2024 Richard Russell revisited his Z80 BBC BASIC and updated it to include many new features that Acorn added to BBC BASIC V for the Acorn Archimedes and RISC OS, which many of his other versions of BBC BASIC already supported.  This includes support for new structured programming constructs, as well as several other new keywords to simplify programming.

[BASIC V](https://github.com/breakintoprogram/agon-bbc-basic-v) has also been ported to run on the Agon by Dean Belfield.  At this time it only runs in Z80 mode and so is limited to using only 64kb memory.  It can be [downloaded here](https://github.com/breakintoprogram/agon-bbc-basic-v/releases/latest).


### Z80 Assembly

If you have ever written software in z80 Assembly for the Sinclair ZX81, ZX Spectrum, MSX, Amstrad CPC, TRS-80, Nintendo Game Boy, or even the Texas Instruments calulators, then this will be familliar territory for you.

As well as the assembler built into BBC BASIC, there is also a dedicated assembler available in the form of [`ez80asm`](https://github.com/AgonPlatform/agon-ez80asm).

You can write your code in whatever text editor you like, then assmble your source code with `ez80asm`.  The assembler can be run natively on the Agon itself, or on Mac, Windows, Linux, and even Raspberry Pi.

The latest version of this assembler can be [downloaded here](https://github.com/AgonPlatform/agon-ez80asm/releases/latest).


### C or C++

Many programmers enjoy coding in C or C++. This language is also available and there are a few options for coding on your desktop PC, then compiling and transferring onto your Agon to run.

[AgonDev](https://github.com/AgonPlatform/agondev) is a modern development toolchain for the Agon based on LLVM and the GNU AS assembler.  Releases are available for ARM-based Macs, and for ARM and x86_64 Linux systems.  Windows users can run AgonDev under WSL (Windows Subsystem for Linux).

AgonDev is still in active development and is getting more advanced every day.

The latest version of AgonDev can be [downloaded here](https://github.com/AgonPlatform/agondev/releases/latest).


### B Simple

B was a language which, unsurpisingly, was around before C, and was based on the earlier BCPL.

[B Simple](https://github.com/nihirash/bsimple-ez80-compiler) is inspired by the original B language.  It is a general purposes typeless compiled language that should be familiar to C programmers.

B Simple's approach is that of a cross-compiler, converting your B Simple source code first into eZ80 assembly language and then assembling that using `ez80asm` into a binary executable that can run on your Agon computer. 

Whilst B Simple has a limited set of variable types, it is fast and easy to use. Plus, it can easily be expanded by writing custom libraries in z80 assembler.

B Simple is available to run on your Agon computer directly, as well as on Mac, Windows or Linux computers.  The latest version can be [downloaded here](https://github.com/nihirash/bsimple-ez80-compiler/releases/latest).

### Forth

The [Forth programming language](https://en.wikipedia.org/wiki/Forth_(programming_language)) is a stack-based language which is quite different from most other programming languages.  In the early days of home computing it was quite popular on some niche systems, and was used as the operating system on the [Jupiter ACE](https://en.wikipedia.org/wiki/Jupiter_Ace).

It's different approach to programming appeals to many programmers, and there is a dedicated community of Forth programmers around the world.  Forth programs can be very efficient, and the compact nature of the core language makes it ideal as the first language for new computer systems.

[Agon Forth](https://github.com/lennart-benschop/agon-forth) runs on the Agon itself and provides a great way to learn the language.  It has versions available for both Z80 and eZ80 modes, and includes several libraries and example programs to help you get started.

## The Emulator

If you create your programs on your PC (or Mac/Linux), the Emulator enables you to test them right there, before downloading to your Agon.
There are some limitations, such as using additional hardware (eg. i2c devices, or io ports) but most other features are fully functional, and is updated regularly to keep in line with the latest Agon MOS/VDP operating system.

Download the latest version of [The Agon Emulator](https://github.com/tomm/fab-agon-emulator)

## Getting your code onto the Agon

If you have been building your Agon program on a PC (or Mac/Linux), there is a tool available called `Hexload` to help transfer the binary over to your Agon.

Further information and download [Hexload](https://github.com/AgonPlatform/agon-hexload)


Good luck with your programming. There is lots of community help and support available on the [Agon & Console8 Community Discord server](https://discord.com/invite/sN3vXru26s)

There are several getting started guides on the internet too, as well as YouTube videos, such as those from [AgonBits](https://www.youtube.com/@AgonBits)


