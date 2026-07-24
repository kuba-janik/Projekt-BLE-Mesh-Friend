#!/bin/bash
set -e
NRFUTIL_URL="https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/x86_64-unknown-linux-gnu/nrfutil"
export DEBIAN_FRONTEND=noninteractive
export TZ=Europe/Warsaw

USERNAME="${USERNAME:-"${_REMOTE_USER:-"automatic"}"}"

if [ "${USERNAME}" = "auto" ] || [ "${USERNAME}" = "automatic" ]; then
    USERNAME=""
    POSSIBLE_USERS=("vscode" "node" "codespace" "$(awk -v val=1000 -F ":" '$3==val{print $1}' /etc/passwd)")
    for CURRENT_USER in "${POSSIBLE_USERS[@]}"; do
        if id -u "${CURRENT_USER}" >/dev/null 2>&1; then
            USERNAME=${CURRENT_USER}
            break
        fi
    done
    if [ "${USERNAME}" = "" ]; then
        USERNAME=NON_ROOT
    fi
elif [ "${USERNAME}" = "none" ] || ! id -u "${USERNAME}" >/dev/null 2>&1; then
    USERNAME=root
fi

export USERNAME_VAR="${USERNAME}"

if [ "${USERNAME}" = "root" ]; then
    USER_HOME="/root"
else
    USER_HOME="/home/${USERNAME}"
fi

PYTHON_DIR=$(find /usr/local/python/ -maxdepth 1 -type d -name "[0-9]*.*" | sort -V | tail -n 1)
export PATH="${PYTHON_DIR}/bin:$PATH"

sudo -u "${USERNAME}" bash -c "export PATH=${PATH}; pip3 install --user -U west"
sudo -u "${USERNAME}" bash -c 'echo "export PATH=\$HOME/.local/bin:\$PATH" >> ~/.bashrc'
sudo -u "${USERNAME}" bash -c 'source ~/.bashrc'
WEST_BIN="${USER_HOME}/.local/bin/west"
if [ ! -x "${WEST_BIN}" ]; then
  echo "west binary not found at ${WEST_BIN}"
  exit 1
fi
sudo -u "${USERNAME}" bash -c "export PATH=${PATH}; pip3 install --user -U pre-commit"


if [ -n "${NRFUTIL_URL:-}" ]; then
  tmp="$(mktemp -d)"
  cd "$tmp"

  curl -fSL "$NRFUTIL_URL" -o nrfutil || {
    echo "Download error"
    exit 1
  }

  chmod +x nrfutil
  mv nrfutil /usr/local/bin/nrfutil

  nrfutil self-upgrade
  nrfutil install device
  nrfutil install completion
  nrfutil install trace

  cd - >/dev/null
  rm -rf "$tmp"
else
  echo "Skipping nrfutil installation (URL not provided)"
  exit 1
fi
