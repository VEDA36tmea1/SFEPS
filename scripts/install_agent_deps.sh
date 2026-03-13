#!/usr/bin/env bash
set -euo pipefail

PKGS=(
  build-essential
  cmake
  pkg-config
  git
  python3-venv
  python3-pip
  libmariadb-dev
  libtinyxml2-dev
  libavcodec-dev
  libavformat-dev
  libavutil-dev
  libswscale-dev
  libasound2-dev
  libssl-dev
  wkhtmltopdf
)

echo "install_agent_deps: detect package manager"
if command -v apt-get >/dev/null 2>&1; then
  PM=apt
elif command -v dnf >/dev/null 2>&1; then
  PM=dnf
elif command -v yum >/dev/null 2>&1; then
  PM=yum
else
  echo "Unsupported package manager. Please install dependencies manually: ${PKGS[*]}" >&2
  exit 1
fi

install_with_sudo() {
  if [ "$PM" = "apt" ]; then
    sudo apt-get update -y
    sudo apt-get install -y "${PKGS[@]}"
  else
    # dnf/yum
    sudo ${PM} install -y "${PKGS[@]}"
  fi
}

if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    echo "install_agent_deps: installing with sudo"
    install_with_sudo
  else
    echo "install_agent_deps: please run as root or install sudo and re-run this script" >&2
    exit 1
  fi
else
  echo "install_agent_deps: running as root"
  if [ "$PM" = "apt" ]; then
    apt-get update -y
    apt-get install -y "${PKGS[@]}"
  else
    ${PM} install -y "${PKGS[@]}"
  fi
fi

echo "install_agent_deps: done"
