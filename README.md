# AION2 Network Overlay

An unofficial Windows overlay and reconnect workaround for lag in individual AION2 instances. It shows the server IP, the current socket, the longest recent gap between incoming game data, TCP ping, and two minutes of receive-gap history.

## When Reconnect helps

If an instance feels consistently laggy or has high latency, click **Reconnect** when it is safe to do so. The overlay closes the current game connection so AION2 can open a fresh one. In my testing over several days, every reconnect I tried during this kind of lag improved responsiveness immediately. I stayed in the same activity each time, including dungeons, Shugo, and Abyss. AION2 controls the reconnect, so those results describe my own sessions.

Multi-second interruptions in server updates can recur after a reconnect. Fixing the underlying server issue requires a change from NCSOFT.

## Run the .exe directly

Download the file **AION2 Network Overlay.exe**, save it anywhere you want and double-click **AION2 Network Overlay.exe**. Approve the Windows administrator prompt. Windows 10 and 11 x64 need no separate runtime or installer.

**Receive Gap** shows a pause between incoming game data. **TCP Ping** is the Windows TCP round-trip time. It can stay low when game updates stall. **Socket** shows the local port, and **Connected** resets when AION2 opens a new connection, even on the same server IP.

Use **BG** to toggle the background. The **−** button collapses the overlay to a small **+** button that restores it.

## Build from source

1. Install [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) with **Desktop development with C++**, including MSVC and a Windows SDK.
2. Open an **x64 Native Tools Command Prompt** for Visual Studio and change into this repository.
3. Run `build.cmd`. The executable will be in `build\AION2 Network Overlay.exe`.

The build uses the Microsoft compiler and Windows libraries. It downloads no application dependencies.

## Permissions and data

The [application manifest](NativeOverlay.manifest) requests administrator rights for live Windows network events and the ability to close the selected TCP connection. The [source](NativeOverlay.cpp) reads timing and endpoint metadata for the AION2 connection. **Reconnect** rechecks that the socket still matches before closing it.

The overlay keeps recent timings in memory and saves only window position, size, and background preference in your Windows profile. It does not read packet contents, save traffic logs, or upload data.

I investigated the network behavior as an AION2 player and used Codex to write the code. This project is independent of NCSOFT and is released under the [MIT License](LICENSE).
