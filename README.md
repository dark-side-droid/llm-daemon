# ⚙️ LLM-Daemon: LLM Server Tray Manager

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build Status](https://img.shields.io/badge/Status-Stable-blue.svg)]([https://github.com/dark-side-droid/llm-daemon])
[![Platform](https://img.shields.io/badge/Platform-Linux%20%2B%20Debian-green.svg)]([])

## Overview

**LLM-Daemon** is a lightweight, system-tray application designed to manage the lifecycle and of locally hosted Large Language Model servers. It provides a central, persistent interface for starting, stopping, configuring, and monitoring AI model services, ensuring that your local LLM operations are reliable and responsive.

Daemon acts as a convenient supervisor, handling process management, periodic health checks, and real-time desktop notifications, making it the perfect tool for local AI development.

## Features

Daemon provides a complete solution for managing your LLM infrastructure:

*   **Process Control:** Seamlessly start and gracefully stop the LLM server process.
*   **Instant Crash Detection:** Utilizes `g_child_watch_add` for immediate detection and reporting if the spawned server process exits unexpectedly.
*   **HTTP Readiness Check:** Periodically polls the server's API endpoint to confirm that the service is actively running and ready to accept requests.
*   **Dynamic System Tray:** Runs as a persistent application in your system tray, providing easy access to management controls.
*   **Real-time Notifications:** Uses `libnotify` to send desktop alerts for status changes (Ready, Crash, Timeout).
*   **Configurable Settings:** Allows users to easily adjust crucial parameters (Model Path, Host, Port, Context Size) via a user-friendly settings dialog.
*   **Persistence:** Configuration settings are saved to a persistent `config.ini` file.

## Getting Started

### Prerequisites

Before building and running Daemon, ensure you have the necessary development tools and runtime dependencies installed on your system.

*   **Compiler:** GCC (required for compilation)
*   **Dependencies (Runtime):** `libayatana-appindicator3-1`, `libcurl`, `libnotify`
*   **Build Tools:** `pkg-config`

### Installation & Build

1.  **Clone the Repository:**
    ```bash
    git clone https://github.com/YourUsername/Daemon.git
    cd Daemon
    ```

2.  **Compile the Application:**
    Use the provided `Makefile` or compile directly using the following command:
    ```bash
    gcc tray_app.c -o daemon \
        $(pkg-config --cflags --libs gtk+-3.0 ayatana-appindicator3-0.1 libnotify) \
        $(curl-config --cflags --libs)
    ```

3.  **Run Daemon:**
    ```bash
    ./daemon
    ```
    The application will initialize, load configuration, and appear in your system tray.

## Configuration

Daemon relies on a configuration file for all server-specific settings.

### Configuration File

The main configuration file is located at:
```
~/.config/trayapp/config.ini
```

This file controls the path to the server binary, the model, network settings, and context size.

**Example `config.ini` structure:**

```ini
[server]
bin_dir = /home/gilgamesh/Projects/AI Tests/llama-b9033-bin-ubuntu-vulkan-x64/llama-b9033/bin
server_bin = /llama-server
model_path = /models/Qwen3.6-35B-A3B-Uncensored-HauhauCS-Aggressive-Q6_K_P.gguf
host = 127.0.0.1
port = 8080
ctx_size = 4096
```

### Settings Dialog

To modify these settings, open the Daemon application, navigate to the **"Settings…"** menu item, and adjust the parameters before clicking **"Save"**.

## Usage Guide

### Starting a Server

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
    *   🟢 **Running:** Service is healthy and accepting requests. Displays the `emblem-ok` icon.
    *   🟡 **Starting/Stopping:** Process is being managed. Displays the `emblem-synchronizing` icon.
    *   🔴 **Stopped:** No server is currently active. Displays the `emblem-readonly` icon.

## 🛠️ Technical Details

| Component | Technology / Library | Purpose |
| :--- | :--- | :--- |
| **Language** | C | Core application logic. |
| **GUI** | GTK+ 3.0 | Building the system tray UI and settings dialog. |
| **Process Mgmt** | `g_spawn_async`, `g_child_watch_add` | Spawning the server and monitoring for crashes. |
| **HTTP Checks** | `libcurl` | Performing periodic readiness probes (HEAD requests). |
| **Configuration** | `GKeyFile` | Reading and writing settings from `~/.config/trayapp/config.ini`. |
| **Notifications** | `libnotify` | Displaying status updates to the desktop. |
