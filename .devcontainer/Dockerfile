FROM mcr.microsoft.com/devcontainers/base:ubuntu-22.04

ARG USERNAME=vscode

# Install system dependencies, toolchain packages, and serial support
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        curl \
        wget \
        python3 \
        python3-pip \
        python3-venv \
        python3-dev \
        libusb-1.0-0-dev \
        udev \
        clang-format \
        ca-certificates \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Add vscode user to dialout and plugdev groups for serial/USB access
RUN usermod -aG dialout,plugdev ${USERNAME}

# Install PlatformIO udev rules for serial devices
RUN curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
    -o /etc/udev/rules.d/99-platformio-udev.rules

# Switch to non-root user for PlatformIO installation
USER ${USERNAME}
ENV PATH="/home/${USERNAME}/.platformio/penv/bin:${PATH}"

# Install PlatformIO Core via official installer
RUN curl -fsSL -o /tmp/get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py \
    && python3 /tmp/get-platformio.py \
    && rm /tmp/get-platformio.py

WORKDIR /workspaces/BrewLink
