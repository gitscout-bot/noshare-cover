FROM ubuntu:24.04

ARG RUNNER_VERSION=2.337.0
ARG RUNNER_SHA256=70920811a4f8ad4328818682bca5c6469c1c942fab52448868071d0063816613

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        docker.io \
        git \
        jq \
        sudo \
        tar \
        gzip \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --shell /bin/bash runner \
    && (getent group docker >/dev/null || groupadd docker) \
    && usermod -aG docker runner \
    && printf 'runner ALL=(ALL) NOPASSWD:ALL\n' > /etc/sudoers.d/runner \
    && chmod 0440 /etc/sudoers.d/runner

RUN mkdir -p /runner \
    && curl --fail --location --retry 3 \
        "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz" \
        --output /tmp/actions-runner.tar.gz \
    && echo "${RUNNER_SHA256}  /tmp/actions-runner.tar.gz" | sha256sum --check --status \
    && tar -xzf /tmp/actions-runner.tar.gz -C /runner \
    && rm /tmp/actions-runner.tar.gz \
    && cd /runner \
    && ./bin/installdependencies.sh \
    && chown -R runner:runner /runner

COPY runner/entrypoint.sh /usr/local/bin/runner-entrypoint
RUN chmod 0755 /usr/local/bin/runner-entrypoint

WORKDIR /runner
ENTRYPOINT ["/usr/local/bin/runner-entrypoint"]
