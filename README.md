# Road Alert Broadcasting Platform

A platform for broadcasting road traffic alerts to information panels, built in **C on the Windows API** around inter-process communication.
Developed for **Operating Systems 2** at ISEC - Polytechnic University of Coimbra (2025/26).

## Architecture

The system is made of three independent programs:

| Program | Role |
|---|---|
| `central` | Manages the system: receives, stores and distributes active alerts |
| `placar` | An information panel that shows the alerts it receives (several can run at once) |
| `monitor` | Win32 GUI that shows every active alert in real time |

## Features

- Communication between processes through **named pipes**
- **Shared memory** for the alert list, synchronised with **mutexes and events**
- Multithreaded programs, with immediate updates and no polling
- Win32 monitor with a File menu (Settings / About / Exit), an alert list (ID, message, duration) and paging with Page Up / Page Down

## Tech

C, Windows API (Win32), named pipes, shared memory, threads, mutexes, events

## Build (Windows + MinGW)

```
gcc -municode -DUNICODE -D_UNICODE -o central.exe central.c
gcc -municode -DUNICODE -D_UNICODE -o placar.exe placar.c
gcc -municode -DUNICODE -D_UNICODE -o monitor.exe monitor.c -lcomctl32 -lgdi32
```

## Run

Run each program in its own console, in this order:

1. `central`
2. one or more `placar`
3. `monitor` (optional)

## Authors

- Rodrigo Costa ([@rodrigo2024134442](https://github.com/rodrigo2024134442))
- Joao Marques ([@joao16marques](https://github.com/joao16marques))