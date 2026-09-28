#!/usr/bin/env bash
set -euo pipefail

cd /runner

if [[ "${1:-}" == "configure" ]]; then
    if [[ -f .runner ]]; then
        echo "Runner is already configured."
        exit 0
    fi

    IFS= read -r registration_token || {
        echo "A registration token must be provided on stdin." >&2
        exit 1
    }

    exec runuser --user runner -- ./config.sh \
        --url "${RUNNER_URL:?RUNNER_URL is required}" \
        --token "$registration_token" \
        --name "${RUNNER_NAME:-ubuntu}" \
        --labels "${RUNNER_LABELS:-ubuntu}" \
        --work _work \
        --unattended \
        --replace
fi

if [[ ! -f .runner ]]; then
    echo "Runner is not configured. Run the configure command first." >&2
    exit 1
fi

rm -f /var/run/docker.sock /var/run/docker.pid
dockerd \
    --host=unix:///var/run/docker.sock \
    --data-root=/var/lib/docker \
    --storage-driver=overlay2 \
    --iptables=true \
    --log-level=warn &
docker_pid=$!

shutdown() {
    if [[ -n "${runner_pid:-}" ]] && kill -0 "$runner_pid" 2>/dev/null; then
        kill -TERM "$runner_pid" 2>/dev/null || true
        wait "$runner_pid" 2>/dev/null || true
    fi
    if kill -0 "$docker_pid" 2>/dev/null; then
        kill -TERM "$docker_pid" 2>/dev/null || true
        wait "$docker_pid" 2>/dev/null || true
    fi
}
trap shutdown TERM INT

for attempt in {1..120}; do
    if docker info >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "$docker_pid" 2>/dev/null; then
        echo "Docker daemon exited before becoming ready." >&2
        exit 1
    fi
    if (( attempt % 30 == 0 )); then
        echo "Waiting for Docker daemon (${attempt}/120 seconds)..."
    fi
    sleep 1
done

if ! docker info >/dev/null 2>&1; then
    echo "Docker daemon did not become ready within 120 seconds." >&2
    exit 1
fi

runuser --user runner -- env \
    "ACTIONS_RUNNER_HOOK_JOB_COMPLETED=${ACTIONS_RUNNER_HOOK_JOB_COMPLETED:?ACTIONS_RUNNER_HOOK_JOB_COMPLETED is required}" \
    ./run.sh &
runner_pid=$!
set +e
wait "$runner_pid"
status=$?
set -e
shutdown
exit "$status"
