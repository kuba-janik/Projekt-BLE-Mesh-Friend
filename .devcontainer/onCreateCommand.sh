#!/bin/bash

set -euo pipefail

cd /repo

if [ -d ".west" ]; then
  echo "Using existing west workspace in current directory"
else
  west init -l app
fi

west update

cd deps/
west zephyr-export

west packages pip --install --ignore-venv-check -- --break-system-packages

if west sdk list >/dev/null 2>&1; then
  echo "Zephyr SDK already installed, skipping."
else
  west sdk install -t arm-zephyr-eabi
fi

west completion bash > "$HOME/west-completion.bash"

echo 'source $HOME/west-completion.bash' >> $HOME/.bashrc
