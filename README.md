# ⚙️ LLM-Daemon: LLM Server Tray Manager

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build Status](https://img.shields.io/badge/Status-Stable-blue.svg)]([https://github.com/dark-side-droid/llm-daemon])
[![Platform](https://img.shields.io/badge/Platform-Debian_%2F_Ubuntu-green)]([])
[![Version](https://img.shields.io/badge/version-1.0-brightgreen)]([])

## Overview

**LLM-Daemon** is a lightweight, system-tray application designed to manage the lifecycle and of locally hosted Large Language Model servers. It provides a central, persistent interface for starting, stopping, configuring, and monitoring AI model services, ensuring that your local LLM operations are reliable and responsive.

LLM-Daemon lets you:
- start and stop `llama-server` without touching terminal.
- manage your model configuration
- monitor server status
- automatically restart crashed servers
- keep your local AI server running in the background.

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
2. The `llama-server` executable
3. At least one `.gguf` model file

## Getting Started

### Dependecies
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
    git clone https://github.com/YourUsername/Daemon.git
    cd Daemon
    ```

2.  **Compile the Application:**
    Compile using the following command:
    ```bash
    gcc llm-daemon.c -o llm-daemon \
        $(pkg-config --cflags --libs gtk+-3.0 ayatana-appindicator3-0.1 libnotify) \
        $(curl-config --cflags --libs)
    ```

3.  **Run Daemon:**
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
server_bin=/home/USERNAME/llama-b9033-bin-ubuntu-vulkan-x64/bin/llama-server
model_path=/home/USERNAME/llama-b9033-bin-ubuntu-vulkan-x64/bin/models/model.gguf
host=127.0.0.1
port=8080
ctx_size=0
threads=1
temperature=1
flash_attn=true
```

Alternatively you can  modify these settings via the tray icon. Open the Daemon application, navigate to the **"Settings…"** menu item, and adjust the parameters before clicking **"Save"**.

You must configure:

| Setting         | Description                |
| --------------- | -------------------------- |
| Server binary   | Path to `llama-server`     |
| Model path      | Path to your `.gguf` model |
| Host            | Usually `127.0.0.1` or `0.0.0.0`      |
| Port            | Usually `8080`             |
| Context size    | Example: `4096`            |
| Threads         | CPU threads to use         |
| Temperature     | Model creativity           |
| Flash Attention | Optional acceleration      |

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

