# ⚙️ LLM-Daemon: System Tray Manager for local llama.cpp Servers

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build Status](https://img.shields.io/badge/Status-Stable-blue.svg)]([https://github.com/dark-side-droid/llm-daemon])
[![Platform](https://img.shields.io/badge/Platform-Ubuntu%2F_GTK-green)]([])
[![Version](https://img.shields.io/badge/version-1.0-brightgreen)]([])

## Overview

LLM-Daemon is a lightweight GTK system tray application for managing local `llama.cpp` servers. It provides a simple graphical interface for starting, stopping, configuring, and monitoring `llama-server` instances without using the terminal or creating scripts.

LLM-Daemon is designed for users who run local GGUF models and want:
- persistent background inference
- automatic crash recovery
- quick model switching
- server health monitoring
- desktop integration

The application acts as a management layer around `llama-server`; it does not perform inference itself. LM-Daemon does not replace `llama.cpp`. It launches and monitors an external `llama-server` process using your existing GGUF models.

![Screenshot](https://github.com/dark-side-droid/llm-daemon/blob/main/screenshot.jpg)

## Features
- Simple GTK system tray interface
- Start / Stop / Restart controls
- Settings dialog for configuring models and server options
- Automatic health checks
- Desktop notifications
- Optional auto-restart on crash
- Persistent configuration file
- Lightweight native C application

## What You Need Before Starting

This application does **not** include `llama.cpp`.

You must already have:
1. A working `llama.cpp` build
2. The `llama-server` executable and everything it requires.
3. At least one compatible model `.gguf` or otherwise compatible with llama.cpp

## Getting Started

### Dependencies
Ubuntu / Debian:
```bash
sudo apt update

sudo apt install -y \
    build-essential \
    pkg-config \
    libgtk-3-dev \
    libayatana-appindicator3-dev \
    libnotify-dev \
    libcurl4-openssl-dev
````
### Installation & Build

1.  **Clone the Repository:**
```bash
git clone https://github.com/dark-side-droid/llm-daemon.git
cd llm-daemon
```

3.  **Compile the Application:**
    Enter the folder and compile using the makefile:
```bash
make
```

4.  **Run Daemon:**
```bash
./llm-daemon
```
The application will initialize, load configuration, and appear in your system tray.

## Configuration

Daemon relies on a configuration file for all server-specific settings. This file controls the path to the server binary, the model, network settings, and context size. The file is located at :

```
~/.config/llm-daemon/config.ini
```

**Example `config.ini` structure:**

```ini
[llm-daemon]
version=1
server_bin=/home/USERNAME/AI_PROJECT/llama-server
model_path=/home/USERNAME/AI_PROJECT/models/model.gguf
host=127.0.0.1
port=8080
ctx_size=65000
threads=12
temperature=1
flash_attn=true
rea=auto
no_mmap=false
tools=false
no_warmup=false
no_webui=false
no_ctx_shift=true
ngl=auto
n_tokens=65000
top_k=40
top_p=0.94
min_p=0.05
cache_type_k=
cache_type_v=
```

Alternatively you can  modify these settings via the tray icon. Open the Daemon application, navigate to the **"Settings…"** menu item, and adjust the parameters before clicking **"Save"**.

## Usage Guide

### Starting a Server
After the settings have been configured you can simply: 

1.  Launch **Daemon** from your system tray.
2.  Click the **"Start"** menu item.
3.  Daemon will launch the LLM server executable defined in the config, set up the required environment variables, and begin monitoring its health.
4.  Once the HTTP readiness check succeeds, the status will update to **"Running"** (green icon).

### Stopping a Server

1.  Click the **"Stop"** menu item.
2.  Daemon will send a graceful `SIGTERM` signal to the running process.
3.  The process will shut down, and the status will revert to **"Stopped"**.

### Monitoring

*   **Status Icon:** The icon in your system tray will dynamically change:
    *   🟢 **Running:** Service is healthy and accepting requests. Displays the `emblem-ok-symbolic` icon.
    *   🟡 **Starting/Stopping:** Process is being managed. Displays the `system-run-symbolic` icon.
    *   🔴 **Stopped:** No server is currently active. Displays the `system-software-update-symbolic` icon.

## Common Problems

### Server instantly exits

Usually caused by:

* wrong model path
* incompatible model
* unsupported GPU settings
* missing CUDA / Vulkan libraries

Run `llama-server` manually in terminal first to confirm it works. This program is just a wrapper, not a debugger for `llama.cpp`.

### No tray icon appears

Install an indicator extension if using GNOME.

Ubuntu usually works automatically.

### Flash Attention errors

Disable it in Settings if startup fails.

